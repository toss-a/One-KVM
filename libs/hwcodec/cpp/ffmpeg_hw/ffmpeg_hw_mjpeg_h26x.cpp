extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixdesc.h>
#include <libavutil/opt.h>
}

#include <string>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <new>

#define LOG_MODULE "FFMPEG_HW"
#include "../common/log.h"

#include "ffmpeg_hw_ffi.h"

namespace {
thread_local std::string g_last_error;

static void set_last_error(const std::string &msg) {
  g_last_error = msg;
  LOG_ERROR(msg);
}

static std::string make_err(const std::string &ctx, int err) {
  return ctx + " (ret=" + std::to_string(err) + "): " + av_err2str(err);
}

static const char* pix_fmt_name(AVPixelFormat fmt) {
  const char *name = av_get_pix_fmt_name(fmt);
  return name ? name : "unknown";
}
} // namespace

// Base pipeline interface
struct FfmpegHwMjpegH26x {
  virtual ~FfmpegHwMjpegH26x() = default;
  virtual int encode(const uint8_t* data, int len, int64_t pts_ms,
                     uint8_t** out_data, int* out_len, int* out_keyframe) = 0;
  virtual int reconfigure(int bitrate_kbps, int gop) = 0;
  virtual int request_keyframe() = 0;
};

#if defined(__linux__) && __has_include(<rockchip/rk_mpi.h>)
#define HAVE_ROCKCHIP_MPP 1
extern "C" {
#include <rockchip/rk_type.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/mpp_meta.h>
#include <rockchip/rk_venc_cfg.h>
#include <rockchip/rk_venc_rc.h>
}

// Native Rockchip MPP zero-copy pipeline:
// Decodes MJPEG directly to NV12 in hardware (VPU), and passes the MppFrame
// directly into the hardware encoder (VEPU) without intermediate format conversion or CPU copy.
struct RkmppNativeMjpegH26xCtx : public FfmpegHwMjpegH26x {
  std::string enc_name;
  int width = 0;
  int height = 0;
  int stride_w = 0;
  int stride_h = 0;
  int fps = 30;
  int bitrate_kbps = 2000;
  int gop = 60;
  bool is_hevc = false;
  bool force_keyframe = false;

  MppCtx dec_ctx = nullptr;
  MppApi *dec_api = nullptr;
  MppBufferGroup dec_pkt_grp = nullptr;
  MppBufferGroup dec_frm_grp = nullptr;

  static constexpr int NUM_DEC_FRAMES = 2;
  MppBuffer dec_frm_bufs[NUM_DEC_FRAMES] = {nullptr, nullptr};
  MppFrame dec_frames[NUM_DEC_FRAMES] = {nullptr, nullptr};
  int dec_frame_idx = 0;

  MppCtx enc_ctx = nullptr;
  MppApi *enc_api = nullptr;
  MppEncCfg enc_cfg = nullptr;
  MppBufferGroup enc_grp = nullptr;
  MppBuffer enc_pkt_buf = nullptr;

  RkmppNativeMjpegH26xCtx(const std::string &enc, int w, int h, int f, int br, int g)
      : enc_name(enc), width(w), height(h), fps(f > 0 ? f : 30),
        bitrate_kbps(br > 0 ? br : 2000), gop(g > 0 ? g : fps) {
    stride_w = (width + 15) & ~15;
    stride_h = (height + 15) & ~15;
    is_hevc = (enc_name.find("hevc") != std::string::npos ||
               enc_name.find("265") != std::string::npos);
  }

  static RK_U32 calculate_buffer_size(int w, int h) {
    int sw = (w + 15) & ~15;
    int sh = (h + 15) & ~15;
    RK_U32 sz = static_cast<RK_U32>(sw) * sh * 22 / 10;
    if (sz < 1024 * 1024) sz = 1024 * 1024;
    if (sz > 20 * 1024 * 1024) sz = 20 * 1024 * 1024;
    return sz;
  }

