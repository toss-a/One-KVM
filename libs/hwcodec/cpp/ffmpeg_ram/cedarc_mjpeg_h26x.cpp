#include "cedarc_mjpeg_h26x.h"
#include "cedarc_abi.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

#define LOG_MODULE "CEDARC_MJPEG"
#include <log.h>

namespace {

using namespace cedarc_abi;

using FnMemAdapterGetOpsS = ScMemOpsS *(*)(void);
using FnAddVDPlugin = void (*)(void);

using FnCreateVideoDecoder = VideoDecoder *(*)(void);
using FnDestroyVideoDecoder = void (*)(VideoDecoder *);
using FnInitializeVideoDecoder = int (*)(VideoDecoder *, VideoStreamInfo *,
                                         VConfig *);
using FnResetVideoDecoder = void (*)(VideoDecoder *);
using FnRequestVideoStreamBuffer = int (*)(VideoDecoder *, int, char **, int *,
                                           char **, int *, int);
using FnSubmitVideoStreamData = int (*)(VideoDecoder *, VideoStreamDataInfo *,
                                        int);
using FnDecodeVideoStream = int (*)(VideoDecoder *, int, int, int, int64_t);
using FnRequestPicture = VideoPicture *(*)(VideoDecoder *, int);
using FnReturnPicture = int (*)(VideoDecoder *, VideoPicture *);

using FnVideoEncCreate = VideoEncoder *(*)(VENC_CODEC_TYPE);
using FnVideoEncDestroy = void (*)(VideoEncoder *);
using FnVideoEncInit = int (*)(VideoEncoder *, VencBaseConfig *);
using FnAddOneInputBuffer = int (*)(VideoEncoder *, VencInputBuffer *);
using FnVideoEncodeOneFrame = int (*)(VideoEncoder *);
using FnAlreadyUsedInputBuffer = int (*)(VideoEncoder *, VencInputBuffer *);
using FnGetOneBitstreamFrame = int (*)(VideoEncoder *, VencOutputBuffer *);
using FnFreeOneBitStreamFrame = int (*)(VideoEncoder *, VencOutputBuffer *);
using FnVideoEncGetParameter = int (*)(VideoEncoder *, VENC_INDEXTYPE, void *);
using FnVideoEncSetParameter = int (*)(VideoEncoder *, VENC_INDEXTYPE, void *);
using FnVideoEncoderGetVeIommuAddr = void (*)(VideoEncoder *,
                                              user_iommu_param *);
using FnVideoEncoderFreeVeIommuAddr = void (*)(VideoEncoder *,
                                               user_iommu_param *);

struct CedarcMjpegApi {
  bool loaded = false;
  void *h_mem = nullptr;
  void *h_ve = nullptr;
  void *h_vdec = nullptr;
  void *h_venc = nullptr;

  FnMemAdapterGetOpsS MemAdapterGetOpsS = nullptr;
  FnAddVDPlugin AddVDPlugin = nullptr;

  FnCreateVideoDecoder CreateVideoDecoder = nullptr;
  FnDestroyVideoDecoder DestroyVideoDecoder = nullptr;
  FnInitializeVideoDecoder InitializeVideoDecoder = nullptr;
  FnResetVideoDecoder ResetVideoDecoder = nullptr;
  FnRequestVideoStreamBuffer RequestVideoStreamBuffer = nullptr;
  FnSubmitVideoStreamData SubmitVideoStreamData = nullptr;
  FnDecodeVideoStream DecodeVideoStream = nullptr;
  FnRequestPicture RequestPicture = nullptr;
  FnReturnPicture ReturnPicture = nullptr;

