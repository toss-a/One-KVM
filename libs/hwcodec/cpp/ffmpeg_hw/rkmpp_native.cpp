#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

#if defined(__linux__) && __has_include(<rockchip/rk_mpi.h>)
#define HAVE_MPP_NATIVE 1
extern "C" {
#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_meta.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_venc_cfg.h>
#include <rockchip/rk_venc_rc.h>
}
#endif

static thread_local char native_error[192] = {};

static int fail(const char *operation, int code) {
    std::snprintf(native_error, sizeof(native_error), "%s (ret=%d)", operation, code);
    return -1;
}

#ifdef HAVE_MPP_NATIVE
static int alloc_mpp_buffer_group(MppBufferGroup *grp) {
    const MppBufferType types[] = {
        static_cast<MppBufferType>(MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_CACHABLE),
        MPP_BUFFER_TYPE_DRM,
        MPP_BUFFER_TYPE_ION,
        MPP_BUFFER_TYPE_NORMAL,
    };
    int ret = -1;
    for (MppBufferType type : types) {
        ret = mpp_buffer_group_get_internal(grp, type);
        if (ret == MPP_OK && *grp) {
            return MPP_OK;
        }
    }
    return ret;
}

static bool set_cfg_s32(MppEncCfg cfg, const char *key, int value) {
    int ret = mpp_enc_cfg_set_s32(cfg, key, value);
    if (ret) {
        fail(key, ret);
        return false;
    }
    return true;
}

static bool configure_h264_encoder(
    MppCtx ctx,
    MppApi *api,
    MppEncCfg cfg,
    int width,
    int height,
    int hor_stride,
    int ver_stride,
    MppFrameFormat fmt,
    int fps,
    int kbps,
    int gop) {
    const int bps = kbps * 1000;
    const int level = int64_t(width) * height * fps <= int64_t(1920) * 1080 * 60 ? 42 : 52;
    if (!set_cfg_s32(cfg, "prep:width", width) ||
        !set_cfg_s32(cfg, "prep:height", height) ||
        !set_cfg_s32(cfg, "prep:hor_stride", hor_stride) ||
        !set_cfg_s32(cfg, "prep:ver_stride", ver_stride) ||
        !set_cfg_s32(cfg, "prep:format", fmt) ||
        !set_cfg_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_CBR) ||
        !set_cfg_s32(cfg, "rc:bps_target", bps) ||
        !set_cfg_s32(cfg, "rc:bps_max", bps + bps / 16) ||
        !set_cfg_s32(cfg, "rc:bps_min", bps - bps / 16) ||
        !set_cfg_s32(cfg, "rc:fps_in_flex", 0) ||
        !set_cfg_s32(cfg, "rc:fps_in_num", fps) ||
        !set_cfg_s32(cfg, "rc:fps_in_denorm", 1) ||
        !set_cfg_s32(cfg, "rc:fps_out_flex", 0) ||
        !set_cfg_s32(cfg, "rc:fps_out_num", fps) ||
        !set_cfg_s32(cfg, "rc:fps_out_denorm", 1) ||
        !set_cfg_s32(cfg, "rc:gop", gop) ||
        !set_cfg_s32(cfg, "codec:type", MPP_VIDEO_CodingAVC) ||
        !set_cfg_s32(cfg, "h264:profile", 66) ||
        !set_cfg_s32(cfg, "h264:level", level) ||
        !set_cfg_s32(cfg, "h264:cabac_en", 0) ||
        !set_cfg_s32(cfg, "h264:trans8x8", 0)) {
        return false;
    }

    int ret = api->control(ctx, MPP_ENC_SET_CFG, cfg);
    if (ret) {
        fail("MPP_ENC_SET_CFG", ret);
        return false;
    }

    MppEncHeaderMode hdr = MPP_ENC_HEADER_MODE_EACH_IDR;
    (void)api->control(ctx, MPP_ENC_SET_HEADER_MODE, &hdr);
    return true;
}