  int init() {
    // 1. Decoder initialization
    if (mpp_create(&dec_ctx, &dec_api) != MPP_OK) {
      set_last_error("Native MPP: mpp_create decoder failed");
      return -1;
    }
    if (mpp_init(dec_ctx, MPP_CTX_DEC, MPP_VIDEO_CodingMJPEG) != MPP_OK) {
      set_last_error("Native MPP: mpp_init decoder MJPEG failed");
      return -1;
    }

    RK_S32 timeout = 50;
    dec_api->control(dec_ctx, MPP_SET_OUTPUT_TIMEOUT, &timeout);
    dec_api->control(dec_ctx, MPP_SET_INPUT_TIMEOUT, &timeout);

    MppDecCfg dec_cfg = nullptr;
    if (mpp_dec_cfg_init(&dec_cfg) == MPP_OK &&
        dec_api->control(dec_ctx, MPP_DEC_GET_CFG, dec_cfg) == MPP_OK) {
      mpp_dec_cfg_set_u32(dec_cfg, "base:split_parse", 1);
      dec_api->control(dec_ctx, MPP_DEC_SET_CFG, dec_cfg);
      mpp_dec_cfg_deinit(dec_cfg);
    }

    // Force hardware VPU to decode into NV12 (YUV420SP) directly
    MppFrameFormat dec_fmt = MPP_FMT_YUV420SP;
    dec_api->control(dec_ctx, MPP_DEC_SET_OUTPUT_FORMAT, &dec_fmt);

    if (mpp_buffer_group_get_internal(&dec_pkt_grp, MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_CACHABLE) != MPP_OK &&
        mpp_buffer_group_get_internal(&dec_pkt_grp, MPP_BUFFER_TYPE_ION) != MPP_OK &&
        mpp_buffer_group_get_internal(&dec_pkt_grp, MPP_BUFFER_TYPE_NORMAL) != MPP_OK) {
      set_last_error("Native MPP: dec_pkt_grp get failed");
      return -1;
    }

    if (mpp_buffer_group_get_internal(&dec_frm_grp, MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_CACHABLE) != MPP_OK &&
        mpp_buffer_group_get_internal(&dec_frm_grp, MPP_BUFFER_TYPE_ION) != MPP_OK &&
        mpp_buffer_group_get_internal(&dec_frm_grp, MPP_BUFFER_TYPE_NORMAL) != MPP_OK) {
      set_last_error("Native MPP: dec_frm_grp get failed");
      return -1;
    }

    RK_U32 frm_buf_size = calculate_buffer_size(width, height);
    for (int i = 0; i < NUM_DEC_FRAMES; ++i) {
      if (mpp_buffer_get(dec_frm_grp, &dec_frm_bufs[i], frm_buf_size) != MPP_OK ||
          mpp_frame_init(&dec_frames[i]) != MPP_OK) {
        set_last_error("Native MPP: dec_frm_buf / dec_frame alloc failed");
        return -1;
      }
      mpp_frame_set_buffer(dec_frames[i], dec_frm_bufs[i]);
      mpp_frame_set_width(dec_frames[i], width);
      mpp_frame_set_height(dec_frames[i], height);
      mpp_frame_set_hor_stride(dec_frames[i], stride_w);
      mpp_frame_set_ver_stride(dec_frames[i], stride_h);
      mpp_frame_set_fmt(dec_frames[i], MPP_FMT_YUV420SP);
    }

    // 2. Encoder initialization
    MppCodingType enc_coding = is_hevc ? MPP_VIDEO_CodingHEVC : MPP_VIDEO_CodingAVC;
    if (mpp_create(&enc_ctx, &enc_api) != MPP_OK) {
      set_last_error("Native MPP: mpp_create encoder failed");
      return -1;
    }
    if (mpp_init(enc_ctx, MPP_CTX_ENC, enc_coding) != MPP_OK) {
      set_last_error("Native MPP: mpp_init encoder failed");
      return -1;
    }

    if (mpp_enc_cfg_init(&enc_cfg) != MPP_OK ||
        enc_api->control(enc_ctx, MPP_ENC_GET_CFG, enc_cfg) != MPP_OK) {
      set_last_error("Native MPP: enc_cfg init failed");
      return -1;
    }

    mpp_enc_cfg_set_s32(enc_cfg, "prep:width", width);
    mpp_enc_cfg_set_s32(enc_cfg, "prep:height", height);
    mpp_enc_cfg_set_s32(enc_cfg, "prep:hor_stride", stride_w);
    mpp_enc_cfg_set_s32(enc_cfg, "prep:ver_stride", stride_h);
    mpp_enc_cfg_set_s32(enc_cfg, "prep:format", MPP_FMT_YUV420SP);

    int bps = bitrate_kbps * 1000;
    mpp_enc_cfg_set_u32(enc_cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:bps_target", bps);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:bps_max", bps * 105 / 100);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:bps_min", bps * 95 / 100);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_in_num", fps);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_in_denom", 1);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_out_num", fps);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_out_denom", 1);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:gop", gop > 0 ? gop : fps);

    if (is_hevc) {
      mpp_enc_cfg_set_s32(enc_cfg, "h265:profile", 1);
      mpp_enc_cfg_set_s32(enc_cfg, "h265:level", 120);
    } else {
      mpp_enc_cfg_set_s32(enc_cfg, "h264:profile", 66);
      mpp_enc_cfg_set_s32(enc_cfg, "h264:level", 41);
      mpp_enc_cfg_set_s32(enc_cfg, "h264:cabac_en", 0);
    }

    if (enc_api->control(enc_ctx, MPP_ENC_SET_CFG, enc_cfg) != MPP_OK) {
      set_last_error("Native MPP: enc_api set cfg failed");
      return -1;
    }

    MppEncHeaderMode hdr = MPP_ENC_HEADER_MODE_EACH_IDR;
    enc_api->control(enc_ctx, MPP_ENC_SET_HEADER_MODE, &hdr);

    if (mpp_buffer_group_get_internal(&enc_grp, MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_CACHABLE) != MPP_OK &&
        mpp_buffer_group_get_internal(&enc_grp, MPP_BUFFER_TYPE_ION) != MPP_OK &&
        mpp_buffer_group_get_internal(&enc_grp, MPP_BUFFER_TYPE_NORMAL) != MPP_OK) {
      set_last_error("Native MPP: enc_grp get failed");
      return -1;
    }
    size_t pkt_buf_size = static_cast<size_t>(stride_w) * stride_h;
    if (pkt_buf_size < 1024 * 1024) pkt_buf_size = 1024 * 1024;
    if (mpp_buffer_get(enc_grp, &enc_pkt_buf, pkt_buf_size) != MPP_OK) {
      set_last_error("Native MPP: enc_pkt_buf get failed");
      return -1;
    }

    return 0;
  }

  int encode(const uint8_t* data, int len, int64_t pts_ms,
             uint8_t** out_data, int* out_len, int* out_keyframe) override {
    if (!data || len < 4 || !out_data || !out_len || !out_keyframe) {
      set_last_error("Invalid parameters for native encode");
      return -1;
    }
    *out_data = nullptr;
    *out_len = 0;
    *out_keyframe = 0;

    MppBuffer inbuf = nullptr;
    if (mpp_buffer_get(dec_pkt_grp, &inbuf, static_cast<RK_U32>(len)) != MPP_OK) {
      set_last_error("Native MPP: dec_pkt_grp buffer get failed");
      return -1;
    }
    void *ptr = mpp_buffer_get_ptr(inbuf);
    if (!ptr) {
      mpp_buffer_put(inbuf);
      set_last_error("Native MPP: inbuf get ptr failed");
      return -1;
    }
    memcpy(ptr, data, len);

    MppPacket pkt = nullptr;
    mpp_packet_init_with_buffer(&pkt, inbuf);
    mpp_buffer_put(inbuf);
    mpp_packet_set_data(pkt, ptr);
    mpp_packet_set_pos(pkt, ptr);
    mpp_packet_set_size(pkt, len);
    mpp_packet_set_length(pkt, len);

    MppFrame target_frame = dec_frames[dec_frame_idx];
    dec_frame_idx = (dec_frame_idx + 1) % NUM_DEC_FRAMES;
    MppMeta meta = mpp_packet_get_meta(pkt);
    if (meta) {
      mpp_meta_set_frame(meta, KEY_OUTPUT_FRAME, target_frame);
    }

    if (dec_api->decode_put_packet(dec_ctx, pkt) != MPP_OK) {
      mpp_packet_deinit(&pkt);
      set_last_error("Native MPP: decode_put_packet failed");
      return -1;
    }

    MppFrame frame_out = nullptr;
    if (dec_api->decode_get_frame(dec_ctx, &frame_out) != MPP_OK || !frame_out ||
        mpp_frame_get_errinfo(frame_out) || mpp_frame_get_discard(frame_out)) {
      mpp_packet_deinit(&pkt);
      if (frame_out) mpp_frame_deinit(&frame_out);
      set_last_error("Native MPP: decode_get_frame failed or frame discarded");
      return -1;
    }
    mpp_packet_deinit(&pkt);

    // Direct zero-copy handoff from VPU hardware decoder into VEPU hardware encoder
    mpp_frame_set_eos(frame_out, 0);
    mpp_frame_set_pts(frame_out, pts_ms * 1000);

    if (force_keyframe) {
      enc_api->control(enc_ctx, MPP_ENC_SET_IDR_FRAME, nullptr);
      force_keyframe = false;
    }

    MppPacket enc_packet = nullptr;
    if (mpp_packet_init_with_buffer(&enc_packet, enc_pkt_buf) != MPP_OK) {
      mpp_frame_deinit(&frame_out);
      set_last_error("Native MPP: enc_packet init failed");
      return -1;
    }
    mpp_packet_set_length(enc_packet, 0);

    MppMeta meta_enc = mpp_frame_get_meta(frame_out);
    if (meta_enc) {
      mpp_meta_set_packet(meta_enc, KEY_OUTPUT_PACKET, enc_packet);
      mpp_meta_set_buffer(meta_enc, KEY_MOTION_INFO, nullptr);
    }

    MPP_RET put_ret = enc_api->encode_put_frame(enc_ctx, frame_out);
    // Release decoder output frame handle after handoff to avoid memory leak
    mpp_frame_deinit(&frame_out);

    if (put_ret != MPP_OK) {
      mpp_packet_deinit(&enc_packet);
      set_last_error("Native MPP: encode_put_frame failed");
      return -1;
    }

    MppPacket out_pkt = nullptr;
    MPP_RET ret = enc_api->encode_get_packet(enc_ctx, &out_pkt);
    if (ret == MPP_ERR_TIMEOUT || !out_pkt) {
      mpp_packet_deinit(&enc_packet);
      return 0; // Pipelined mode: waiting for next frame
    }
    if (ret != MPP_OK) {
      mpp_packet_deinit(&enc_packet);
      set_last_error("Native MPP: encode_get_packet failed");
      return -1;
    }

    void *pkt_data = mpp_packet_get_data(out_pkt);
    size_t pkt_len = mpp_packet_get_length(out_pkt);
    if (pkt_data && pkt_len > 0) {
      uint8_t *buf = static_cast<uint8_t*>(malloc(pkt_len));
      if (!buf) {
        if (out_pkt == enc_packet) enc_packet = nullptr;
        mpp_packet_deinit(&out_pkt);
        if (enc_packet) mpp_packet_deinit(&enc_packet);
        set_last_error("malloc for output packet failed");
        return -1;
      }
      memcpy(buf, pkt_data, pkt_len);
      *out_data = buf;
      *out_len = static_cast<int>(pkt_len);

      int is_key = mpp_packet_is_intra(out_pkt) ? 1 : 0;
      if (!is_key && pkt_len >= 5) {
        for (size_t i = 0; i + 4 < pkt_len && i < 64; ++i) {
          int nal_offset = 0;
          if (buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 0 && buf[i+3] == 1) {
            nal_offset = i + 4;
          } else if (buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 1) {
            nal_offset = i + 3;
          }
          if (nal_offset > 0 && nal_offset < static_cast<int>(pkt_len)) {
            if (!is_hevc) {
              uint8_t nal_type = buf[nal_offset] & 0x1F;
              if (nal_type == 5 || nal_type == 7) {
                is_key = 1;
                break;
              }
            } else {
              uint8_t nal_type = (buf[nal_offset] >> 1) & 0x3F;
              if (nal_type >= 16 && nal_type <= 21) {
                is_key = 1;
                break;
              }
            }
          }
        }
      }
      *out_keyframe = is_key;
    }

    if (out_pkt == enc_packet) enc_packet = nullptr;
    mpp_packet_deinit(&out_pkt);
    if (enc_packet) mpp_packet_deinit(&enc_packet);
    return 1;
  }

  int reconfigure(int br_kbps, int g) override {
    if (!enc_ctx || !enc_api || !enc_cfg) return -1;
    bitrate_kbps = br_kbps > 0 ? br_kbps : bitrate_kbps;
    gop = g > 0 ? g : gop;
    int bps = bitrate_kbps * 1000;
    mpp_enc_cfg_set_s32(enc_cfg, "rc:bps_target", bps);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:bps_max", bps * 105 / 100);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:bps_min", bps * 95 / 100);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:gop", gop > 0 ? gop : fps);
    return enc_api->control(enc_ctx, MPP_ENC_SET_CFG, enc_cfg) == MPP_OK ? 0 : -1;
  }

  int request_keyframe() override {
    force_keyframe = true;
    return 0;
  }

  ~RkmppNativeMjpegH26xCtx() override {
    for (int i = 0; i < NUM_DEC_FRAMES; ++i) {
      if (dec_frames[i]) { mpp_frame_deinit(&dec_frames[i]); dec_frames[i] = nullptr; }
      if (dec_frm_bufs[i]) { mpp_buffer_put(dec_frm_bufs[i]); dec_frm_bufs[i] = nullptr; }
    }
    if (dec_frm_grp) { mpp_buffer_group_put(dec_frm_grp); dec_frm_grp = nullptr; }
    if (dec_pkt_grp) { mpp_buffer_group_put(dec_pkt_grp); dec_pkt_grp = nullptr; }
    if (dec_ctx && dec_api) { dec_api->reset(dec_ctx); mpp_destroy(dec_ctx); dec_ctx = nullptr; dec_api = nullptr; }

    if (enc_pkt_buf) { mpp_buffer_put(enc_pkt_buf); enc_pkt_buf = nullptr; }
    if (enc_grp) { mpp_buffer_group_put(enc_grp); enc_grp = nullptr; }
    if (enc_cfg) { mpp_enc_cfg_deinit(enc_cfg); enc_cfg = nullptr; }
    if (enc_ctx && enc_api) { enc_api->reset(enc_ctx); mpp_destroy(enc_ctx); enc_ctx = nullptr; enc_api = nullptr; }
  }
};
#endif // HAVE_ROCKCHIP_MPP

