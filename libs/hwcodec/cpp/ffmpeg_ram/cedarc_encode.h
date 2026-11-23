#ifndef CEDARC_ENCODE_H
#define CEDARC_ENCODE_H

#include <stdint.h>

typedef void (*CedarcEncodeCallback)(const uint8_t *data, int len, int64_t pts,
                                     int key, const void *obj);
typedef void (*CedarcEncodePacketCallback)(void *packet, const uint8_t *data,
                                           int len, int64_t pts, int key,
                                           const void *obj);

#ifdef __cplusplus
extern "C" {
#endif

int cedarc_is_supported(void);

void *cedarc_new_encoder(const char *name, int width, int height, int pixfmt,
                         int align, int fps, int gop, int rc, int kbs,
                         int *linesize, int *offset, int *length,
                         CedarcEncodeCallback callback);

int cedarc_encode(void *encoder, const uint8_t *data, int length,
                  const void *obj, int64_t ms);

int cedarc_encode_packet(void *encoder, const uint8_t *data, int length,
                         const void *obj, int64_t ms,
                         CedarcEncodePacketCallback callback);

int cedarc_set_bitrate(void *encoder, int kbs);

void cedarc_request_keyframe(void *encoder);

void cedarc_free_encoder(void *encoder);

const char *cedarc_encoder_last_error(void);

#ifdef __cplusplus
}
#endif

#endif // CEDARC_ENCODE_H