static bool reconfigure_h264_bitrate(MppCtx ctx, MppApi *api, MppEncCfg cfg, int kbps, int gop) {
    if (kbps <= 0 || kbps > 1000000 || gop <= 0) {
        fail("invalid native MPP bitrate configuration", -1);
        return false;
    }
    const int bps = kbps * 1000;
    if (!set_cfg_s32(cfg, "rc:bps_target", bps) ||
        !set_cfg_s32(cfg, "rc:bps_max", bps + bps / 16) ||
        !set_cfg_s32(cfg, "rc:bps_min", bps - bps / 16) ||
        !set_cfg_s32(cfg, "rc:gop", gop)) {
        return false;
    }
    int ret = api->control(ctx, MPP_ENC_SET_CFG, cfg);
    if (ret) {
        fail("MPP_ENC_SET_CFG", ret);
        return false;
    }
    return true;
}

static int is_h264_idr(const uint8_t *data, size_t len) {
    if (!data || len < 5) return 0;
    for (size_t i = 0; i + 4 < len; ++i) {
        size_t nal_pos = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
            nal_pos = i + 4;
        } else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            nal_pos = i + 3;
        }
        if (nal_pos > 0 && nal_pos < len) {
            uint8_t nal_type = data[nal_pos] & 0x1F;
            if (nal_type == 5) return 1;
        }
    }
    return 0;
}

struct RkmppNativeMjpegH264 {
    MppCtx dec_ctx = nullptr;
    MppApi *dec_api = nullptr;
    MppBufferGroup pkt_grp = nullptr;
    MppBufferGroup frm_grp = nullptr;
    MppBuffer dec_buf = nullptr;
    MppFrame dec_frame = nullptr;

    MppCtx enc_ctx = nullptr;
    MppApi *enc_api = nullptr;
    MppEncCfg enc_cfg = nullptr;
    MppPacket enc_packet = nullptr;

    int width = 0;
    int height = 0;
    int enc_hor_stride = 0;
    int enc_ver_stride = 0;
    MppFrameFormat enc_fmt = MPP_FMT_YUV420SP;
    bool layout_configured = false;

    void close() {
        if (enc_packet) mpp_packet_deinit(&enc_packet);
        if (enc_ctx) {
            if (enc_api) enc_api->reset(enc_ctx);
            mpp_destroy(enc_ctx);
            enc_ctx = nullptr;
        }
        if (enc_cfg) {
            mpp_enc_cfg_deinit(enc_cfg);
            enc_cfg = nullptr;
        }
        if (dec_ctx) {
            if (dec_api) dec_api->reset(dec_ctx);
            mpp_destroy(dec_ctx);
            dec_ctx = nullptr;
        }
        if (dec_frame) mpp_frame_deinit(&dec_frame);
        if (dec_buf) {
            mpp_buffer_put(dec_buf);
            dec_buf = nullptr;
        }
        if (frm_grp) {
            mpp_buffer_group_put(frm_grp);
            frm_grp = nullptr;
        }
        if (pkt_grp) {
            mpp_buffer_group_put(pkt_grp);
            pkt_grp = nullptr;
        }
    }

    ~RkmppNativeMjpegH264() { close(); }
};