  FnVideoEncCreate VideoEncCreate = nullptr;
  FnVideoEncDestroy VideoEncDestroy = nullptr;
  FnVideoEncInit VideoEncInit = nullptr;
  FnAddOneInputBuffer AddOneInputBuffer = nullptr;
  FnVideoEncodeOneFrame VideoEncodeOneFrame = nullptr;
  FnAlreadyUsedInputBuffer AlreadyUsedInputBuffer = nullptr;
  FnGetOneBitstreamFrame GetOneBitstreamFrame = nullptr;
  FnFreeOneBitStreamFrame FreeOneBitStreamFrame = nullptr;
  FnVideoEncGetParameter VideoEncGetParameter = nullptr;
  FnVideoEncSetParameter VideoEncSetParameter = nullptr;
  FnVideoEncoderGetVeIommuAddr VideoEncoderGetVeIommuAddr = nullptr;
  FnVideoEncoderFreeVeIommuAddr VideoEncoderFreeVeIommuAddr = nullptr;
};

static CedarcMjpegApi g_api;
static std::once_flag g_api_once;
static thread_local std::string g_last_error;

static void set_last_error(const std::string &msg) { g_last_error = msg; }

static void *dlopen_cedarc_lib(const char *soname, int flags) {
  const std::string candidates[] = {
      soname,
      std::string("/usr/lib/aarch64-linux-gnu/") + soname,
      std::string("/usr/lib/arm-linux-gnueabihf/") + soname,
      std::string("/usr/lib/") + soname,
  };
  for (const auto &path : candidates) {
    void *h = dlopen(path.c_str(), flags);
    if (h) {
      return h;
    }
  }
  return nullptr;
}

static bool load_cedarc_mjpeg_api() {
  std::call_once(g_api_once, []() {
    // RTLD_GLOBAL is required so AddVDPlugin() can dlopen libawmjpeg.so and
    // resolve symbols from libvideoengine.so / libvdecoder.so.
    g_api.h_mem =
        dlopen_cedarc_lib("libMemAdapter.so", RTLD_NOW | RTLD_GLOBAL);
    g_api.h_ve =
        dlopen_cedarc_lib("libvideoengine.so", RTLD_NOW | RTLD_GLOBAL);
    g_api.h_vdec =
        dlopen_cedarc_lib("libvdecoder.so", RTLD_NOW | RTLD_GLOBAL);
    g_api.h_venc =
        dlopen_cedarc_lib("libvencoder.so", RTLD_NOW | RTLD_GLOBAL);

    if (!g_api.h_mem || !g_api.h_vdec || !g_api.h_venc) {
      LOG_TRACE(std::string(
          "CedarC MJPEG: required libraries (libvdecoder/libvencoder/"
          "libMemAdapter) not found"));
      return;
    }

#define LOAD_REQUIRED(handle, sym)                                             \
  do {                                                                         \
    g_api.sym = reinterpret_cast<Fn##sym>(dlsym((handle), #sym));              \
    if (!g_api.sym) {                                                          \
      LOG_WARN(std::string("CedarC MJPEG: missing symbol ") + #sym);           \
      return;                                                                  \
    }                                                                          \
  } while (0)

    LOAD_REQUIRED(g_api.h_mem, MemAdapterGetOpsS);

    g_api.AddVDPlugin = reinterpret_cast<FnAddVDPlugin>(
        dlsym(g_api.h_ve ? g_api.h_ve : RTLD_DEFAULT, "AddVDPlugin"));
    if (!g_api.AddVDPlugin) {
      g_api.AddVDPlugin =
          reinterpret_cast<FnAddVDPlugin>(dlsym(g_api.h_vdec, "AddVDPlugin"));
    }

    LOAD_REQUIRED(g_api.h_vdec, CreateVideoDecoder);
    LOAD_REQUIRED(g_api.h_vdec, DestroyVideoDecoder);
    LOAD_REQUIRED(g_api.h_vdec, InitializeVideoDecoder);
    g_api.ResetVideoDecoder = reinterpret_cast<FnResetVideoDecoder>(
        dlsym(g_api.h_vdec, "ResetVideoDecoder"));
    LOAD_REQUIRED(g_api.h_vdec, RequestVideoStreamBuffer);
    LOAD_REQUIRED(g_api.h_vdec, SubmitVideoStreamData);
    LOAD_REQUIRED(g_api.h_vdec, DecodeVideoStream);
    LOAD_REQUIRED(g_api.h_vdec, RequestPicture);
    LOAD_REQUIRED(g_api.h_vdec, ReturnPicture);

    LOAD_REQUIRED(g_api.h_venc, VideoEncCreate);
    LOAD_REQUIRED(g_api.h_venc, VideoEncDestroy);
    LOAD_REQUIRED(g_api.h_venc, VideoEncInit);
    LOAD_REQUIRED(g_api.h_venc, AddOneInputBuffer);
    LOAD_REQUIRED(g_api.h_venc, VideoEncodeOneFrame);
    LOAD_REQUIRED(g_api.h_venc, AlreadyUsedInputBuffer);
    LOAD_REQUIRED(g_api.h_venc, GetOneBitstreamFrame);
    LOAD_REQUIRED(g_api.h_venc, FreeOneBitStreamFrame);
    LOAD_REQUIRED(g_api.h_venc, VideoEncGetParameter);
    LOAD_REQUIRED(g_api.h_venc, VideoEncSetParameter);
    LOAD_REQUIRED(g_api.h_venc, VideoEncoderGetVeIommuAddr);
    LOAD_REQUIRED(g_api.h_venc, VideoEncoderFreeVeIommuAddr);
#undef LOAD_REQUIRED

    if (g_api.AddVDPlugin) {
      g_api.AddVDPlugin();
    }

    g_api.loaded = true;
    LOG_DEBUG(std::string(
        "CedarC MJPEG: loaded VE1 decoder + VE2 encoder libraries"));
  });
  return g_api.loaded;
}

static inline int align16(int v) { return (v + 15) & ~15; }

// Standard ISO/IEC 10918-1 Annex K Huffman tables (DHT segment, 420 bytes).
// Many UVC HDMI capture cards emit valid MJPEG frames without a DHT marker;
// injecting this segment before SOS lets VE1 decode them directly.
static const uint8_t kStandardMjpegDht[420] = {
    0xFF, 0xC4, 0x01, 0xA2,
    // DC Luminance (Tc=0, Th=0)
    0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
    0x07, 0x08, 0x09, 0x0A, 0x0B,
    // DC Chrominance (Tc=0, Th=1)
    0x01, 0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
    0x07, 0x08, 0x09, 0x0A, 0x0B,
    // AC Luminance (Tc=1, Th=0)
    0x10, 0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04,
    0x04, 0x00, 0x00, 0x01, 0x7D, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05,
    0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14,
    0x32, 0x81, 0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1,
    0xF0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19,
    0x1A, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54,
    0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84,
    0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA,
    0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4,
    0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7,
    0xD8, 0xD9, 0xDA, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9,
    0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA,
    // AC Chrominance (Tc=1, Th=1)
    0x11, 0x00, 0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04,
    0x04, 0x00, 0x01, 0x02, 0x77, 0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05,
    0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32,
    0x81, 0x08, 0x14, 0x42, 0x91, 0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52,
    0xF0, 0x15, 0x62, 0x72, 0xD1, 0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1,
    0x17, 0x18, 0x19, 0x1A, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53,
    0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67,
    0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x82,
    0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95,
    0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8,
    0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2,
    0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5,
    0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8,
    0xE9, 0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA,
};

// Inspect JPEG headers up to SOS (0xFF 0xDA). If no DHT (0xFF 0xC4) is found,
// inject kStandardMjpegDht immediately before SOS into `patch_buf`.
static bool prepare_jpeg_with_dht(const uint8_t *src, size_t len,
                                  std::vector<uint8_t> &patch_buf,
                                  const uint8_t **out_ptr, size_t *out_len) {
  if (!src || len < 4 || src[0] != 0xFF || src[1] != 0xD8) {
    return false;
  }

  size_t pos = 2;
  bool has_dht = false;
  size_t sos_offset = 0;
  bool found_sos = false;

  while (pos + 1 < len) {
    if (src[pos] != 0xFF) {
      ++pos;
      continue;
    }
    const size_t marker_start = pos;
    while (pos < len && src[pos] == 0xFF) {
      ++pos;
    }
    if (pos >= len) {
      break;
    }
    const uint8_t marker = src[pos++];
    if (marker == 0x00 || (marker >= 0xD0 && marker <= 0xD9) ||
        marker == 0x01) {
      if (marker == 0xD9) {
        break;
      }
      continue;
    }
    if (marker == 0xC4) {
      has_dht = true;
      break;
    }
    if (marker == 0xDA) {
      sos_offset = marker_start;
      found_sos = true;
      break;
    }
    if (pos + 2 > len) {
      break;
    }
    const uint16_t seg_len =
        static_cast<uint16_t>((static_cast<uint16_t>(src[pos]) << 8) |
                              static_cast<uint16_t>(src[pos + 1]));
    if (seg_len < 2 || pos + seg_len > len) {
      break;
    }
    pos += seg_len;
  }

  if (has_dht || !found_sos) {
    *out_ptr = src;
    *out_len = len;
    return true;
  }

  patch_buf.resize(len + sizeof(kStandardMjpegDht));
  std::memcpy(patch_buf.data(), src, sos_offset);
  std::memcpy(patch_buf.data() + sos_offset, kStandardMjpegDht,
              sizeof(kStandardMjpegDht));
  std::memcpy(patch_buf.data() + sos_offset + sizeof(kStandardMjpegDht),
              src + sos_offset, len - sos_offset);
  *out_ptr = patch_buf.data();
  *out_len = patch_buf.size();
  return true;
}

static bool map_decoder_pixfmt_to_venc(int dec_fmt, VENC_PIXEL_FMT *out_fmt) {
  switch (dec_fmt) {
  case PIXEL_FORMAT_NV12:
    *out_fmt = VENC_PIXEL_YUV420SP;
    return true;
  case PIXEL_FORMAT_NV21:
    *out_fmt = VENC_PIXEL_YVU420SP;
    return true;
  case PIXEL_FORMAT_YUV_PLANER_420:
    *out_fmt = VENC_PIXEL_YUV420P;
    return true;
  case PIXEL_FORMAT_YV12:
    *out_fmt = VENC_PIXEL_YVU420P;
    return true;
  case PIXEL_FORMAT_PLANARUV_422:
    *out_fmt = VENC_PIXEL_YUV422SP;
    return true;
  case PIXEL_FORMAT_PLANARVU_422:
    *out_fmt = VENC_PIXEL_YVU422SP;
    return true;
  case PIXEL_FORMAT_YUV_PLANER_422:
    *out_fmt = VENC_PIXEL_YUV422P;
    return true;
  default:
    return false;
  }
}

static bool packet_has_param_sets(const uint8_t *data, size_t size,
                                  VENC_CODEC_TYPE codec) {
  if (!data || size < 5) {
    return false;
  }
  size_t offset = 0;
  if (data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
    offset = 4;
  } else if (data[0] == 0 && data[1] == 0 && data[2] == 1) {
    offset = 3;
  } else {
    return false;
  }
  if (offset >= size) {
    return false;
  }
  if (codec == VENC_CODEC_H264) {
    return (data[offset] & 0x1f) == 7; // SPS
  }
  if (codec == VENC_CODEC_H265) {
    return ((data[offset] >> 1) & 0x3f) == 32; // VPS
  }
  return false;
}

class CedarcMjpegH26xPipeline {
public:
  VENC_CODEC_TYPE codec_type_ = VENC_CODEC_H264;
  int width_ = 0;
  int height_ = 0;
  int fps_ = 30;
  int kbs_ = 2000;
  int gop_ = 30;
  int dec_vbv_size_ = 4 * 1024 * 1024;

  ScMemOpsS *memops_ = nullptr;
  bool memops_opened_ = false;
  VideoDecoder *dec_ = nullptr;
  VideoEncoder *enc_ = nullptr;

  VENC_PIXEL_FMT enc_fmt_ = VENC_PIXEL_YUV420SP;
  int enc_stride_ = 0;
  uint64_t frame_id_ = 0;
  int consecutive_decode_misses_ = 0;

  std::vector<uint8_t> header_;
  std::vector<uint8_t> jpeg_patch_buf_;
  std::vector<uint8_t> out_buf_;

  CedarcMjpegH26xPipeline(int codec, int width, int height, int fps, int kbps,
                          int gop)
      : codec_type_(codec == 1 ? VENC_CODEC_H265 : VENC_CODEC_H264),
        width_(width), height_(height), fps_(fps > 0 ? fps : 30),
        kbs_(kbps > 0 ? kbps : 2000),
        gop_(gop > 0 ? gop : (fps > 0 ? fps : 30)),
        dec_vbv_size_((width >= 2560 || height >= 1440) ? (8 * 1024 * 1024)
                                                        : (4 * 1024 * 1024)) {}

  ~CedarcMjpegH26xPipeline() { cleanup(); }

  bool init() {
    set_last_error("");
    if (cedarc_mjpeg_h26x_supported() != 0) {
      set_last_error("Allwinner VE MJPEG->H26x pipeline is not supported");
      return false;
    }

    if (width_ <= 0 || height_ <= 0 || (width_ % 2) != 0 ||
        (height_ % 2) != 0) {
      set_last_error("CedarC MJPEG->H26x requires positive even dimensions");
      return false;
    }

    memops_ = g_api.MemAdapterGetOpsS();
    if (!memops_ || !memops_->open || !memops_->close) {
      set_last_error("CedarC MemAdapterGetOpsS failed");
      return false;
    }
    if (memops_->open() != 0) {
      set_last_error("CedarC MemAdapter open failed");
      return false;
    }
    memops_opened_ = true;

    dec_ = g_api.CreateVideoDecoder();
    if (!dec_) {
      set_last_error("CedarC CreateVideoDecoder failed");
      return false;
    }

    VConfig vcfg;
    std::memset(&vcfg, 0, sizeof(vcfg));
    vcfg.eOutputPixelFormat = PIXEL_FORMAT_NV12;
    vcfg.nDeInterlaceHoldingFrameBufferNum = 0;
    vcfg.nDisplayHoldingFrameBufferNum = 1;
    vcfg.nRotateHoldingFrameBufferNum = 0;
    vcfg.nDecodeSmoothFrameBufferNum = 1;
    vcfg.nVbvBufferSize = dec_vbv_size_;
    vcfg.memops = memops_;
    vcfg.nAlignStride = 16;

    VideoStreamInfo sinfo;
    std::memset(&sinfo, 0, sizeof(sinfo));
    sinfo.eCodecFormat = VIDEO_CODEC_FORMAT_MJPEG;
    sinfo.nWidth = width_;
    sinfo.nHeight = height_;
    sinfo.nFrameRate = fps_ * 1000;
    sinfo.bIsFramePackage = 1;

    int ret = g_api.InitializeVideoDecoder(dec_, &sinfo, &vcfg);
    if (ret != 0) {
      set_last_error("CedarC InitializeVideoDecoder(MJPEG) failed: " +
                     std::to_string(ret));
      return false;
    }

    // Initialize VE2 encoder upfront with standard NV12 + 16-aligned stride so
    // hardware availability is validated immediately during pipeline setup.
    if (!init_encoder(VENC_PIXEL_YUV420SP, align16(width_))) {
      return false;
    }

    return true;
  }

  int reconfigure(int kbps, int gop) {
    if (kbps > 0) {
      kbs_ = kbps;
    }
    if (gop > 0) {
      gop_ = gop;
    }
    if (!enc_) {
      return 0;
    }
    if (kbps > 0) {
      int bps = std::max(kbs_, 64) * 1000;
      int ret = g_api.VideoEncSetParameter(enc_, VENC_IndexParamBitrate, &bps);
      if (ret != 0) {
        set_last_error("CedarC MJPEG reconfigure bitrate failed: " +
                       std::to_string(ret));
        return -1;
      }
    }
    if (gop > 0) {
      int interval = gop_;
      g_api.VideoEncSetParameter(enc_, VENC_IndexParamMaxKeyInterval,
                                 &interval);
    }
    return 0;
  }

  int encode(const uint8_t *jpeg_data, size_t jpeg_len, int64_t pts_us,
             int force_idr, const uint8_t **out_data, size_t *out_size,
             int *out_keyframe) {
    *out_data = nullptr;
    *out_size = 0;
    *out_keyframe = 0;

    if (!dec_ || !enc_) {
      set_last_error("CedarC MJPEG pipeline not initialized");
      return -1;
    }

    const uint8_t *submit_ptr = nullptr;
    size_t submit_len = 0;
    if (!prepare_jpeg_with_dht(jpeg_data, jpeg_len, jpeg_patch_buf_,
                               &submit_ptr, &submit_len)) {
      // Malformed / non-SOI packet from UVC capture; skip without killing HW.
      return 0;
    }
    if (submit_len > static_cast<size_t>(dec_vbv_size_)) {
      set_last_error("MJPEG frame exceeds VE1 VBV buffer size (" +
                     std::to_string(submit_len) + " > " +
                     std::to_string(dec_vbv_size_) + ")");
      return -1;
    }

    // 1. Submit JPEG bitstream to VE1
    char *buf = nullptr;
    char *ring = nullptr;
    int buf_sz = 0;
    int ring_sz = 0;
    int ret = g_api.RequestVideoStreamBuffer(
        dec_, static_cast<int>(submit_len), &buf, &buf_sz, &ring, &ring_sz, 0);
    if (ret != 0 || !buf || (buf_sz + ring_sz) < static_cast<int>(submit_len)) {
      if (g_api.ResetVideoDecoder) {
        g_api.ResetVideoDecoder(dec_);
        ret = g_api.RequestVideoStreamBuffer(dec_, static_cast<int>(submit_len),
                                             &buf, &buf_sz, &ring, &ring_sz, 0);
      }
      if (ret != 0 || !buf ||
          (buf_sz + ring_sz) < static_cast<int>(submit_len)) {
        set_last_error("CedarC RequestVideoStreamBuffer failed: " +
                       std::to_string(ret));
        return -1;
      }
    }

    if (static_cast<int>(submit_len) <= buf_sz) {
      std::memcpy(buf, submit_ptr, submit_len);
    } else {
      std::memcpy(buf, submit_ptr, static_cast<size_t>(buf_sz));
      std::memcpy(ring, submit_ptr + buf_sz,
                  submit_len - static_cast<size_t>(buf_sz));
    }

    VideoStreamDataInfo dinfo;
    std::memset(&dinfo, 0, sizeof(dinfo));
    dinfo.pData = buf;
    dinfo.nLength = static_cast<int>(submit_len);
    dinfo.nPts = pts_us;
    dinfo.bIsFirstPart = 1;
    dinfo.bIsLastPart = 1;
    dinfo.bValid = 1;

    ret = g_api.SubmitVideoStreamData(dec_, &dinfo, 0);
    if (ret != 0) {
      set_last_error("CedarC SubmitVideoStreamData failed: " +
                     std::to_string(ret));
      return -1;
    }

    // 2. Hardware MJPEG decode on VE1
    VideoPicture *pic = nullptr;
    int last_dret = VDECODE_RESULT_OK;
    for (int attempt = 0; attempt < 4; ++attempt) {
      last_dret = g_api.DecodeVideoStream(dec_, 0, 0, 0, 0);
      pic = g_api.RequestPicture(dec_, 0);
      if (pic) {
        break;
      }
      if (last_dret == VDECODE_RESULT_UNSUPPORTED) {
        break;
      }
      if (last_dret == VDECODE_RESULT_NO_BITSTREAM) {
        break;
      }
    }

    if (!pic) {
      ++consecutive_decode_misses_;
      if (last_dret == VDECODE_RESULT_UNSUPPORTED ||
          consecutive_decode_misses_ >= 5) {
        set_last_error("CedarC VE1 MJPEG decode failed (ret=" +
                       std::to_string(last_dret) + ", misses=" +
                       std::to_string(consecutive_decode_misses_) + ")");
        return -1;
      }
      // Transient corrupt MJPEG frame from UVC; drop cleanly.
      return 0;
    }
    consecutive_decode_misses_ = 0;

    // Guarantee VE1 FBM buffer is always returned on every exit path.
    struct PictureGuard {
      VideoDecoder *dec;
      VideoPicture *pic;
      ~PictureGuard() {
        if (dec && pic) {
          g_api.ReturnPicture(dec, pic);
        }
      }
    } pic_guard{dec_, pic};

    if (pic->bFrameErrorFlag != 0) {
      return 0;
    }

    VENC_PIXEL_FMT decoded_venc_fmt = VENC_PIXEL_YUV420SP;
    if (!map_decoder_pixfmt_to_venc(pic->ePixelFormat, &decoded_venc_fmt)) {
      set_last_error("Unsupported VE1 decoded pixel format for VE2: " +
                     std::to_string(pic->ePixelFormat));
      return -1;
    }

    const int pic_stride =
        (pic->nLineStride > 0) ? pic->nLineStride : align16(width_);
    if (decoded_venc_fmt != enc_fmt_ || pic_stride != enc_stride_) {
      LOG_INFO(std::string("CedarC MJPEG: adapting VE2 encoder to fmt=") +
               std::to_string(static_cast<int>(decoded_venc_fmt)) +
               " stride=" + std::to_string(pic_stride));
      if (!init_encoder(decoded_venc_fmt, pic_stride)) {
        return -1;
      }
    }

    // 3. Zero-copy DMA-buf -> VE2 IOMMU mapping
    if (pic->nBufFd <= 0) {
      set_last_error("CedarC VE1 decoded picture has no DMA-buf fd");
      return -1;
    }

    user_iommu_param iommu;
    std::memset(&iommu, 0, sizeof(iommu));
    iommu.fd = pic->nBufFd;
    g_api.VideoEncoderGetVeIommuAddr(enc_, &iommu);
    if (iommu.iommu_addr == 0) {
      set_last_error("CedarC VideoEncoderGetVeIommuAddr failed for fd=" +
                     std::to_string(pic->nBufFd));
      return -1;
    }

    struct IommuGuard {
      VideoEncoder *enc;
      user_iommu_param *param;
      ~IommuGuard() {
        if (enc && param && param->iommu_addr != 0) {
          g_api.VideoEncoderFreeVeIommuAddr(enc, param);
          param->iommu_addr = 0;
        }
      }
    } iommu_guard{enc_, &iommu};

    const int pic_h = (pic->nHeight > 0) ? pic->nHeight : align16(height_);
    const uintptr_t c_off =
        (pic->phyCBufAddr >= pic->phyYBufAddr && pic->phyCBufAddr > 0)
            ? static_cast<uintptr_t>(pic->phyCBufAddr - pic->phyYBufAddr)
            : static_cast<uintptr_t>(pic_stride) *
                  static_cast<uintptr_t>(pic_h);
    const uintptr_t phy_y = static_cast<uintptr_t>(iommu.iommu_addr);
    const uintptr_t phy_c = phy_y + c_off;

    if (force_idr) {
      int force = 1;
      g_api.VideoEncSetParameter(enc_, VENC_IndexParamForceKeyFrame, &force);
    }

    VencInputBuffer inbuf;
    std::memset(&inbuf, 0, sizeof(inbuf));
    inbuf.nID = static_cast<unsigned long>(frame_id_++);
    inbuf.nPts = static_cast<long long>(pts_us);
    inbuf.pAddrPhyY = reinterpret_cast<unsigned char *>(phy_y);
    inbuf.pAddrPhyC = reinterpret_cast<unsigned char *>(phy_c);
    inbuf.pAddrVirY = reinterpret_cast<unsigned char *>(pic->pData0);
    inbuf.pAddrVirC = reinterpret_cast<unsigned char *>(pic->pData1);
    inbuf.bEnableCorp = 0;
    inbuf.bAllocMemSelf = 0;
    inbuf.nShareBufFd = pic->nBufFd;

    ret = g_api.AddOneInputBuffer(enc_, &inbuf);
    if (ret != 0) {
      set_last_error("CedarC VE2 AddOneInputBuffer failed: " +
                     std::to_string(ret));
      return -1;
    }

    const int enc_ret = g_api.VideoEncodeOneFrame(enc_);

    VencInputBuffer used;
    std::memset(&used, 0, sizeof(used));
    g_api.AlreadyUsedInputBuffer(enc_, &used);

    // Unmap IOMMU before leaving scope (iommu_guard also ensures this).
    g_api.VideoEncoderFreeVeIommuAddr(enc_, &iommu);
    iommu.iommu_addr = 0;

    if (enc_ret == VENC_RESULT_NO_FRAME_BUFFER ||
        enc_ret == VENC_RESULT_BITSTREAM_IS_EMPTY ||
        enc_ret == VENC_RESULT_DROP_FRAME) {
      return 0;
    }
    if (enc_ret != VENC_RESULT_OK) {
      set_last_error("CedarC VE2 VideoEncodeOneFrame failed: " +
                     std::to_string(enc_ret));
      return -1;
    }

    VencOutputBuffer outbuf;
    std::memset(&outbuf, 0, sizeof(outbuf));
    ret = g_api.GetOneBitstreamFrame(enc_, &outbuf);
    if (ret != 0) {
      return 0;
    }

    const int is_key = (outbuf.nFlag & VENC_BUFFERFLAG_KEYFRAME) ? 1 : 0;
    const bool need_header =
        is_key && !header_.empty() &&
        !packet_has_param_sets(outbuf.pData0, outbuf.nSize0, codec_type_);

    const size_t total_size =
        (need_header ? header_.size() : 0u) +
        (outbuf.pData0 ? static_cast<size_t>(outbuf.nSize0) : 0u) +
        (outbuf.pData1 ? static_cast<size_t>(outbuf.nSize1) : 0u) +
        (outbuf.pData2 ? static_cast<size_t>(outbuf.nSize2) : 0u);

    if (total_size == 0) {
      g_api.FreeOneBitStreamFrame(enc_, &outbuf);
      return 0;
    }

    out_buf_.resize(total_size);
    uint8_t *dst = out_buf_.data();
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

    *out_data = out_buf_.data();
    *out_size = out_buf_.size();
    *out_keyframe = is_key;
    return 0;
  }

private:
  bool init_encoder(VENC_PIXEL_FMT fmt, int stride) {
    if (enc_) {
      g_api.VideoEncDestroy(enc_);
      enc_ = nullptr;
    }
    header_.clear();

    enc_ = g_api.VideoEncCreate(codec_type_);
    if (!enc_) {
      set_last_error("CedarC VE2 VideoEncCreate failed");
      return false;
    }

    unsigned int vbv_size =
        (width_ >= 2560 || height_ >= 1440) ? (8u * 1024u * 1024u)
                                            : (4u * 1024u * 1024u);
    g_api.VideoEncSetParameter(enc_, VENC_IndexParamSetVbvSize, &vbv_size);

    const int bitrate_bps = std::max(kbs_, 64) * 1000;

    if (codec_type_ == VENC_CODEC_H264) {
      VencH264Param h264;
      std::memset(&h264, 0, sizeof(h264));
      h264.bEntropyCodingCABAC = 0; // Constrained Baseline uses CAVLC
      h264.nBitrate = bitrate_bps;
      h264.nFramerate = fps_;
      h264.nSrcFramerate = fps_;
      h264.nCodingMode = VENC_FRAME_CODING;
      h264.nMaxKeyInterval = gop_;
      h264.sProfileLevel.nProfile = VENC_H264ProfileBaseline;
      h264.sProfileLevel.nLevel = (width_ >= 2560 || height_ >= 1440)
                                      ? VENC_H264Level51
                                      : VENC_H264Level41;
      h264.sQPRange.nMinqp = 15;
      h264.sQPRange.nMaxqp = 45;
      h264.sQPRange.nMinPqp = 15;
      h264.sQPRange.nMaxPqp = 45;
      h264.sQPRange.nQpInit = 28;
      h264.sGopParam.bUseGopCtrlEn = 1;
      h264.sGopParam.eGopMode = AW_NORMALP;
      h264.sRcParam.eRcMode = AW_VBR;
      h264.sRcParam.sVbrParam.uMaxBitRate =
          static_cast<unsigned int>(bitrate_bps) * 2u;
      h264.sRcParam.sVbrParam.nMovingTh = 20;
      h264.sRcParam.sVbrParam.nQuality = 10;

      int ret = g_api.VideoEncSetParameter(enc_, VENC_IndexParamH264Param,
                                           &h264);
      if (ret != 0) {
        set_last_error("CedarC VE2 SetParameter(H264Param) failed: " +
                       std::to_string(ret));
        return false;
      }

      VencH264ConstraintFlag cflag;
      std::memset(&cflag, 0, sizeof(cflag));
      cflag.constraint_0 = 1;
      cflag.constraint_1 = 1;
      g_api.VideoEncSetParameter(enc_, VENC_IndexParamH264ConstraintFlag,
                                 &cflag);
    } else {
      VencH265Param h265;
      std::memset(&h265, 0, sizeof(h265));
      h265.nBitrate = bitrate_bps;
      h265.nFramerate = fps_;
      h265.nSrcFramerate = fps_;
      h265.nGopSize = gop_;
      h265.nIntraPeriod = gop_;
      h265.idr_period = gop_;
      h265.nQPInit = 28;
      h265.sProfileLevel.nProfile = VENC_H265ProfileMain;
      h265.sProfileLevel.nLevel = (width_ >= 2560 || height_ >= 1440)
                                      ? VENC_H265Level51
                                      : VENC_H265Level41;
      h265.sQPRange.nMinqp = 15;
      h265.sQPRange.nMaxqp = 45;
      h265.sQPRange.nMinPqp = 15;
      h265.sQPRange.nMaxPqp = 45;
      h265.sQPRange.nQpInit = 28;
      h265.sGopParam.bUseGopCtrlEn = 1;
      h265.sGopParam.eGopMode = AW_NORMALP;
      h265.sRcParam.eRcMode = AW_VBR;
      h265.sRcParam.sVbrParam.uMaxBitRate =
          static_cast<unsigned int>(bitrate_bps) * 2u;
      h265.sRcParam.sVbrParam.nMovingTh = 20;
      h265.sRcParam.sVbrParam.nQuality = 10;

      int ret = g_api.VideoEncSetParameter(enc_, VENC_IndexParamH265Param,
                                           &h265);
      if (ret != 0) {
        set_last_error("CedarC VE2 SetParameter(H265Param) failed: " +
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
    base.nStride = static_cast<unsigned int>(stride);
    base.eInputFormat = fmt;

    int ret = g_api.VideoEncInit(enc_, &base);
    if (ret != 0) {
      set_last_error("CedarC VE2 VideoEncInit failed: " + std::to_string(ret));
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

    enc_fmt_ = fmt;
    enc_stride_ = stride;
    return true;
  }

  void cleanup() {
    if (enc_) {
      g_api.VideoEncDestroy(enc_);
      enc_ = nullptr;
    }
    if (dec_) {
      g_api.DestroyVideoDecoder(dec_);
      dec_ = nullptr;
    }
    if (memops_opened_ && memops_ && memops_->close) {
      memops_->close();
      memops_opened_ = false;
    }
  }
};

} // namespace

extern "C" int cedarc_mjpeg_h26x_supported(void) {
  const bool has_ve1 = (access("/dev/cedar_dev", F_OK) == 0);
  const bool has_ve2 = (access("/dev/cedar_dev_ve2", F_OK) == 0);
  if (!has_ve1) {
    return -1;
  }

  const bool ve1_ok = (access("/dev/cedar_dev", R_OK | W_OK) == 0);
  const bool ve2_ok =
      !has_ve2 || (access("/dev/cedar_dev_ve2", R_OK | W_OK) == 0);
  const bool mem_ok = (access("/dev/dma_heap/system", R_OK | W_OK) == 0) ||
                      (access("/dev/ion", R_OK | W_OK) == 0);
  if (!ve1_ok || !ve2_ok || !mem_ok) {
    return -1;
  }

  if (!load_cedarc_mjpeg_api()) {
    return -1;
  }

  return 0;
}

extern "C" void *cedarc_mjpeg_h26x_new(int codec, int width, int height,
                                       int fps, int kbps, int gop) {
  try {
    auto *p = new CedarcMjpegH26xPipeline(codec, width, height, fps, kbps, gop);
    if (p->init()) {
      return p;
    }
    delete p;
    return nullptr;
  } catch (const std::exception &e) {
    set_last_error(std::string("cedarc_mjpeg_h26x_new exception: ") + e.what());
    return nullptr;
  }
}

extern "C" int cedarc_mjpeg_h26x_encode(void *pipeline,
                                        const uint8_t *jpeg_data,
                                        size_t jpeg_len, int64_t pts_us,
                                        int force_idr, const uint8_t **out_data,
                                        size_t *out_size, int *out_keyframe) {
  if (!pipeline || !out_data || !out_size || !out_keyframe) {
    set_last_error("Invalid arguments to cedarc_mjpeg_h26x_encode");
    return -1;
  }
  try {
    return static_cast<CedarcMjpegH26xPipeline *>(pipeline)->encode(
        jpeg_data, jpeg_len, pts_us, force_idr, out_data, out_size,
        out_keyframe);
  } catch (const std::exception &e) {
    set_last_error(std::string("cedarc_mjpeg_h26x_encode exception: ") +
                   e.what());
    return -1;
  }
}

extern "C" int cedarc_mjpeg_h26x_reconfigure(void *pipeline, int kbps,
                                             int gop) {
  if (!pipeline) {
    set_last_error("Invalid pipeline pointer");
    return -1;
  }
  try {
    return static_cast<CedarcMjpegH26xPipeline *>(pipeline)->reconfigure(kbps,
                                                                         gop);
  } catch (const std::exception &e) {
    set_last_error(std::string("cedarc_mjpeg_h26x_reconfigure exception: ") +
                   e.what());
    return -1;
  }
}

extern "C" void cedarc_mjpeg_h26x_free(void *pipeline) {
  if (!pipeline) {
    return;
  }
  try {
    delete static_cast<CedarcMjpegH26xPipeline *>(pipeline);
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("cedarc_mjpeg_h26x_free failed: ") + e.what());
  }
}

extern "C" const char *cedarc_mjpeg_h26x_last_error(void) {
  return g_last_error.c_str();
}