// Standard FFmpeg HW pipeline implementation
struct FfmpegHwMjpegH26xCtx : public FfmpegHwMjpegH26x {
  AVCodecContext *dec_ctx = nullptr;
  AVCodecContext *enc_ctx = nullptr;
  AVPacket *dec_pkt = nullptr;
  AVFrame *dec_frame = nullptr;
  AVPacket *enc_pkt = nullptr;
  AVBufferRef *hw_device_ctx = nullptr;
  AVBufferRef *hw_frames_ctx = nullptr;
  AVPixelFormat hw_pixfmt = AV_PIX_FMT_NONE;
  std::string dec_name;
  std::string enc_name;
  int width = 0;
  int height = 0;
  int aligned_width = 0;
  int aligned_height = 0;
  int fps = 30;
  int bitrate_kbps = 2000;
  int gop = 60;
  int thread_count = 1;
  bool force_keyframe = false;

  FfmpegHwMjpegH26xCtx(const char* dec, const char* enc, int w, int h, int f, int br, int g, int tc)
      : dec_name(dec), enc_name(enc), width(w), height(h), aligned_width(w), aligned_height(h),
        fps(f > 0 ? f : 30), bitrate_kbps(br > 0 ? br : 2000), gop(g > 0 ? g : fps), thread_count(tc > 0 ? tc : 1) {}

