#ifndef CEDARC_ABI_H
#define CEDARC_ABI_H

// Self-contained ABI definitions for Allwinner CedarC v2 (libvencoder.so),
// verified against libcedarc-dev-2.0.0-arm64 (Allwinner A733 / sun60iw2p1).
// Loaded dynamically via dlopen("libvencoder.so") so builds do not require
// Allwinner vendor headers or libraries at compile time.

#include <stddef.h>
#include <stdint.h>

namespace cedarc_abi {

static constexpr unsigned int VENC_BUFFERFLAG_KEYFRAME = 0x00000001;
static constexpr int CLASSIFY_TH_NUM = 16;
static constexpr int MAX_OUTPUT_PACK_NUM = 8;

enum VENC_CODEC_TYPE {
  VENC_CODEC_H264 = 0,
  VENC_CODEC_JPEG = 1,
  VENC_CODEC_H264_VER2 = 2,
  VENC_CODEC_H265 = 3,
  VENC_CODEC_VP8 = 4,
};

enum VENC_PIXEL_FMT {
  VENC_PIXEL_YUV420SP = 0, // NV12
  VENC_PIXEL_YVU420SP = 1, // NV21
  VENC_PIXEL_YUV420P = 2,  // I420
  VENC_PIXEL_YVU420P = 3,  // YV12
  VENC_PIXEL_YUV422_BEGIN = 4,
  VENC_PIXEL_YUV422SP = 5,
  VENC_PIXEL_YVU422SP = 6,
  VENC_PIXEL_YUV422P = 7,
  VENC_PIXEL_YVU422P = 8,
  VENC_PIXEL_YUYV422 = 9,
  VENC_PIXEL_UYVY422 = 10,
  VENC_PIXEL_YVYU422 = 11,
  VENC_PIXEL_VYUY422 = 12,
};

enum VENC_CODING_MODE {
  VENC_FRAME_CODING = 0,
  VENC_FIELD_CODING = 1,
};

enum VENC_RC_MODE {
  AW_CBR = 0,
  AW_VBR = 1,
  AW_AVBR = 2,
  AW_QPMAP = 3,
  AW_FIXQP = 4,
};

enum VENC_VIDEO_GOP_MODE {
  AW_NORMALP = 1,
  AW_ADVANCE_SINGLE = 2,
  AW_DOUBLEP = 3,
  AW_SPECIAL_DOUBLE = 4,
  AW_SPECIAL_SMARTP = 5,
  AW_SMARTP = 6,
};

enum VENC_H264PROFILETYPE {
  VENC_H264ProfileBaseline = 66,
  VENC_H264ProfileMain = 77,
  VENC_H264ProfileHigh = 100,
};

enum VENC_H264LEVELTYPE {
  VENC_H264Level31 = 31,
  VENC_H264Level4 = 40,
  VENC_H264Level41 = 41,
  VENC_H264Level42 = 42,
  VENC_H264Level5 = 50,
  VENC_H264Level51 = 51,
  VENC_H264Level52 = 52,
  VENC_H264LevelDefault = 0,
};

enum VENC_H265PROFILETYPE {
  VENC_H265ProfileMain = 1,
  VENC_H265ProfileMain10 = 2,
  VENC_H265ProfileMainStill = 3,
};

enum VENC_H265LEVELTYPE {
  VENC_H265Level31 = 93,
  VENC_H265Level4 = 120,
  VENC_H265Level41 = 123,
  VENC_H265Level5 = 150,
  VENC_H265Level51 = 153,
  VENC_H265Level52 = 156,
  VENC_H265LevelDefault = 0,
};

enum VENC_INDEXTYPE {
  VENC_IndexParamBitrate = 0x0,
  VENC_IndexParamFramerate = 0x1,
  VENC_IndexParamMaxKeyInterval = 0x2,
  VENC_IndexParamForceKeyFrame = 0x6,
  VENC_IndexParamSetVbvSize = 0x0f,
  VENC_IndexParamH264Param = 0x100,
  VENC_IndexParamH264SPSPPS = 0x101,
  VENC_IndexParamH265Param = 0x300,
  VENC_IndexParamH265Header = 0x304,
  VENC_IndexParamH264ConstraintFlag = 0x50b,
};

enum VENC_RESULT_TYPE {
  VENC_RESULT_ERROR = -1,
  VENC_RESULT_OK = 0,
  VENC_RESULT_NO_FRAME_BUFFER = 1,
  VENC_RESULT_BITSTREAM_IS_FULL = 2,
  VENC_RESULT_ILLEGAL_PARAM = 3,
  VENC_RESULT_NOT_SUPPORT = 4,
  VENC_RESULT_BITSTREAM_IS_EMPTY = 5,
  VENC_RESULT_NO_MEMORY = 6,
  VENC_RESULT_NO_RESOURCE = 7,
  VENC_RESULT_NULL_PTR = 8,
  VENC_RESULT_DROP_FRAME = 9,
};

struct VencRect {
  int nLeft;
  int nTop;
  int nWidth;
  int nHeight;
};

struct VencQPRange {
  int nMaxqp;
  int nMinqp;
  int nMaxPqp;
  int nMinPqp;
  int nQpInit;
  int bEnMbQpLimit;
};

struct VencVbrParam {
  unsigned int uMaxBitRate;
  unsigned int nMovingTh;
  int nQuality;
  int nIFrmBitsCoef;
  int nPFrmBitsCoef;
};

struct VencFixQP {
  int bEnable;
  int nIQp;
  int nPQp;
};

struct sQpMapPara {
  unsigned int sad_th;
  unsigned int sad_num;
  int hor_expand;
  int ver_expand;
  unsigned int mse_th[4];
  int mse_qp[5];
  int max_mse_qp;
  int min_mse_qp;
  float force_qp_ratio_th0;
  float force_qp_ratio_th1;
};

struct sCtuQpInfo {
  unsigned int is_move : 1;
  unsigned int is_expand : 1;
  unsigned int en_mad : 1;
  unsigned int r0 : 5;
  unsigned int mad_val : 8;
  unsigned int mse_val : 16;
};

struct VencMBModeCtrl {
  unsigned char mode_ctrl_en;
  unsigned char *p_info;
  unsigned int last_slice_type;
  unsigned int est_bit_rate;
  unsigned int pre_est_bits;
  unsigned int cur_est_bits;
  unsigned int frm_mad;
  unsigned int frm_mse;
  int mad_mb_num;
  int mse_mb_num;
  int force_qp_num;
  int en_chn_mse_qp;
  float mad_qp_ratio;
  float mse_qp_ratio;
  float force_qp_ratio;
  sQpMapPara qp_map_para;
  sCtuQpInfo *p_ctu_info;
};

struct VencRcParam {
  VENC_RC_MODE eRcMode;
  unsigned char bLowBitrateBeginFlag;
  unsigned char bUseSetMadThrdFlag;
  unsigned char uMadThrdI[CLASSIFY_TH_NUM];
  unsigned char uMadThrdP[CLASSIFY_TH_NUM];
  unsigned char uMadThrdB[CLASSIFY_TH_NUM];
  unsigned char uMadQpI[CLASSIFY_TH_NUM];
  unsigned char uMadQpP[CLASSIFY_TH_NUM];
  unsigned int uStatTime;
  unsigned int uMinIQp;
  int nMaxReEncodeTimes;
  VencVbrParam sVbrParam;
  VencFixQP sFixQp;
  VencMBModeCtrl sQpMap;
  unsigned int uRowQpDelta;
  unsigned int uDirectionThrd;
  unsigned int uQpDeltaLevelI;
  unsigned int uQpDeltaLevelP;
  unsigned int uQpDeltaLevelB;
  unsigned int uInputFrmRate;
  unsigned int uOutputFrmRate;
  unsigned int uFluctuateLevel;
  unsigned int uMinIprop;
  unsigned int uMaxIprop;
};

struct VencAdvancedRefParam {
  unsigned char bAdvancedRefEn;
  unsigned int nBase;
  unsigned int nEnhance;
  unsigned char bRefBaseEn;
};

struct VencGopParam {
  unsigned char bUseGopCtrlEn;
  VENC_VIDEO_GOP_MODE eGopMode;
  unsigned int nVirtualIFrameInterval;
  unsigned int nSpInterval;
  VencAdvancedRefParam sRefParam;
};

struct VencH264ProfileLevel {
  VENC_H264PROFILETYPE nProfile;
  VENC_H264LEVELTYPE nLevel;
};

struct VencH264Param {
  VencH264ProfileLevel sProfileLevel;
  int bEntropyCodingCABAC;
  VencQPRange sQPRange;
  int nFramerate;
  int nSrcFramerate;
  int nBitrate;
  int nMaxKeyInterval;
  VENC_CODING_MODE nCodingMode;
  VencGopParam sGopParam;
  VencRcParam sRcParam;
};

struct VencH265ProfileLevel {
  VENC_H265PROFILETYPE nProfile;
  VENC_H265LEVELTYPE nLevel;
};

struct VencH265Param {
  int idr_period;
  VencH265ProfileLevel sProfileLevel;
  VencQPRange sQPRange;
  int nFramerate;
  int nSrcFramerate;
  int nBitrate;
  int nIntraPeriod;
  int nGopSize;
  int nQPInit;
  VencRcParam sRcParam;
  VencGopParam sGopParam;
};

struct VencH264ConstraintFlag {
  unsigned char reserve_zero : 2;
  unsigned char constraint_5 : 1;
  unsigned char constraint_4 : 1;
  unsigned char constraint_3 : 1;
  unsigned char constraint_2 : 1;
  unsigned char constraint_1 : 1;
  unsigned char constraint_0 : 1;
};

struct VencBaseConfig {
  unsigned char bEncH264Nalu;
  unsigned int nInputWidth;
  unsigned int nInputHeight;
  unsigned int nDstWidth;
  unsigned int nDstHeight;
  unsigned int nStride;
  VENC_PIXEL_FMT eInputFormat;
  void *memops;
  void *veOpsS;
  void *pVeOpsSelf;
  unsigned char bOnlyWbFlag;
  unsigned char bLbcLossyComEnFlag2x;
  unsigned char bLbcLossyComEnFlag2_5x;
  unsigned char bIsVbvNoCache;
  unsigned int bVcuAutoMode;
  unsigned int nFrameNumInGroup;
  unsigned int extend_flag;
  unsigned int en_vcu;
};

struct VencHeaderData {
  unsigned char *pBuffer;
  unsigned int nLength;
};

struct VencROIConfig {
  int bEnable;
  int index;
  int nQPoffset;
  unsigned char roi_abs_flag;
  VencRect sRect;
};

struct VencInputBuffer {
  unsigned long nID;
  long long nPts;
  unsigned int nFlag;
  unsigned char *pAddrPhyY;
  unsigned char *pAddrPhyC;
  unsigned char *pAddrVirY;
  unsigned char *pAddrVirC;
  int nWidth;
  int nHeight;
  int nAlign;
  int bEnableCorp;
  VencRect sCropInfo;
  int ispPicVar;
  int ispPicVarChroma;
  int bUseInputBufferRoi;
  VencROIConfig roi_param[8];
  int bAllocMemSelf;
  int nShareBufFd;
  unsigned char bUseCsiColorFormat;
  VENC_PIXEL_FMT eCsiColorFormat;
  int envLV;
};

struct FrameInfo {
  int CurrQp;
  int avQp;
  int nGopIndex;
  int nFrameIndex;
  int nTotalIndex;
  unsigned int sliceType;
};

struct VencPackInfo {
  int uType;
  unsigned int nOffset;
  unsigned int nLength;
};

struct VencOutputBuffer {
  int nID;
  long long nPts;
  unsigned int nFlag;
  unsigned int nSize0;
  unsigned int nSize1;
  unsigned char *pData0;
  unsigned char *pData1;
  FrameInfo frame_info;
  unsigned int nSize2;
  unsigned char *pData2;
  unsigned int nExSize0;
  unsigned int nExSize1;
  unsigned char *pExData0;
  unsigned char *pExData1;
  unsigned int nPackNum;
  VencPackInfo mPackInfo[MAX_OUTPUT_PACK_NUM];
};

struct VencAllocateBufferParam {
  unsigned int nBufferNum;
  unsigned int nSizeY;
  unsigned int nSizeC;
};

using VideoEncoder = void *;

#if defined(__aarch64__)
static_assert(sizeof(VencBaseConfig) == 80, "VencBaseConfig ABI mismatch");
static_assert(offsetof(VencBaseConfig, nStride) == 20, "VencBaseConfig.nStride mismatch");
static_assert(offsetof(VencBaseConfig, eInputFormat) == 24, "VencBaseConfig.eInputFormat mismatch");
static_assert(offsetof(VencBaseConfig, memops) == 32, "VencBaseConfig.memops mismatch");
static_assert(offsetof(VencBaseConfig, bVcuAutoMode) == 60, "VencBaseConfig.bVcuAutoMode mismatch");
static_assert(sizeof(VencRcParam) == 320, "VencRcParam ABI mismatch");
static_assert(offsetof(VencRcParam, sVbrParam) == 100, "VencRcParam.sVbrParam mismatch");
static_assert(offsetof(VencRcParam, sFixQp) == 120, "VencRcParam.sFixQp mismatch");
static_assert(offsetof(VencRcParam, sQpMap) == 136, "VencRcParam.sQpMap mismatch");
static_assert(sizeof(VencGopParam) == 32, "VencGopParam ABI mismatch");
static_assert(sizeof(VencH264Param) == 408, "VencH264Param ABI mismatch");
static_assert(offsetof(VencH264Param, sGopParam) == 56, "VencH264Param.sGopParam mismatch");
static_assert(offsetof(VencH264Param, sRcParam) == 88, "VencH264Param.sRcParam mismatch");
static_assert(sizeof(VencH265Param) == 416, "VencH265Param ABI mismatch");
static_assert(offsetof(VencH265Param, sRcParam) == 64, "VencH265Param.sRcParam mismatch");
static_assert(offsetof(VencH265Param, sGopParam) == 384, "VencH265Param.sGopParam mismatch");
static_assert(sizeof(VencInputBuffer) == 376, "VencInputBuffer ABI mismatch");
static_assert(offsetof(VencInputBuffer, pAddrVirY) == 40, "VencInputBuffer.pAddrVirY mismatch");
static_assert(offsetof(VencInputBuffer, pAddrVirC) == 48, "VencInputBuffer.pAddrVirC mismatch");
static_assert(offsetof(VencInputBuffer, bEnableCorp) == 68, "VencInputBuffer.bEnableCorp mismatch");
static_assert(sizeof(VencOutputBuffer) == 216, "VencOutputBuffer ABI mismatch");
static_assert(offsetof(VencOutputBuffer, nFlag) == 16, "VencOutputBuffer.nFlag mismatch");
static_assert(offsetof(VencOutputBuffer, pData0) == 32, "VencOutputBuffer.pData0 mismatch");
static_assert(offsetof(VencOutputBuffer, pData1) == 40, "VencOutputBuffer.pData1 mismatch");
static_assert(offsetof(VencOutputBuffer, pData2) == 80, "VencOutputBuffer.pData2 mismatch");
static_assert(sizeof(VencAllocateBufferParam) == 12, "VencAllocateBufferParam ABI mismatch");
static_assert(sizeof(VencHeaderData) == 16, "VencHeaderData ABI mismatch");
static_assert(sizeof(VencH264ConstraintFlag) == 1, "VencH264ConstraintFlag ABI mismatch");
#endif

} // namespace cedarc_abi

#endif // CEDARC_ABI_H
