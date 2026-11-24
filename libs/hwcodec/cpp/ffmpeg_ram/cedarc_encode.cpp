#include "cedarc_encode.h"
#include "cedarc_abi.h"
#include "cedarc_common.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

#include "common.h"

#define LOG_MODULE "CEDARC_ENC"
#include <log.h>

extern "C" int ffmpeg_ram_get_linesize_offset_length(int pix_fmt, int width,
                                                     int height, int align,
                                                     int *linesize, int *offset,
                                                     int *length);

namespace {

using namespace cedarc_abi;

using FnVideoEncCreate = VideoEncoder *(*)(VENC_CODEC_TYPE);
using FnVideoEncDestroy = void (*)(VideoEncoder *);
using FnVideoEncInit = int (*)(VideoEncoder *, VencBaseConfig *);
using FnAllocInputBuffer = int (*)(VideoEncoder *, VencAllocateBufferParam *);
using FnGetOneAllocInputBuffer = int (*)(VideoEncoder *, VencInputBuffer *);
using FnFlushCacheAllocInputBuffer = int (*)(VideoEncoder *, VencInputBuffer *);
using FnReturnOneAllocInputBuffer = int (*)(VideoEncoder *, VencInputBuffer *);
using FnReleaseAllocInputBuffer = int (*)(VideoEncoder *);
using FnAddOneInputBuffer = int (*)(VideoEncoder *, VencInputBuffer *);
using FnVideoEncodeOneFrame = int (*)(VideoEncoder *);
using FnAlreadyUsedInputBuffer = int (*)(VideoEncoder *, VencInputBuffer *);
using FnGetOneBitstreamFrame = int (*)(VideoEncoder *, VencOutputBuffer *);
using FnFreeOneBitStreamFrame = int (*)(VideoEncoder *, VencOutputBuffer *);
using FnVideoEncGetParameter = int (*)(VideoEncoder *, VENC_INDEXTYPE, void *);
using FnVideoEncSetParameter = int (*)(VideoEncoder *, VENC_INDEXTYPE, void *);

struct CedarcApi {
  void *handle = nullptr;
  bool loaded = false;
  FnVideoEncCreate VideoEncCreate = nullptr;
  FnVideoEncDestroy VideoEncDestroy = nullptr;
  FnVideoEncInit VideoEncInit = nullptr;
  FnAllocInputBuffer AllocInputBuffer = nullptr;
  FnGetOneAllocInputBuffer GetOneAllocInputBuffer = nullptr;
  FnFlushCacheAllocInputBuffer FlushCacheAllocInputBuffer = nullptr;
  FnReturnOneAllocInputBuffer ReturnOneAllocInputBuffer = nullptr;
  FnReleaseAllocInputBuffer ReleaseAllocInputBuffer = nullptr;
  FnAddOneInputBuffer AddOneInputBuffer = nullptr;
  FnVideoEncodeOneFrame VideoEncodeOneFrame = nullptr;
  FnAlreadyUsedInputBuffer AlreadyUsedInputBuffer = nullptr;
  FnGetOneBitstreamFrame GetOneBitstreamFrame = nullptr;
  FnFreeOneBitStreamFrame FreeOneBitStreamFrame = nullptr;
  FnVideoEncGetParameter VideoEncGetParameter = nullptr;
  FnVideoEncSetParameter VideoEncSetParameter = nullptr;
};

static CedarcApi g_api;
static std::once_flag g_api_once;
static thread_local std::string g_last_error;

static void set_last_error(const std::string &msg) { g_last_error = msg; }

static bool load_cedarc_api() {
  std::call_once(g_api_once, []() {
    void *h = cedarc_common::dlopen_lib("libvencoder.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) {
      LOG_TRACE(std::string("CedarC: libvencoder.so not found"));
      return;
    }

#define LOAD_SYM(sym)                                                          \
  do {                                                                         \
    g_api.sym = reinterpret_cast<Fn##sym>(dlsym(h, #sym));                     \
    if (!g_api.sym) {                                                          \
      LOG_WARN(std::string("CedarC: missing symbol ") + #sym);                 \
      dlclose(h);                                                              \
      return;                                                                  \
    }                                                                          \
  } while (0)

    LOAD_SYM(VideoEncCreate);
    LOAD_SYM(VideoEncDestroy);
    LOAD_SYM(VideoEncInit);
    LOAD_SYM(AllocInputBuffer);
    LOAD_SYM(GetOneAllocInputBuffer);
    LOAD_SYM(FlushCacheAllocInputBuffer);
    LOAD_SYM(ReturnOneAllocInputBuffer);
    LOAD_SYM(ReleaseAllocInputBuffer);
    LOAD_SYM(AddOneInputBuffer);
    LOAD_SYM(VideoEncodeOneFrame);
    LOAD_SYM(AlreadyUsedInputBuffer);
    LOAD_SYM(GetOneBitstreamFrame);
    LOAD_SYM(FreeOneBitStreamFrame);
    LOAD_SYM(VideoEncGetParameter);
    LOAD_SYM(VideoEncSetParameter);
#undef LOAD_SYM

    g_api.handle = h;
    g_api.loaded = true;
    LOG_DEBUG(std::string("CedarC: loaded libvencoder.so successfully"));
  });
  return g_api.loaded;
}

static bool map_pixel_format(int av_pixfmt, VENC_PIXEL_FMT *out_fmt,
                             bool *out_is_packed_422,
                             bool *out_is_planar_420) {
  *out_is_packed_422 = false;
  *out_is_planar_420 = false;
  switch (av_pixfmt) {
  case AV_PIX_FMT_NV12:
    *out_fmt = VENC_PIXEL_YUV420SP;
    return true;
  case AV_PIX_FMT_NV21:
    *out_fmt = VENC_PIXEL_YVU420SP;
    return true;
  case AV_PIX_FMT_YUV420P:
    *out_fmt = VENC_PIXEL_YUV420P;
    *out_is_planar_420 = true;
    return true;
  case AV_PIX_FMT_YUYV422:
    *out_fmt = VENC_PIXEL_YUYV422;
    *out_is_packed_422 = true;
    return true;
  case AV_PIX_FMT_UYVY422:
    *out_fmt = VENC_PIXEL_UYVY422;
    *out_is_packed_422 = true;
    return true;
  case AV_PIX_FMT_YVYU422:
    *out_fmt = VENC_PIXEL_YVYU422;
    *out_is_packed_422 = true;
    return true;
  default:
    return false;
  }
}

class CedarcEncoder {
public:
  std::string name_;
  VENC_CODEC_TYPE codec_type_ = VENC_CODEC_H264;
  VENC_PIXEL_FMT venc_pixfmt_ = VENC_PIXEL_YUV420SP;
  bool is_packed_422_ = false;
  bool is_planar_420_ = false;
  int av_pixfmt_ = AV_PIX_FMT_NV12;
  int width_ = 0;
  int height_ = 0;
  int aligned_w_ = 0;
  int aligned_h_ = 0;
  int align_ = 1;
  int fps_ = 30;
  int gop_ = 30;
  int kbs_ = 2000;
  CedarcEncodeCallback callback_ = nullptr;

  VideoEncoder *enc_ = nullptr;
  bool input_buffers_allocated_ = false;
  bool force_keyframe_ = false;
  std::vector<uint8_t> header_;
  std::vector<uint8_t> staging_buf_;

  CedarcEncoder(const char *name, int width, int height, int pixfmt, int align,
                int fps, int gop, int kbs, CedarcEncodeCallback callback)
      : name_(name ? name : ""), av_pixfmt_(pixfmt), width_(width),
        height_(height), aligned_w_(cedarc_common::align16(width)),
        aligned_h_(cedarc_common::align16(height)), align_(align),
        fps_(fps > 0 ? fps : 30), gop_(gop > 0 ? gop : (fps > 0 ? fps : 30)),
        kbs_(kbs > 0 ? kbs : 2000), callback_(callback) {}

  ~CedarcEncoder() { free_encoder(); }

  bool init(int *linesize, int *offset, int *length) {
    set_last_error("");
    if (!load_cedarc_api()) {
      set_last_error("CedarC libvencoder.so is not available");
      return false;
    }

    if (name_.find("h264") != std::string::npos) {
      codec_type_ = VENC_CODEC_H264;
    } else if (name_.find("hevc") != std::string::npos ||
               name_.find("h265") != std::string::npos) {
      codec_type_ = VENC_CODEC_H265;
    } else {
      set_last_error("Unsupported CedarC codec: " + name_);
      return false;
    }

    if (width_ <= 0 || height_ <= 0 || (width_ % 2) != 0 ||
        (height_ % 2) != 0) {
      set_last_error("CedarC requires positive even dimensions");
      return false;
    }

    if (!map_pixel_format(av_pixfmt_, &venc_pixfmt_, &is_packed_422_,
                          &is_planar_420_)) {
      set_last_error("Unsupported CedarC input pixel format: " +
                     std::to_string(av_pixfmt_));
      return false;
    }

    if (ffmpeg_ram_get_linesize_offset_length(av_pixfmt_, width_, height_,
                                              align_, linesize, offset,
                                              length) != 0) {
      set_last_error("Failed to compute CedarC buffer layout");
      return false;
    }

    enc_ = g_api.VideoEncCreate(codec_type_);
    if (!enc_) {
      set_last_error("CedarC VideoEncCreate failed");
      return false;
    }

    unsigned int vbv_size =
        cedarc_common::vbv_size_for_resolution(width_, height_);
    g_api.VideoEncSetParameter(enc_, VENC_IndexParamSetVbvSize, &vbv_size);

    if (codec_type_ == VENC_CODEC_H264) {
      VencH264Param h264 = cedarc_common::build_h264_cbr_param(
          width_, height_, fps_, kbs_, gop_);

      int ret = g_api.VideoEncSetParameter(enc_, VENC_IndexParamH264Param,
                                           &h264);
      if (ret != 0) {
        set_last_error("CedarC SetParameter(H264Param) failed: " +
                       std::to_string(ret));
        return false;
      }
    } else {
      VencH265Param h265 = cedarc_common::build_h265_cbr_param(
          width_, height_, fps_, kbs_, gop_);

      int ret = g_api.VideoEncSetParameter(enc_, VENC_IndexParamH265Param,
                                           &h265);
      if (ret != 0) {
        set_last_error("CedarC SetParameter(H265Param) failed: " +
                       std::to_string(ret));
        return false;
      }
    }

    VencBaseConfig base;
    std::memset(&base, 0, sizeof(base));
    base.nInputWidth = static_cast<unsigned int>(width_);
    base.nInputHeight = static_cast<unsigned int>(height_);
    base.nDstWidth = static_cast<unsigned int>(width_);
    base.nDstHeight = static_cast<unsigned int>(height_);
    base.nStride = static_cast<unsigned int>(aligned_w_);
    base.eInputFormat = venc_pixfmt_;

    int ret = g_api.VideoEncInit(enc_, &base);
    if (ret != 0) {
      set_last_error("CedarC VideoEncInit failed: " + std::to_string(ret));
      return false;
    }

    VencHeaderData hdr;
    std::memset(&hdr, 0, sizeof(hdr));
    const VENC_INDEXTYPE hdr_idx = (codec_type_ == VENC_CODEC_H264)
                                       ? VENC_IndexParamH264SPSPPS
                                       : VENC_IndexParamH265Header;
    if (g_api.VideoEncGetParameter(enc_, hdr_idx, &hdr) == 0 && hdr.pBuffer &&
        hdr.nLength > 0) {
      header_.assign(hdr.pBuffer, hdr.pBuffer + hdr.nLength);
    }

    VencAllocateBufferParam abp;
    std::memset(&abp, 0, sizeof(abp));
    abp.nBufferNum = 2;
    if (is_packed_422_) {
      abp.nSizeY = static_cast<unsigned int>(aligned_w_ * aligned_h_ * 2);
      abp.nSizeC = 0;
    } else {
      abp.nSizeY = static_cast<unsigned int>(aligned_w_ * aligned_h_);
      abp.nSizeC = static_cast<unsigned int>(aligned_w_ * aligned_h_ / 2);
    }

    ret = g_api.AllocInputBuffer(enc_, &abp);
    if (ret != 0) {
      set_last_error("CedarC AllocInputBuffer failed: " + std::to_string(ret));
      return false;
    }
    input_buffers_allocated_ = true;
    return true;
  }

  void request_keyframe() { force_keyframe_ = true; }

  int set_bitrate(int kbs) {
    if (!enc_ || kbs <= 0) {
      return -1;
    }
    int bps = std::max(kbs, 64) * 1000;
    int ret = g_api.VideoEncSetParameter(enc_, VENC_IndexParamBitrate, &bps);
    if (ret == 0) {
      kbs_ = kbs;
      return 0;
    }
    LOG_WARN(std::string("CedarC SetBitrate failed, ret = ") +
             std::to_string(ret));
    return -1;
  }

  int encode(const uint8_t *data, int length, const void *obj, int64_t ms,
             CedarcEncodePacketCallback packet_callback) {
    if (!enc_ || !data || length <= 0) {
      return AVERROR(EINVAL);
    }

    const size_t required_len =
        is_packed_422_
            ? static_cast<size_t>(width_) * static_cast<size_t>(height_) * 2u
            : static_cast<size_t>(width_) * static_cast<size_t>(height_) * 3u /
                  2u;
    if (static_cast<size_t>(length) < required_len) {
      LOG_ERROR(std::string("CedarC input too small: ") +
                std::to_string(length) + " < " + std::to_string(required_len));
      return AVERROR(EINVAL);
    }

    if (force_keyframe_) {
      int force = 1;
      g_api.VideoEncSetParameter(enc_, VENC_IndexParamForceKeyFrame, &force);
      force_keyframe_ = false;
    }

    VencInputBuffer inbuf;
    std::memset(&inbuf, 0, sizeof(inbuf));
    int ret = g_api.GetOneAllocInputBuffer(enc_, &inbuf);
    if (ret != 0) {
      LOG_ERROR(std::string("CedarC GetOneAllocInputBuffer failed: ") +
                std::to_string(ret));
      return AVERROR(EAGAIN);
    }

    if (!copy_input_frame(data, &inbuf)) {
      g_api.ReturnOneAllocInputBuffer(enc_, &inbuf);
      return AVERROR(EINVAL);
    }

    inbuf.nPts = ms * 1000;
    inbuf.bEnableCorp = 0;

    g_api.FlushCacheAllocInputBuffer(enc_, &inbuf);
    ret = g_api.AddOneInputBuffer(enc_, &inbuf);
    if (ret != 0) {
      g_api.ReturnOneAllocInputBuffer(enc_, &inbuf);
      LOG_ERROR(std::string("CedarC AddOneInputBuffer failed: ") +
                std::to_string(ret));
      return -1;
    }

    const int enc_ret = g_api.VideoEncodeOneFrame(enc_);

    VencInputBuffer used;
    std::memset(&used, 0, sizeof(used));
    if (g_api.AlreadyUsedInputBuffer(enc_, &used) == 0) {
      g_api.ReturnOneAllocInputBuffer(enc_, &used);
    }

    if (enc_ret == VENC_RESULT_NO_FRAME_BUFFER ||
        enc_ret == VENC_RESULT_BITSTREAM_IS_EMPTY ||
        enc_ret == VENC_RESULT_DROP_FRAME) {
      return AVERROR(EAGAIN);
    }
    if (enc_ret != VENC_RESULT_OK) {
      LOG_ERROR(std::string("CedarC VideoEncodeOneFrame failed: ") +
                std::to_string(enc_ret));
      return -1;
    }

    VencOutputBuffer outbuf;
    std::memset(&outbuf, 0, sizeof(outbuf));
    ret = g_api.GetOneBitstreamFrame(enc_, &outbuf);
    if (ret != 0) {
      return AVERROR(EAGAIN);
    }

    const int is_key = (outbuf.nFlag & VENC_BUFFERFLAG_KEYFRAME) ? 1 : 0;
    const bool need_header =
        is_key && !header_.empty() &&
        !cedarc_common::packet_has_param_sets(outbuf.pData0, outbuf.nSize0,
                                              codec_type_);

    const size_t total_size =
        (need_header ? header_.size() : 0u) +
        (outbuf.pData0 ? static_cast<size_t>(outbuf.nSize0) : 0u) +
        (outbuf.pData1 ? static_cast<size_t>(outbuf.nSize1) : 0u) +
        (outbuf.pData2 ? static_cast<size_t>(outbuf.nSize2) : 0u);

    if (total_size == 0) {
      g_api.FreeOneBitStreamFrame(enc_, &outbuf);
      return AVERROR(EAGAIN);
    }

    if (packet_callback) {
      AVPacket *pkt = av_packet_alloc();
      if (!pkt || av_new_packet(pkt, static_cast<int>(total_size)) < 0) {
        if (pkt) {
          av_packet_free(&pkt);
        }
        g_api.FreeOneBitStreamFrame(enc_, &outbuf);
        return AVERROR(ENOMEM);
      }
      uint8_t *dst = pkt->data;
      if (need_header) {
        std::memcpy(dst, header_.data(), header_.size());
        dst += header_.size();
      }
      if (outbuf.pData0 && outbuf.nSize0 > 0) {
        std::memcpy(dst, outbuf.pData0, outbuf.nSize0);
        dst += outbuf.nSize0;
      }
      if (outbuf.pData1 && outbuf.nSize1 > 0) {
        std::memcpy(dst, outbuf.pData1, outbuf.nSize1);
        dst += outbuf.nSize1;
      }
      if (outbuf.pData2 && outbuf.nSize2 > 0) {
        std::memcpy(dst, outbuf.pData2, outbuf.nSize2);
      }
      g_api.FreeOneBitStreamFrame(enc_, &outbuf);

      pkt->pts = ms;
      pkt->dts = ms;
      pkt->flags = is_key ? AV_PKT_FLAG_KEY : 0;
      packet_callback(pkt, pkt->data, pkt->size, pkt->pts, is_key, obj);
      return 0;
    }

    staging_buf_.resize(total_size);
    uint8_t *dst = staging_buf_.data();
    if (need_header) {
      std::memcpy(dst, header_.data(), header_.size());
      dst += header_.size();
    }
    if (outbuf.pData0 && outbuf.nSize0 > 0) {
      std::memcpy(dst, outbuf.pData0, outbuf.nSize0);
      dst += outbuf.nSize0;
    }
    if (outbuf.pData1 && outbuf.nSize1 > 0) {
      std::memcpy(dst, outbuf.pData1, outbuf.nSize1);
      dst += outbuf.nSize1;
    }
    if (outbuf.pData2 && outbuf.nSize2 > 0) {
      std::memcpy(dst, outbuf.pData2, outbuf.nSize2);
    }
    g_api.FreeOneBitStreamFrame(enc_, &outbuf);

    if (callback_) {
      callback_(staging_buf_.data(), static_cast<int>(staging_buf_.size()), ms,
                is_key, obj);
    }
    return 0;
  }

  void free_encoder() {
    if (!enc_) {
      return;
    }
    if (input_buffers_allocated_) {
      g_api.ReleaseAllocInputBuffer(enc_);
      input_buffers_allocated_ = false;
    }
    g_api.VideoEncDestroy(enc_);
    enc_ = nullptr;
  }

private:
  bool copy_input_frame(const uint8_t *src, VencInputBuffer *inbuf) {
    if (!inbuf->pAddrVirY) {
      return false;
    }
    if (is_packed_422_) {
      const size_t src_stride = static_cast<size_t>(width_) * 2u;
      const size_t dst_stride = static_cast<size_t>(aligned_w_) * 2u;
      if (src_stride == dst_stride) {
        std::memcpy(inbuf->pAddrVirY, src,
                    src_stride * static_cast<size_t>(height_));
      } else {
        for (int y = 0; y < height_; ++y) {
          std::memcpy(inbuf->pAddrVirY + static_cast<size_t>(y) * dst_stride,
                      src + static_cast<size_t>(y) * src_stride, src_stride);
        }
      }
      return true;
    }

    if (!inbuf->pAddrVirC) {
      return false;
    }
    const size_t y_src_stride = static_cast<size_t>(width_);
    const size_t y_dst_stride = static_cast<size_t>(aligned_w_);
    const size_t y_src_size = y_src_stride * static_cast<size_t>(height_);
    if (is_planar_420_) {
      const size_t uv_src_stride = y_src_stride / 2u;
      const size_t uv_dst_stride = y_dst_stride / 2u;
      const size_t uv_rows = static_cast<size_t>(height_ / 2);
      const size_t uv_dst_plane_size =
          uv_dst_stride * static_cast<size_t>(aligned_h_ / 2);
      const uint8_t *src_u = src + y_src_size;
      const uint8_t *src_v = src_u + uv_src_stride * uv_rows;
      if (y_src_stride == y_dst_stride && height_ == aligned_h_) {
        std::memcpy(inbuf->pAddrVirY, src, y_src_size);
        std::memcpy(inbuf->pAddrVirC, src_u, y_src_size / 2u);
      } else {
        for (int y = 0; y < height_; ++y) {
          std::memcpy(inbuf->pAddrVirY + static_cast<size_t>(y) * y_dst_stride,
                      src + static_cast<size_t>(y) * y_src_stride,
                      y_src_stride);
        }
        uint8_t *dst_u = inbuf->pAddrVirC;
        uint8_t *dst_v = inbuf->pAddrVirC + uv_dst_plane_size;
        for (size_t y = 0; y < uv_rows; ++y) {
          std::memcpy(dst_u + y * uv_dst_stride, src_u + y * uv_src_stride,
                      uv_src_stride);
          std::memcpy(dst_v + y * uv_dst_stride, src_v + y * uv_src_stride,
                      uv_src_stride);
        }
      }
      return true;
    }
    const uint8_t *src_uv = src + y_src_size;
    if (y_src_stride == y_dst_stride) {
      std::memcpy(inbuf->pAddrVirY, src, y_src_size);
      std::memcpy(inbuf->pAddrVirC, src_uv, y_src_size / 2u);
    } else {
      for (int y = 0; y < height_; ++y) {
        std::memcpy(inbuf->pAddrVirY + static_cast<size_t>(y) * y_dst_stride,
                    src + static_cast<size_t>(y) * y_src_stride, y_src_stride);
      }
      for (int y = 0; y < height_ / 2; ++y) {
        std::memcpy(inbuf->pAddrVirC + static_cast<size_t>(y) * y_dst_stride,
                    src_uv + static_cast<size_t>(y) * y_src_stride,
                    y_src_stride);
      }
    }
    return true;
  }
};

} // namespace

extern "C" int cedarc_is_supported(void) {
  const bool has_ve1 = (access("/dev/cedar_dev", F_OK) == 0);
  const bool has_ve2 = (access("/dev/cedar_dev_ve2", F_OK) == 0);
  if (!has_ve1 && !has_ve2) {
    return -1;
  }

  const bool ve1_ok = !has_ve1 || (access("/dev/cedar_dev", R_OK | W_OK) == 0);
  const bool ve2_ok =
      !has_ve2 || (access("/dev/cedar_dev_ve2", R_OK | W_OK) == 0);
  const bool mem_accessible =
      (access("/dev/dma_heap/system", R_OK | W_OK) == 0) ||
      (access("/dev/ion", R_OK | W_OK) == 0);

  if (!ve1_ok || !ve2_ok || !mem_accessible) {
    LOG_WARN(std::string(
        "Allwinner VE device found, but /dev/cedar_dev* or /dev/dma_heap/system "
        "is not readable/writable; run as root or configure udev permissions"));
    return -1;
  }

  if (!load_cedarc_api()) {
    return -1;
  }

  return 0;
}

extern "C" void *cedarc_new_encoder(const char *name, int width, int height,
                                    int pixfmt, int align, int fps, int gop,
                                    int rc, int kbs, int *linesize, int *offset,
                                    int *length,
                                    CedarcEncodeCallback callback) {
  (void)rc;
  try {
    auto *enc = new CedarcEncoder(name, width, height, pixfmt, align, fps, gop,
                                  kbs, callback);
    if (enc->init(linesize, offset, length)) {
      return enc;
    }
    delete enc;
    return nullptr;
  } catch (const std::exception &e) {
    set_last_error(std::string("cedarc_new_encoder exception: ") + e.what());
    return nullptr;
  }
}

extern "C" int cedarc_encode(void *encoder, const uint8_t *data, int length,
                             const void *obj, int64_t ms) {
  if (!encoder) {
    return -1;
  }
  try {
    return static_cast<CedarcEncoder *>(encoder)->encode(data, length, obj, ms,
                                                         nullptr);
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("cedarc_encode failed: ") + e.what());
    return -1;
  }
}