  static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
    auto *self = reinterpret_cast<FfmpegHwMjpegH26xCtx *>(ctx->opaque);
    if (self && self->hw_pixfmt != AV_PIX_FMT_NONE) {
      for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == self->hw_pixfmt) return *p;
      }
    }
    return pix_fmts[0];
  }

  int init_decoder() {
    const AVCodec *dec = avcodec_find_decoder_by_name(dec_name.c_str());
    if (!dec) {
      set_last_error("Decoder not found: " + dec_name);
      return -1;
    }

    dec_ctx = avcodec_alloc_context3(dec);
    if (!dec_ctx) {
      set_last_error("Failed to allocate decoder context");
      return -1;
    }

    dec_ctx->width = width;
    dec_ctx->height = height;
    dec_ctx->thread_count = thread_count > 0 ? thread_count : 1;
    dec_ctx->opaque = this;

    const AVCodecHWConfig *cfg = nullptr;
    for (int i = 0; (cfg = avcodec_get_hw_config(dec, i)); i++) {
      if (cfg->device_type == AV_HWDEVICE_TYPE_RKMPP) {
        hw_pixfmt = cfg->pix_fmt;
        break;
      }
    }
    if (hw_pixfmt == AV_PIX_FMT_NONE) {
      set_last_error("No RKMPP hw pixfmt for decoder");
      return -1;
    }

    int ret = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_RKMPP, NULL, NULL, 0);
    if (ret < 0) {
      set_last_error(make_err("av_hwdevice_ctx_create failed", ret));
      return -1;
    }

    dec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
    dec_ctx->get_format = get_hw_format;
    dec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;

    ret = avcodec_open2(dec_ctx, dec, NULL);
    if (ret < 0) {
      set_last_error(make_err("avcodec_open2 decoder failed", ret));
      return -1;
    }

    dec_pkt = av_packet_alloc();
    dec_frame = av_frame_alloc();
    enc_pkt = av_packet_alloc();
    if (!dec_pkt || !dec_frame || !enc_pkt) {
      set_last_error("Failed to allocate packet/frame");
      return -1;
    }
    return 0;
  }

  int init_encoder(AVBufferRef *frames_ctx) {
    const AVCodec *enc = avcodec_find_encoder_by_name(enc_name.c_str());
    if (!enc) {
      set_last_error("Encoder not found: " + enc_name);
      return -1;
    }
    enc_ctx = avcodec_alloc_context3(enc);
    if (!enc_ctx) {
      set_last_error("Failed to allocate encoder context");
      return -1;
    }
    enc_ctx->width = width;
    enc_ctx->height = height;
    enc_ctx->coded_width = width;
    enc_ctx->coded_height = height;
    aligned_width = width;
    aligned_height = height;
    enc_ctx->time_base = AVRational{1, 1000};
    enc_ctx->framerate = AVRational{fps, 1};
    enc_ctx->bit_rate = (int64_t)bitrate_kbps * 1000;
    enc_ctx->gop_size = gop > 0 ? gop : fps;
    enc_ctx->max_b_frames = 0;
    enc_ctx->pix_fmt = AV_PIX_FMT_DRM_PRIME;
    enc_ctx->sw_pix_fmt = AV_PIX_FMT_NV12;

    if (frames_ctx) {
      AVHWFramesContext *hwfc = reinterpret_cast<AVHWFramesContext *>(frames_ctx->data);
      if (hwfc) {
        enc_ctx->pix_fmt = static_cast<AVPixelFormat>(hwfc->format);
        enc_ctx->sw_pix_fmt = static_cast<AVPixelFormat>(hwfc->sw_format);
        if (hwfc->width > 0) {
          aligned_width = hwfc->width;
          enc_ctx->coded_width = hwfc->width;
        }
        if (hwfc->height > 0) {
          aligned_height = hwfc->height;
          enc_ctx->coded_height = hwfc->height;
        }
      }
      hw_frames_ctx = av_buffer_ref(frames_ctx);
      enc_ctx->hw_frames_ctx = av_buffer_ref(frames_ctx);
    }
    if (hw_device_ctx) {
      enc_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
    }

    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "rc_mode", "CBR", 0);
    if (enc->id == AV_CODEC_ID_H264) {
      av_dict_set(&opts, "profile", "high", 0);
    } else if (enc->id == AV_CODEC_ID_HEVC) {
      av_dict_set(&opts, "profile", "main", 0);
    }
    av_dict_set_int(&opts, "qp_init", 23, 0);
    av_dict_set_int(&opts, "qp_max", 48, 0);
    av_dict_set_int(&opts, "qp_min", 0, 0);
    av_dict_set_int(&opts, "qp_max_i", 48, 0);
    av_dict_set_int(&opts, "qp_min_i", 0, 0);
    int ret = avcodec_open2(enc_ctx, enc, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
      std::string detail = "avcodec_open2 encoder failed: ";
      detail += enc_name;
      detail += " fmt=" + std::string(pix_fmt_name(enc_ctx->pix_fmt));
      detail += " sw=" + std::string(pix_fmt_name(enc_ctx->sw_pix_fmt));
      detail += " size=" + std::to_string(enc_ctx->width) + "x" + std::to_string(enc_ctx->height);
      detail += " fps=" + std::to_string(fps);
      set_last_error(make_err(detail, ret));
      avcodec_free_context(&enc_ctx);
      enc_ctx = nullptr;
      if (hw_frames_ctx) {
        av_buffer_unref(&hw_frames_ctx);
        hw_frames_ctx = nullptr;
      }
      return -1;
    }
    return 0;
  }

  void free_encoder() {
    if (enc_ctx) {
      avcodec_free_context(&enc_ctx);
      enc_ctx = nullptr;
    }
    if (hw_frames_ctx) {
      av_buffer_unref(&hw_frames_ctx);
      hw_frames_ctx = nullptr;
    }
  }

  int init() {
    return init_decoder();
  }

  int encode(const uint8_t* data, int len, int64_t pts_ms,
             uint8_t** out_data, int* out_len, int* out_keyframe) override {
    if (!data || len <= 0 || !out_data || !out_len || !out_keyframe) {
      set_last_error("Invalid parameters for encode");
      return -1;
    }
    *out_data = nullptr;
    *out_len = 0;
    *out_keyframe = 0;
    bool encoded = false;

    av_packet_unref(dec_pkt);
    int ret = av_new_packet(dec_pkt, len);
    if (ret < 0) {
      set_last_error(make_err("av_new_packet failed", ret));
      return -1;
    }
    memcpy(dec_pkt->data, data, len);
    dec_pkt->size = len;

    auto drain_and_encode = [&]() -> int {
      while (true) {
        int r = avcodec_receive_frame(dec_ctx, dec_frame);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
          return 0;
        }
        if (r < 0) {
          set_last_error(make_err("avcodec_receive_frame failed", r));
          return -1;
        }

        if (dec_frame->format != AV_PIX_FMT_DRM_PRIME) {
          set_last_error("Decoder output is not DRM_PRIME");
          av_frame_unref(dec_frame);
          return -1;
        }

        if (!enc_ctx) {
          if (!dec_frame->hw_frames_ctx) {
            set_last_error("Decoder returned frame without hw_frames_ctx");
            av_frame_unref(dec_frame);
            return -1;
          }
          if (init_encoder(dec_frame->hw_frames_ctx) != 0) {
            av_frame_unref(dec_frame);
            return -1;
          }
        }

        AVFrame *send_frame = dec_frame;
        AVFrame *tmp = nullptr;
        if (force_keyframe) {
          tmp = av_frame_clone(send_frame);
          if (tmp) {
            tmp->pict_type = AV_PICTURE_TYPE_I;
            send_frame = tmp;
          }
          force_keyframe = false;
        }

        if (aligned_width > 0 && width > 0 && aligned_width > width) {
          send_frame->crop_right = aligned_width - width;
        }
        if (aligned_height > 0 && height > 0 && aligned_height > height) {
          send_frame->crop_bottom = aligned_height - height;
        }

        send_frame->pts = pts_ms;

        r = avcodec_send_frame(enc_ctx, send_frame);
        if (tmp) {
          av_frame_free(&tmp);
        }
        if (r < 0) {
          std::string detail = "avcodec_send_frame failed";
          if (send_frame) {
            detail += " frame_fmt=";
            detail += pix_fmt_name(static_cast<AVPixelFormat>(send_frame->format));
            detail += " w=" + std::to_string(send_frame->width);
            detail += " h=" + std::to_string(send_frame->height);
            if (send_frame->format == AV_PIX_FMT_DRM_PRIME && send_frame->data[0]) {
              const AVDRMFrameDescriptor *drm =
                  reinterpret_cast<const AVDRMFrameDescriptor *>(send_frame->data[0]);
              if (drm && drm->layers[0].format) {
                detail += " drm_fmt=0x";
                char buf[9];
                snprintf(buf, sizeof(buf), "%08x", drm->layers[0].format);
                detail += buf;
              }
              if (drm && drm->objects[0].format_modifier) {
                detail += " drm_mod=0x";
                char buf[17];
                snprintf(buf, sizeof(buf), "%016llx",
                         (unsigned long long)drm->objects[0].format_modifier);
                detail += buf;
              }
            }
          }
          set_last_error(make_err(detail, r));
          av_frame_unref(dec_frame);
          return -1;
        }

        while (true) {
          av_packet_unref(enc_pkt);
          r = avcodec_receive_packet(enc_ctx, enc_pkt);
          if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
            break;
          }
          if (r < 0) {
            set_last_error(make_err("avcodec_receive_packet failed", r));
            av_packet_unref(enc_pkt);
            av_frame_unref(dec_frame);
            return -1;
          }

          if (enc_pkt->size <= 0) {
            set_last_error("avcodec_receive_packet failed, pkt size is 0");
            av_packet_unref(enc_pkt);
            av_frame_unref(dec_frame);
            return -1;
          }

          if (!encoded) {
            uint8_t *buf = (uint8_t*)malloc(enc_pkt->size);
            if (!buf) {
              set_last_error("malloc for output packet failed");
              av_packet_unref(enc_pkt);
              av_frame_unref(dec_frame);
              return -1;
            }
            memcpy(buf, enc_pkt->data, enc_pkt->size);
            *out_data = buf;
            *out_len = enc_pkt->size;
            *out_keyframe = (enc_pkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0;
            encoded = true;
          }
        }

        av_frame_unref(dec_frame);
      }
      return 0;
    };

    ret = avcodec_send_packet(dec_ctx, dec_pkt);
    if (ret == AVERROR(EAGAIN)) {
      if (drain_and_encode() < 0) {
        return -1;
      }
      ret = avcodec_send_packet(dec_ctx, dec_pkt);
    }
    if (ret < 0) {
      set_last_error(make_err("avcodec_send_packet failed", ret));
      return -1;
    }

    if (drain_and_encode() < 0) {
      return -1;
    }

    return encoded ? 1 : 0;
  }

  int reconfigure(int br, int g) override {
    if (!enc_ctx || !hw_frames_ctx) {
      set_last_error("Encoder not initialized for reconfigure");
      return -1;
    }
    bitrate_kbps = br > 0 ? br : bitrate_kbps;
    gop = g > 0 ? g : gop;

    AVBufferRef *frames_ref = hw_frames_ctx ? av_buffer_ref(hw_frames_ctx) : nullptr;
    free_encoder();
    int ret = init_encoder(frames_ref);
    if (frames_ref) av_buffer_unref(&frames_ref);
    return ret;
  }

  int request_keyframe() override {
    force_keyframe = true;
    return 0;
  }

  ~FfmpegHwMjpegH26xCtx() override {
    if (dec_pkt) av_packet_free(&dec_pkt);
    if (dec_frame) av_frame_free(&dec_frame);
    if (enc_pkt) av_packet_free(&enc_pkt);
    if (dec_ctx) avcodec_free_context(&dec_ctx);
    free_encoder();
    if (hw_device_ctx) av_buffer_unref(&hw_device_ctx);
  }
};

