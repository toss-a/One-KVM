#ifndef CEDARC_MJPEG_H26X_H
#define CEDARC_MJPEG_H26X_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Codec values for cedarc_mjpeg_h26x_new:
//   0 = H.264
//   1 = H.265 / HEVC
int cedarc_mjpeg_h26x_supported(void);

void *cedarc_mjpeg_h26x_new(int codec, int width, int height, int fps,
                            int kbps, int gop);

// Decode MJPEG on VE1 -> zero-copy DMA-buf/IOMMU -> encode H.264/H.265 on VE2.
// Returns 0 on success (*out_size == 0 means frame skipped without fatal error),
// or -1 on fatal pipeline error (caller should fall back to CPU MJPEG decode).
int cedarc_mjpeg_h26x_encode(void *pipeline, const uint8_t *jpeg_data,
                             size_t jpeg_len, int64_t pts_us, int force_idr,
                             const uint8_t **out_data, size_t *out_size,
                             int *out_keyframe);

int cedarc_mjpeg_h26x_reconfigure(void *pipeline, int kbps, int gop);

void cedarc_mjpeg_h26x_free(void *pipeline);

const char *cedarc_mjpeg_h26x_last_error(void);

#ifdef __cplusplus
}
#endif

#endif // CEDARC_MJPEG_H26X_H
