#ifndef CEDARC_COMMON_H
#define CEDARC_COMMON_H

#include "cedarc_abi.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <string>

namespace cedarc_common {

inline int align16(int v) { return (v + 15) & ~15; }

inline void *dlopen_lib(const char *soname, int flags) {
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

inline bool packet_has_param_sets(const uint8_t *data, size_t size,
                                  cedarc_abi::VENC_CODEC_TYPE codec) {
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
  if (codec == cedarc_abi::VENC_CODEC_H264) {
    return (data[offset] & 0x1f) == 7; // SPS
  }
  if (codec == cedarc_abi::VENC_CODEC_H265) {
    return ((data[offset] >> 1) & 0x3f) == 32; // VPS
  }
  return false;
}

inline unsigned int vbv_size_for_resolution(int width, int height) {
  return (width >= 2560 || height >= 1440) ? (8u * 1024u * 1024u)
                                           : (4u * 1024u * 1024u);
}

inline cedarc_abi::VencH264Param build_h264_cbr_param(int width, int height,
                                                       int fps, int kbs,
                                                       int gop) {
  using namespace cedarc_abi;
  VencH264Param h264;
  std::memset(&h264, 0, sizeof(h264));
  // Allwinner VE2 v2 (e.g. A733) H.264 hardware unconditionally emits
  // High-profile 8x8 transform macroblock syntax; Baseline/Main PPS omits
  // transform_8x8_mode_flag and produces undecodable/green streams.
  // Use AW_CBR (matching Allwinner vencoderdemo) with tighter QP bounds so
  // static KVM screens do not drop to QP 45 and blur.
  h264.bEntropyCodingCABAC = 1;
  h264.nBitrate = std::max(kbs, 64) * 1000;
  h264.nFramerate = fps;
  h264.nSrcFramerate = fps;
  h264.nCodingMode = VENC_FRAME_CODING;
  h264.nMaxKeyInterval = gop;
  h264.sProfileLevel.nProfile = VENC_H264ProfileHigh;
  h264.sProfileLevel.nLevel = (width >= 2560 || height >= 1440)
                                  ? VENC_H264Level51
                                  : (fps > 30 || width >= 1920)
                                        ? VENC_H264Level42
                                        : VENC_H264Level41;
  h264.sQPRange.nMinqp = 10;
  h264.sQPRange.nMaxqp = 38;
  h264.sQPRange.nMinPqp = 10;
  h264.sQPRange.nMaxPqp = 38;
  h264.sQPRange.nQpInit = 24;
  h264.sGopParam.bUseGopCtrlEn = 1;
  h264.sGopParam.eGopMode = AW_NORMALP;
  h264.sRcParam.eRcMode = AW_CBR;
  return h264;
}

inline cedarc_abi::VencH265Param build_h265_cbr_param(int width, int height,
                                                       int fps, int kbs,
                                                       int gop) {
  using namespace cedarc_abi;
  VencH265Param h265;
  std::memset(&h265, 0, sizeof(h265));
  h265.nBitrate = std::max(kbs, 64) * 1000;
  h265.nFramerate = fps;
  h265.nSrcFramerate = fps;
  h265.nGopSize = gop;
  h265.nIntraPeriod = gop;
  h265.idr_period = gop;
  h265.nQPInit = 24;
  h265.sProfileLevel.nProfile = VENC_H265ProfileMain;
  h265.sProfileLevel.nLevel = (width >= 2560 || height >= 1440)
                                  ? VENC_H265Level51
                                  : VENC_H265Level41;
  h265.sQPRange.nMinqp = 10;
  h265.sQPRange.nMaxqp = 38;
  h265.sQPRange.nMinPqp = 10;
  h265.sQPRange.nMaxPqp = 38;
  h265.sQPRange.nQpInit = 24;
  h265.sGopParam.bUseGopCtrlEn = 1;
  h265.sGopParam.eGopMode = AW_NORMALP;
  h265.sRcParam.eRcMode = AW_CBR;
  return h265;
}

} // namespace cedarc_common

#endif // CEDARC_COMMON_H