extern "C" FfmpegHwMjpegH26x* ffmpeg_hw_mjpeg_h26x_new(const char* dec_name,
                                                        const char* enc_name,
                                                        int width,
                                                        int height,
                                                        int fps,
                                                        int bitrate_kbps,
                                                        int gop,
                                                        int thread_count) {
  if (!dec_name || !enc_name || width <= 0 || height <= 0) {
    set_last_error("Invalid parameters for ffmpeg_hw_mjpeg_h26x_new");
    return nullptr;
  }

#if defined(HAVE_ROCKCHIP_MPP)
  if (std::string(dec_name).find("rkmpp") != std::string::npos &&
      std::string(enc_name).find("rkmpp") != std::string::npos) {
    auto *mpp_ctx = new (std::nothrow) RkmppNativeMjpegH26xCtx(
        enc_name, width, height, fps, bitrate_kbps, gop);
    if (mpp_ctx) {
      if (mpp_ctx->init() == 0) {
        LOG_INFO("Using native Rockchip MPP zero-copy hardware pipeline (VPU NV12 -> VEPU H.26x)");
        return mpp_ctx;
      }
      delete mpp_ctx;
      LOG_WARN("Native Rockchip MPP pipeline init failed, falling back to FFmpeg");
    }
  }
#endif

  auto *ctx = new (std::nothrow) FfmpegHwMjpegH26xCtx(
      dec_name, enc_name, width, height, fps, bitrate_kbps, gop, thread_count);
  if (ctx) {
    if (ctx->init() == 0) {
      return ctx;
    }
    delete ctx;
  }
  return nullptr;
}