extern "C" RkmppNativeMjpegH264 *rkmpp_native_mjpeg_h264_new(
    int width, int height, int fps, int kbps, int gop) {
    if (width <= 0 || height <= 0 || (width & 1) || (height & 1) ||
        fps <= 0 || fps > 240 || kbps <= 0 || gop <= 0) {
        fail("invalid native MJPEG->H264 parameters", -1);
        return nullptr;
    }

    auto *p = new (std::nothrow) RkmppNativeMjpegH264;
    if (!p) {
        fail("allocate native MJPEG->H264", -1);
        return nullptr;
    }
    p->width = width;
    p->height = height;
    const int stride_w = (width + 15) & ~15;
    const int stride_h = (height + 15) & ~15;

    auto abort_init = [p](const char *op, int ret) -> RkmppNativeMjpegH264 * {
        fail(op, ret);
        delete p;
        return nullptr;
    };

    int ret = mpp_create(&p->dec_ctx, &p->dec_api);
    if (ret) return abort_init("mpp_create JPEG decoder", ret);
    RK_S64 dec_timeout = 200;
    (void)p->dec_api->control(p->dec_ctx, MPP_SET_INPUT_TIMEOUT, &dec_timeout);
    (void)p->dec_api->control(p->dec_ctx, MPP_SET_OUTPUT_TIMEOUT, &dec_timeout);

    ret = mpp_init(p->dec_ctx, MPP_CTX_DEC, MPP_VIDEO_CodingMJPEG);
    if (ret) return abort_init("mpp_init JPEG decoder", ret);

    MppDecCfg dec_cfg = nullptr;
    if (mpp_dec_cfg_init(&dec_cfg) == MPP_OK) {
        if (p->dec_api->control(p->dec_ctx, MPP_DEC_GET_CFG, dec_cfg) == MPP_OK) {
            mpp_dec_cfg_set_u32(dec_cfg, "base:split_parse", 1);
            (void)p->dec_api->control(p->dec_ctx, MPP_DEC_SET_CFG, dec_cfg);
        }
        mpp_dec_cfg_deinit(dec_cfg);
    }

    MppFrameFormat out_fmt = MPP_FMT_YUV420SP;
    (void)p->dec_api->control(p->dec_ctx, MPP_DEC_SET_OUTPUT_FORMAT, &out_fmt);

    if (alloc_mpp_buffer_group(&p->pkt_grp) != MPP_OK) {
        return abort_init("JPEG packet buffer group", -1);
    }
    if (alloc_mpp_buffer_group(&p->frm_grp) != MPP_OK) {
        return abort_init("JPEG frame buffer group", -1);
    }

    size_t buf_size = static_cast<size_t>(stride_w) * stride_h * 22 / 10;
    buf_size = std::max<size_t>(buf_size, 1024 * 1024);
    buf_size = std::min<size_t>(buf_size, 20 * 1024 * 1024);

    ret = mpp_buffer_get(p->frm_grp, &p->dec_buf, buf_size);
    if (ret) return abort_init("JPEG frame buffer", ret);
    ret = mpp_frame_init(&p->dec_frame);
    if (ret) return abort_init("JPEG mpp_frame_init", ret);
    mpp_frame_set_buffer(p->dec_frame, p->dec_buf);

    ret = mpp_create(&p->enc_ctx, &p->enc_api);
    if (ret) return abort_init("mpp_create H264 encoder", ret);
    RK_S64 enc_timeout = 2000;
    (void)p->enc_api->control(p->enc_ctx, MPP_SET_INPUT_TIMEOUT, &enc_timeout);
    (void)p->enc_api->control(p->enc_ctx, MPP_SET_OUTPUT_TIMEOUT, &enc_timeout);

    ret = mpp_init(p->enc_ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
    if (ret) return abort_init("mpp_init H264 encoder", ret);
    ret = mpp_enc_cfg_init(&p->enc_cfg);
    if (ret) return abort_init("mpp_enc_cfg_init", ret);
    ret = p->enc_api->control(p->enc_ctx, MPP_ENC_GET_CFG, p->enc_cfg);
    if (ret) return abort_init("MPP_ENC_GET_CFG", ret);

    if (!configure_h264_encoder(
            p->enc_ctx, p->enc_api, p->enc_cfg,
            width, height, stride_w, stride_h, MPP_FMT_YUV420SP, fps, kbps, gop)) {
        delete p;
        return nullptr;
    }

    p->enc_hor_stride = stride_w;
    p->enc_ver_stride = stride_h;
    return p;
}

extern "C" int rkmpp_native_mjpeg_h264_encode(
    RkmppNativeMjpegH264 *p,
    const uint8_t *jpeg_data,
    size_t jpeg_len,
    int64_t pts_us,
    int force_idr,
    const uint8_t **out_data,
    size_t *out_size,
    int *out_keyframe) {
    if (!p || !p->dec_ctx || !p->enc_ctx || !jpeg_data || jpeg_len < 2 ||
        !out_data || !out_size || !out_keyframe) {
        return fail("invalid native MJPEG->H264 encode call", -1);
    }
    *out_data = nullptr;
    *out_size = 0;
    *out_keyframe = 0;

    if (jpeg_data[0] != 0xFF || jpeg_data[1] != 0xD8) {
        return fail("invalid JPEG SOI header", -1);
    }

    if (p->enc_packet) mpp_packet_deinit(&p->enc_packet);

    MppBuffer in_buf = nullptr;
    int ret = mpp_buffer_get(p->pkt_grp, &in_buf, jpeg_len);
    if (ret || !in_buf) return fail("JPEG packet buffer get", ret);

    void *in_ptr = mpp_buffer_get_ptr(in_buf);
    if (!in_ptr) {
        mpp_buffer_put(in_buf);
        return fail("JPEG packet buffer ptr", -1);
    }
    std::memcpy(in_ptr, jpeg_data, jpeg_len);

    MppPacket dec_pkt = nullptr;
    ret = mpp_packet_init_with_buffer(&dec_pkt, in_buf);
    mpp_buffer_put(in_buf);
    if (ret || !dec_pkt) return fail("mpp_packet_init_with_buffer", ret);

    mpp_packet_set_data(dec_pkt, in_ptr);
    mpp_packet_set_pos(dec_pkt, in_ptr);
    mpp_packet_set_size(dec_pkt, jpeg_len);
    mpp_packet_set_length(dec_pkt, jpeg_len);

    MppMeta pkt_meta = mpp_packet_get_meta(dec_pkt);
    if (pkt_meta) {
        mpp_meta_set_frame(pkt_meta, KEY_OUTPUT_FRAME, p->dec_frame);
    }

    ret = p->dec_api->decode_put_packet(p->dec_ctx, dec_pkt);
    mpp_packet_deinit(&dec_pkt);
    if (ret != MPP_OK) return fail("JPEG decode_put_packet", ret);

    MppFrame decoded = nullptr;
    ret = p->dec_api->decode_get_frame(p->dec_ctx, &decoded);
    if (ret == MPP_ERR_TIMEOUT || !decoded) {
        return 0;
    }
    if (ret != MPP_OK || mpp_frame_get_errinfo(decoded) ||
        mpp_frame_get_discard(decoded) || mpp_frame_get_info_change(decoded)) {
        if (decoded && decoded != p->dec_frame) mpp_frame_deinit(&decoded);
        return fail("JPEG decode_get_frame", ret);
    }

    const int fw = static_cast<int>(mpp_frame_get_width(decoded));
    const int fh = static_cast<int>(mpp_frame_get_height(decoded));
    const int hs = static_cast<int>(mpp_frame_get_hor_stride(decoded));
    const int vs = static_cast<int>(mpp_frame_get_ver_stride(decoded));
    const MppFrameFormat fmt = mpp_frame_get_fmt(decoded);
    MppBuffer out_buf = mpp_frame_get_buffer(decoded);

    if (!out_buf || fw != p->width || fh != p->height || hs < fw || vs < fh) {
        if (decoded != p->dec_frame) mpp_frame_deinit(&decoded);
        return fail("unexpected JPEG decoded geometry", -1);
    }

    if (!p->layout_configured || hs != p->enc_hor_stride || vs != p->enc_ver_stride || fmt != p->enc_fmt) {
        if (!set_cfg_s32(p->enc_cfg, "prep:hor_stride", hs) ||
            !set_cfg_s32(p->enc_cfg, "prep:ver_stride", vs) ||
            !set_cfg_s32(p->enc_cfg, "prep:format", fmt)) {
            if (decoded != p->dec_frame) mpp_frame_deinit(&decoded);
            return -1;
        }
        ret = p->enc_api->control(p->enc_ctx, MPP_ENC_SET_CFG, p->enc_cfg);
        if (ret) {
            if (decoded != p->dec_frame) mpp_frame_deinit(&decoded);
            return fail("update encoder layout", ret);
        }
        p->enc_hor_stride = hs;
        p->enc_ver_stride = vs;
        p->enc_fmt = fmt;
        p->layout_configured = true;
    }

    if (force_idr) {
        ret = p->enc_api->control(p->enc_ctx, MPP_ENC_SET_IDR_FRAME, nullptr);
        if (ret) {
            if (decoded != p->dec_frame) mpp_frame_deinit(&decoded);
            return fail("MPP_ENC_SET_IDR_FRAME", ret);
        }
    }

    MppFrame enc_frame = nullptr;
    ret = mpp_frame_init(&enc_frame);
    if (ret || !enc_frame) {
        if (decoded != p->dec_frame) mpp_frame_deinit(&decoded);
        return fail("mpp_frame_init", ret);
    }

    mpp_frame_set_width(enc_frame, fw);
    mpp_frame_set_height(enc_frame, fh);
    mpp_frame_set_hor_stride(enc_frame, hs);
    mpp_frame_set_ver_stride(enc_frame, vs);
    mpp_frame_set_fmt(enc_frame, fmt);
    mpp_frame_set_buffer(enc_frame, out_buf);
    mpp_frame_set_eos(enc_frame, 0);
    mpp_frame_set_pts(enc_frame, pts_us);

    ret = p->enc_api->encode_put_frame(p->enc_ctx, enc_frame);
    mpp_frame_deinit(&enc_frame);
    if (decoded != p->dec_frame) mpp_frame_deinit(&decoded);
    if (ret != MPP_OK) return fail("encode_put_frame", ret);

    ret = p->enc_api->encode_get_packet(p->enc_ctx, &p->enc_packet);
    if (ret == MPP_ERR_TIMEOUT || !p->enc_packet) {
        return 0;
    }
    if (ret != MPP_OK) return fail("encode_get_packet", ret);

    const auto *pkt_data = static_cast<const uint8_t *>(mpp_packet_get_pos(p->enc_packet));
    const size_t pkt_len = mpp_packet_get_length(p->enc_packet);
    if (pkt_data && pkt_len > 0) {
        *out_data = pkt_data;
        *out_size = pkt_len;
        *out_keyframe = is_h264_idr(pkt_data, pkt_len);
    }
    return 0;
}

extern "C" int rkmpp_native_mjpeg_h264_reconfigure(RkmppNativeMjpegH264 *p, int kbps, int gop) {
    if (!p || !p->enc_ctx || !p->enc_cfg) {
        return fail("native MJPEG->H264 not initialized", -1);
    }
    return reconfigure_h264_bitrate(p->enc_ctx, p->enc_api, p->enc_cfg, kbps, gop) ? 0 : -1;
}

extern "C" void rkmpp_native_mjpeg_h264_free(RkmppNativeMjpegH264 *p) {
    delete p;
}

// Raw RAM input (NV12=0, YUYV422=1, YUV420P=2) -> H.264 encoder
struct RkmppNativeEncoder {
    MppCtx ctx = nullptr;
    MppApi *api = nullptr;
    MppEncCfg cfg = nullptr;
    MppBufferGroup grp = nullptr;
    MppBuffer frm_buf = nullptr;
    MppFrame frame = nullptr;
    MppPacket packet = nullptr;

    int width = 0;
    int height = 0;
    int stride_w = 0;
    int stride_h = 0;
    int format = 0; // 0: NV12, 1: YUYV422, 2: YUV420P

    void close() {
        if (packet) mpp_packet_deinit(&packet);
        if (frame) mpp_frame_deinit(&frame);
        if (frm_buf) {
            mpp_buffer_put(frm_buf);
            frm_buf = nullptr;
        }
        if (grp) {
            mpp_buffer_group_put(grp);
            grp = nullptr;
        }
        if (cfg) {
            mpp_enc_cfg_deinit(cfg);
            cfg = nullptr;
        }
        if (ctx) {
            if (api) api->reset(ctx);
            mpp_destroy(ctx);
            ctx = nullptr;
        }
    }

    ~RkmppNativeEncoder() { close(); }
};

extern "C" RkmppNativeEncoder *rkmpp_native_enc_new(
    int width, int height, int format, int fps, int kbps, int gop) {
    if (width <= 0 || height <= 0 || (width & 1) || (height & 1) ||
        format < 0 || format > 2 || fps <= 0 || fps > 240 || kbps <= 0 || gop <= 0) {
        fail("invalid native encoder parameters", -1);
        return nullptr;
    }

    auto *e = new (std::nothrow) RkmppNativeEncoder;
    if (!e) {
        fail("allocate native encoder", -1);
        return nullptr;
    }
    e->width = width;
    e->height = height;
    e->format = format;
    e->stride_w = (width + 15) & ~15;
    e->stride_h = (height + 15) & ~15;

    auto abort_init = [e](const char *op, int ret) -> RkmppNativeEncoder * {
        fail(op, ret);
        delete e;
        return nullptr;
    };

    int ret = mpp_create(&e->ctx, &e->api);
    if (ret) return abort_init("mpp_create", ret);
    RK_S64 timeout = 2000;
    (void)e->api->control(e->ctx, MPP_SET_INPUT_TIMEOUT, &timeout);
    (void)e->api->control(e->ctx, MPP_SET_OUTPUT_TIMEOUT, &timeout);

    ret = mpp_init(e->ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
    if (ret) return abort_init("mpp_init", ret);
    ret = mpp_enc_cfg_init(&e->cfg);
    if (ret) return abort_init("mpp_enc_cfg_init", ret);
    ret = e->api->control(e->ctx, MPP_ENC_GET_CFG, e->cfg);
    if (ret) return abort_init("MPP_ENC_GET_CFG", ret);

    const MppFrameFormat mpp_fmt =
        format == 1 ? MPP_FMT_YUV422_YUYV :
        format == 2 ? MPP_FMT_YUV420P : MPP_FMT_YUV420SP;
    const int hor_stride = format == 1 ? e->stride_w * 2 : e->stride_w;

    if (!configure_h264_encoder(
            e->ctx, e->api, e->cfg,
            width, height, hor_stride, e->stride_h, mpp_fmt, fps, kbps, gop)) {
        delete e;
        return nullptr;
    }

    if (alloc_mpp_buffer_group(&e->grp) != MPP_OK) {
        return abort_init("native encoder buffer group", -1);
    }

    const size_t frame_size = format == 1
        ? static_cast<size_t>(hor_stride) * e->stride_h
        : static_cast<size_t>(e->stride_w) * e->stride_h * 3 / 2;

    ret = mpp_buffer_get(e->grp, &e->frm_buf, frame_size);
    if (ret) return abort_init("native encoder frame buffer", ret);
    ret = mpp_frame_init(&e->frame);
    if (ret) return abort_init("mpp_frame_init", ret);

    mpp_frame_set_width(e->frame, e->width);
    mpp_frame_set_height(e->frame, e->height);
    mpp_frame_set_hor_stride(e->frame, hor_stride);
    mpp_frame_set_ver_stride(e->frame, e->stride_h);
    mpp_frame_set_fmt(e->frame, mpp_fmt);
    mpp_frame_set_buffer(e->frame, e->frm_buf);

    return e;
}

extern "C" int rkmpp_native_enc_encode(
    RkmppNativeEncoder *e,
    const uint8_t *raw_data,
    size_t raw_len,
    int64_t pts_us,
    int force_idr,
    const uint8_t **out_data,
    size_t *out_size,
    int *out_keyframe) {
    if (!e || !e->ctx || !e->frm_buf || !raw_data || !out_data || !out_size || !out_keyframe) {
        return fail("invalid native encode call", -1);
    }
    *out_data = nullptr;
    *out_size = 0;
    *out_keyframe = 0;

    if (e->packet) mpp_packet_deinit(&e->packet);

    auto *dst = static_cast<uint8_t *>(mpp_buffer_get_ptr(e->frm_buf));
    if (!dst) return fail("native encoder buffer ptr", -1);

    const int w = e->width;
    const int h = e->height;
    const int sw = e->stride_w;
    const int sh = e->stride_h;

    if (e->format == 1) {
        const size_t expected = static_cast<size_t>(w) * 2 * h;
        if (raw_len < expected) return fail("short YUYV frame", -1);
        const int dst_stride = sw * 2;
        const int src_stride = w * 2;
        if (src_stride == dst_stride) {
            std::memcpy(dst, raw_data, expected);
        } else {
            for (int y = 0; y < h; ++y) {
                std::memcpy(dst + y * dst_stride, raw_data + y * src_stride, src_stride);
            }
        }
    } else if (e->format == 2) {
        const size_t y_size = static_cast<size_t>(w) * h;
        const size_t uv_size = static_cast<size_t>(w / 2) * (h / 2);
        if (raw_len < y_size + uv_size * 2) return fail("short YUV420P frame", -1);
        if (w == sw && h == sh) {
            std::memcpy(dst, raw_data, y_size + uv_size * 2);
        } else {
            for (int y = 0; y < h; ++y) {
                std::memcpy(dst + y * sw, raw_data + y * w, w);
            }
            uint8_t *dst_u = dst + static_cast<size_t>(sw) * sh;
            const uint8_t *src_u = raw_data + y_size;
            for (int y = 0; y < h / 2; ++y) {
                std::memcpy(dst_u + y * (sw / 2), src_u + y * (w / 2), w / 2);
            }
            uint8_t *dst_v = dst_u + static_cast<size_t>(sw / 2) * (sh / 2);
            const uint8_t *src_v = src_u + uv_size;
            for (int y = 0; y < h / 2; ++y) {
                std::memcpy(dst_v + y * (sw / 2), src_v + y * (w / 2), w / 2);
            }
        }
    } else {
        const size_t y_size = static_cast<size_t>(w) * h;
        if (raw_len < y_size * 3 / 2) return fail("short NV12 frame", -1);
        if (w == sw && h == sh) {
            std::memcpy(dst, raw_data, y_size * 3 / 2);
        } else {
            for (int y = 0; y < h; ++y) {
                std::memcpy(dst + y * sw, raw_data + y * w, w);
            }
            uint8_t *dst_uv = dst + static_cast<size_t>(sw) * sh;
            const uint8_t *src_uv = raw_data + y_size;
            for (int y = 0; y < h / 2; ++y) {
                std::memcpy(dst_uv + y * sw, src_uv + y * w, w);
            }
        }
    }

    if (force_idr) {
        int ret = e->api->control(e->ctx, MPP_ENC_SET_IDR_FRAME, nullptr);
        if (ret) return fail("MPP_ENC_SET_IDR_FRAME", ret);
    }

    mpp_frame_set_eos(e->frame, 0);
    mpp_frame_set_pts(e->frame, pts_us);

    int ret = e->api->encode_put_frame(e->ctx, e->frame);
    if (ret != MPP_OK) return fail("encode_put_frame", ret);

    ret = e->api->encode_get_packet(e->ctx, &e->packet);
    if (ret == MPP_ERR_TIMEOUT || !e->packet) {
        return 0;
    }
    if (ret != MPP_OK) return fail("encode_get_packet", ret);

    const auto *pkt_data = static_cast<const uint8_t *>(mpp_packet_get_pos(e->packet));
    const size_t pkt_len = mpp_packet_get_length(e->packet);
    if (pkt_data && pkt_len > 0) {
        *out_data = pkt_data;
        *out_size = pkt_len;
        *out_keyframe = is_h264_idr(pkt_data, pkt_len);
    }
    return 0;
}

extern "C" int rkmpp_native_enc_reconfigure(RkmppNativeEncoder *e, int kbps, int gop) {
    if (!e || !e->ctx || !e->cfg) {
        return fail("native encoder not initialized", -1);
    }
    return reconfigure_h264_bitrate(e->ctx, e->api, e->cfg, kbps, gop) ? 0 : -1;
}

extern "C" void rkmpp_native_enc_free(RkmppNativeEncoder *e) {
    delete e;
}
#else
struct RkmppNativeMjpegH264;
struct RkmppNativeEncoder;

extern "C" RkmppNativeMjpegH264 *rkmpp_native_mjpeg_h264_new(int, int, int, int, int) {
    fail("RKMPP native support not built", -1);
    return nullptr;
}
extern "C" int rkmpp_native_mjpeg_h264_encode(
    RkmppNativeMjpegH264 *, const uint8_t *, size_t, int64_t, int,
    const uint8_t **, size_t *, int *) {
    return -1;
}
extern "C" int rkmpp_native_mjpeg_h264_reconfigure(RkmppNativeMjpegH264 *, int, int) { return -1; }
extern "C" void rkmpp_native_mjpeg_h264_free(RkmppNativeMjpegH264 *) {}

extern "C" RkmppNativeEncoder *rkmpp_native_enc_new(int, int, int, int, int, int) {
    fail("RKMPP native support not built", -1);
    return nullptr;
}
extern "C" int rkmpp_native_enc_encode(
    RkmppNativeEncoder *, const uint8_t *, size_t, int64_t, int,
    const uint8_t **, size_t *, int *) {
    return -1;
}
extern "C" int rkmpp_native_enc_reconfigure(RkmppNativeEncoder *, int, int) { return -1; }
extern "C" void rkmpp_native_enc_free(RkmppNativeEncoder *) {}
#endif

extern "C" const char *rkmpp_native_error(void) {
    return native_error;
}