extern "C" int cedarc_encode_packet(void *encoder, const uint8_t *data,
                                    int length, const void *obj, int64_t ms,
                                    CedarcEncodePacketCallback callback) {
  if (!encoder) {
    return -1;
  }
  try {
    return static_cast<CedarcEncoder *>(encoder)->encode(data, length, obj, ms,
                                                         callback);
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("cedarc_encode_packet failed: ") + e.what());
    return -1;
  }
}

extern "C" int cedarc_set_bitrate(void *encoder, int kbs) {
  if (!encoder) {
    return -1;
  }
  try {
    return static_cast<CedarcEncoder *>(encoder)->set_bitrate(kbs);
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("cedarc_set_bitrate failed: ") + e.what());
    return -1;
  }
}

extern "C" void cedarc_request_keyframe(void *encoder) {
  if (!encoder) {
    return;
  }
  try {
    static_cast<CedarcEncoder *>(encoder)->request_keyframe();
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("cedarc_request_keyframe failed: ") + e.what());
  }
}

extern "C" void cedarc_free_encoder(void *encoder) {
  if (!encoder) {
    return;
  }
  try {
    delete static_cast<CedarcEncoder *>(encoder);
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("cedarc_free_encoder failed: ") + e.what());
  }
}

extern "C" const char *cedarc_encoder_last_error(void) {
  return g_last_error.c_str();
}