extern "C" int ffmpeg_hw_mjpeg_h26x_encode(FfmpegHwMjpegH26x* handle,
                                           const uint8_t* data,
                                           int len,
                                           int64_t pts_ms,
                                           uint8_t** out_data,
                                           int* out_len,
                                           int* out_keyframe) {
  if (!handle) {
    set_last_error("Invalid handle for encode");
    return -1;
  }
  return handle->encode(data, len, pts_ms, out_data, out_len, out_keyframe);
}

extern "C" int ffmpeg_hw_mjpeg_h26x_reconfigure(FfmpegHwMjpegH26x* handle,
                                                int bitrate_kbps,
                                                int gop) {
  if (!handle) {
    set_last_error("Invalid handle for reconfigure");
    return -1;
  }
  return handle->reconfigure(bitrate_kbps, gop);
}

extern "C" int ffmpeg_hw_mjpeg_h26x_request_keyframe(FfmpegHwMjpegH26x* handle) {
  if (!handle) {
    set_last_error("Invalid handle for request_keyframe");
    return -1;
  }
  return handle->request_keyframe();
}

extern "C" void ffmpeg_hw_mjpeg_h26x_free(FfmpegHwMjpegH26x* handle) {
  delete handle;
}

extern "C" void ffmpeg_hw_packet_free(uint8_t* data) {
  if (data) {
    free(data);
  }
}

extern "C" const char* ffmpeg_hw_last_error(void) {
  return g_last_error.c_str();
}
