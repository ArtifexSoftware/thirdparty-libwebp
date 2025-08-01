// Copyright 2011 Google Inc. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the COPYING file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS. All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.
// -----------------------------------------------------------------------------
//
//   WebP encoder: internal header.
//
// Author: Skal (pascal.massimino@gmail.com)

#ifndef WEBP_ENC_VP8I_ENC_H_
#define WEBP_ENC_VP8I_ENC_H_

#include <string.h>  // for memcpy()

#include "src/dec/common_dec.h"
#include "src/dsp/cpu.h"
#include "src/dsp/dsp.h"
#include "src/utils/bit_writer_utils.h"
#include "src/utils/thread_utils.h"
#include "src/utils/utils.h"
#include "src/webp/encode.h"
#include "src/webp/types.h"

#ifdef __cplusplus
extern "C" {
#endif

//------------------------------------------------------------------------------
// Various defines and enums

// version numbers
#define ENC_MAJ_VERSION 1
#define ENC_MIN_VERSION 6
#define ENC_REV_VERSION 0

enum {
  MAX_LF_LEVELS = 64,       // Maximum loop filter level
  MAX_VARIABLE_LEVEL = 67,  // last (inclusive) level with variable cost
  MAX_LEVEL = 2047          // max level (note: max codable is 2047 + 67)
};

typedef enum {            // Rate-distortion optimization levels
  RD_OPT_NONE = 0,        // no rd-opt
  RD_OPT_BASIC = 1,       // basic scoring (no trellis)
  RD_OPT_TRELLIS = 2,     // perform trellis-quant on the final decision only
  RD_OPT_TRELLIS_ALL = 3  // trellis-quant for every scoring (much slower)
} VP8RDLevel;

// YUV-cache parameters. Cache is 32-bytes wide (= one cacheline).
// The original or reconstructed samples can be accessed using VP8Scan[].
// The predicted blocks can be accessed using offsets to 'yuv_p' and
// the arrays VP8*ModeOffsets[].
// * YUV Samples area ('yuv_in'/'yuv_out'/'yuv_out2')
//   (see VP8Scan[] for accessing the blocks, along with
//   Y_OFF_ENC/U_OFF_ENC/V_OFF_ENC):
//             +----+----+
//  Y_OFF_ENC  |YYYY|UUVV|
//  U_OFF_ENC  |YYYY|UUVV|
//  V_OFF_ENC  |YYYY|....| <- 25% wasted U/V area
//             |YYYY|....|
//             +----+----+
// * Prediction area ('yuv_p', size = PRED_SIZE_ENC)
//   Intra16 predictions (16x16 block each, two per row):
//         |I16DC16|I16TM16|
//         |I16VE16|I16HE16|
//   Chroma U/V predictions (16x8 block each, two per row):
//         |C8DC8|C8TM8|
//         |C8VE8|C8HE8|
//   Intra 4x4 predictions (4x4 block each)
//         |I4DC4 I4TM4 I4VE4 I4HE4|I4RD4 I4VR4 I4LD4 I4VL4|
//         |I4HD4 I4HU4 I4TMP .....|.......................| <- ~31% wasted
#define YUV_SIZE_ENC (BPS * 16)
#define PRED_SIZE_ENC (32 * BPS + 16 * BPS + 8 * BPS)  // I16+Chroma+I4 preds
#define Y_OFF_ENC (0)
#define U_OFF_ENC (16)
#define V_OFF_ENC (16 + 8)

extern const uint16_t VP8Scan[16];
extern const uint16_t VP8UVModeOffsets[4];
extern const uint16_t VP8I16ModeOffsets[4];

// Layout of prediction blocks
// intra 16x16
#define I16DC16 (0 * 16 * BPS)
#define I16TM16 (I16DC16 + 16)
#define I16VE16 (1 * 16 * BPS)
#define I16HE16 (I16VE16 + 16)
// chroma 8x8, two U/V blocks side by side (hence: 16x8 each)
#define C8DC8 (2 * 16 * BPS)
#define C8TM8 (C8DC8 + 1 * 16)
#define C8VE8 (2 * 16 * BPS + 8 * BPS)
#define C8HE8 (C8VE8 + 1 * 16)
// intra 4x4
#define I4DC4 (3 * 16 * BPS + 0)
#define I4TM4 (I4DC4 + 4)
#define I4VE4 (I4DC4 + 8)
#define I4HE4 (I4DC4 + 12)
#define I4RD4 (I4DC4 + 16)
#define I4VR4 (I4DC4 + 20)
#define I4LD4 (I4DC4 + 24)
#define I4VL4 (I4DC4 + 28)
#define I4HD4 (3 * 16 * BPS + 4 * BPS)
#define I4HU4 (I4HD4 + 4)
#define I4TMP (I4HD4 + 8)

typedef int64_t score_t;  // type used for scores, rate, distortion
// Note that MAX_COST is not the maximum allowed by sizeof(score_t),
// in order to allow overflowing computations.
#define MAX_COST ((score_t)0x7fffffffffffffLL)

#define QFIX 17
#define BIAS(b) ((b) << (QFIX - 8))
// Fun fact: this is the _only_ line where we're actually being lossy and
// discarding bits.
static WEBP_INLINE int QUANTDIV(uint32_t n, uint32_t iQ, uint32_t B) {
  return (int)((n * iQ + B) >> QFIX);
}

// Uncomment the following to remove token-buffer code:
// #define DISABLE_TOKEN_BUFFER

// quality below which error-diffusion is enabled
#define ERROR_DIFFUSION_QUALITY 98

//------------------------------------------------------------------------------
// Headers

typedef uint32_t proba_t;  // 16b + 16b
typedef uint8_t ProbaArray[NUM_CTX][NUM_PROBAS];
typedef proba_t StatsArray[NUM_CTX][NUM_PROBAS];
typedef uint16_t CostArray[NUM_CTX][MAX_VARIABLE_LEVEL + 1];
typedef const uint16_t* (*CostArrayPtr)[NUM_CTX];  // for easy casting
typedef const uint16_t* CostArrayMap[16][NUM_CTX];
typedef double LFStats[NUM_MB_SEGMENTS][MAX_LF_LEVELS];  // filter stats

typedef struct VP8Encoder VP8Encoder;

// segment features
typedef struct {
  int num_segments;  // Actual number of segments. 1 segment only = unused.
  int update_map;    // whether to update the segment map or not.
                     // must be 0 if there's only 1 segment.
  int size;          // bit-cost for transmitting the segment map
} VP8EncSegmentHeader;

// Struct collecting all frame-persistent probabilities.
typedef struct {
  uint8_t segments[3];  // probabilities for segment tree
  uint8_t skip_proba;   // final probability of being skipped.
  ProbaArray coeffs[NUM_TYPES][NUM_BANDS];     // 1056 bytes
  StatsArray stats[NUM_TYPES][NUM_BANDS];      // 4224 bytes
  CostArray level_cost[NUM_TYPES][NUM_BANDS];  // 13056 bytes
  CostArrayMap remapped_costs[NUM_TYPES];      // 1536 bytes
  int dirty;           // if true, need to call VP8CalculateLevelCosts()
  int use_skip_proba;  // Note: we always use skip_proba for now.
  int nb_skip;         // number of skipped blocks
} VP8EncProba;

// Filter parameters. Not actually used in the code (we don't perform
// the in-loop filtering), but filled from user's config
typedef struct {
  int simple;         // filtering type: 0=complex, 1=simple
  int level;          // base filter level [0..63]
  int sharpness;      // [0..7]
  int i4x4_lf_delta;  // delta filter level for i4x4 relative to i16x16
} VP8EncFilterHeader;

//------------------------------------------------------------------------------
// Informations about the macroblocks.

typedef struct {
  // block type
  unsigned int type : 2;  // 0=i4x4, 1=i16x16
  unsigned int uv_mode : 2;
  unsigned int skip : 1;
  unsigned int segment : 2;
  uint8_t alpha;  // quantization-susceptibility
} VP8MBInfo;

typedef struct VP8Matrix {
  uint16_t q[16];        // quantizer steps
  uint16_t iq[16];       // reciprocals, fixed point.
  uint32_t bias[16];     // rounding bias
  uint32_t zthresh[16];  // value below which a coefficient is zeroed
  uint16_t sharpen[16];  // frequency boosters for slight sharpening
} VP8Matrix;

typedef struct {
  VP8Matrix y1, y2, uv;  // quantization matrices
  int alpha;      // quant-susceptibility, range [-127,127]. Zero is neutral.
                  // Lower values indicate a lower risk of blurriness.
  int beta;       // filter-susceptibility, range [0,255].
  int quant;      // final segment quantizer.
  int fstrength;  // final in-loop filtering strength
  int max_edge;   // max edge delta (for filtering strength)
  int min_disto;  // minimum distortion required to trigger filtering record
  // reactivities
  int lambda_i16, lambda_i4, lambda_uv;
  int lambda_mode, lambda_trellis, tlambda;
  int lambda_trellis_i16, lambda_trellis_i4, lambda_trellis_uv;

  // lambda values for distortion-based evaluation
  score_t i4_penalty;  // penalty for using Intra4
} VP8SegmentInfo;

typedef int8_t DError[2 /* u/v */][2 /* top or left */];

// Handy transient struct to accumulate score and info during RD-optimization
// and mode evaluation.
typedef struct {
  score_t D, SD;            // Distortion, spectral distortion
  score_t H, R, score;      // header bits, rate, score.
  int16_t y_dc_levels[16];  // Quantized levels for luma-DC, luma-AC, chroma.
  int16_t y_ac_levels[16][16];
  int16_t uv_levels[4 + 4][16];
  int mode_i16;          // mode number for intra16 prediction
  uint8_t modes_i4[16];  // mode numbers for intra4 predictions
  int mode_uv;           // mode number of chroma prediction
  uint32_t nz;           // non-zero blocks
  int8_t derr[2][3];     // DC diffusion errors for U/V for blocks #1/2/3
} VP8ModeScore;

// Iterator structure to iterate through macroblocks, pointing to the
// right neighbouring data (samples, predictions, contexts, ...)
typedef struct {
  int x, y;           // current macroblock
  uint8_t* yuv_in;    // input samples
  uint8_t* yuv_out;   // output samples
  uint8_t* yuv_out2;  // secondary buffer swapped with yuv_out.
  uint8_t* yuv_p;     // scratch buffer for prediction
  VP8Encoder* enc;    // back-pointer
  VP8MBInfo* mb;      // current macroblock
  VP8BitWriter* bw;   // current bit-writer
  uint8_t* preds;     // intra mode predictors (4x4 blocks)
  uint32_t* nz;       // non-zero pattern
#if WEBP_AARCH64 && BPS == 32
  uint8_t i4_boundary[40];  // 32+8 boundary samples needed by intra4x4
#else
  uint8_t i4_boundary[37];  // 32+5 boundary samples needed by intra4x4
#endif
  uint8_t* i4_top;           // pointer to the current top boundary sample
  int i4;                    // current intra4x4 mode being tested
  int top_nz[9];             // top-non-zero context.
  int left_nz[9];            // left-non-zero. left_nz[8] is independent.
  uint64_t bit_count[4][3];  // bit counters for coded levels.
  uint64_t luma_bits;        // macroblock bit-cost for luma
  uint64_t uv_bits;          // macroblock bit-cost for chroma
  LFStats* lf_stats;         // filter stats (borrowed from enc)
  int do_trellis;            // if true, perform extra level optimisation
  int count_down;            // number of mb still to be processed
  int count_down0;           // starting counter value (for progress)
  int percent0;              // saved initial progress percent

  DError left_derr;  // left error diffusion (u/v)
  DError* top_derr;  // top diffusion error - NULL if disabled

  uint8_t* y_left;  // left luma samples (addressable from index -1 to 15).
  uint8_t* u_left;  // left u samples (addressable from index -1 to 7)
  uint8_t* v_left;  // left v samples (addressable from index -1 to 7)

  uint8_t* y_top;   // top luma samples at position 'x'
  uint8_t* uv_top;  // top u/v samples at position 'x', packed as 16 bytes

  // memory for storing y/u/v_left
  uint8_t yuv_left_mem[17 + 16 + 16 + 8 + WEBP_ALIGN_CST];
  // memory for yuv*
  uint8_t yuv_mem[3 * YUV_SIZE_ENC + PRED_SIZE_ENC + WEBP_ALIGN_CST];
} VP8EncIterator;

// in iterator.c
// must be called first
void VP8IteratorInit(VP8Encoder* const enc, VP8EncIterator* const it);
// reset iterator position to row 'y'
void VP8IteratorSetRow(VP8EncIterator* const it, int y);
// set count down (=number of iterations to go)
void VP8IteratorSetCountDown(VP8EncIterator* const it, int count_down);
// return true if iteration is finished
int VP8IteratorIsDone(const VP8EncIterator* const it);
// Import uncompressed samples from source.
// If tmp_32 is not NULL, import boundary samples too.
// tmp_32 is a 32-bytes scratch buffer that must be aligned in memory.
void VP8IteratorImport(VP8EncIterator* const it, uint8_t* const tmp_32);
// export decimated samples
void VP8IteratorExport(const VP8EncIterator* const it);
// go to next macroblock. Returns false if not finished.
int VP8IteratorNext(VP8EncIterator* const it);
// save the 'yuv_out' boundary values to 'top'/'left' arrays for next
// iterations.
void VP8IteratorSaveBoundary(VP8EncIterator* const it);
// Report progression based on macroblock rows. Return 0 for user-abort request.
int VP8IteratorProgress(const VP8EncIterator* const it, int delta);
// Intra4x4 iterations
void VP8IteratorStartI4(VP8EncIterator* const it);
// returns true if not done.
int VP8IteratorRotateI4(VP8EncIterator* const it, const uint8_t* const yuv_out);

// Non-zero context setup/teardown
void VP8IteratorNzToBytes(VP8EncIterator* const it);
void VP8IteratorBytesToNz(VP8EncIterator* const it);

// Helper functions to set mode properties
void VP8SetIntra16Mode(const VP8EncIterator* const it, int mode);
void VP8SetIntra4Mode(const VP8EncIterator* const it, const uint8_t* modes);
void VP8SetIntraUVMode(const VP8EncIterator* const it, int mode);
void VP8SetSkip(const VP8EncIterator* const it, int skip);
void VP8SetSegment(const VP8EncIterator* const it, int segment);

//------------------------------------------------------------------------------
// Paginated token buffer

typedef struct VP8Tokens VP8Tokens;  // struct details in token.c

typedef struct {
#if !defined(DISABLE_TOKEN_BUFFER)
  VP8Tokens* pages;       // first page
  VP8Tokens** last_page;  // last page
  uint16_t* tokens;       // set to (*last_page)->tokens
  int left;               // how many free tokens left before the page is full
  int page_size;          // number of tokens per page
#endif
  int error;  // true in case of malloc error
} VP8TBuffer;

// initialize an empty buffer
void VP8TBufferInit(VP8TBuffer* const b, int page_size);
void VP8TBufferClear(VP8TBuffer* const b);  // de-allocate pages memory

#if !defined(DISABLE_TOKEN_BUFFER)

// Finalizes bitstream when probabilities are known.
// Deletes the allocated token memory if final_pass is true.
int VP8EmitTokens(VP8TBuffer* const b, VP8BitWriter* const bw,
                  const uint8_t* const probas, int final_pass);

// record the coding of coefficients without knowing the probabilities yet
int VP8RecordCoeffTokens(int ctx, const struct VP8Residual* const res,
                         VP8TBuffer* const tokens);

// Estimate the final coded size given a set of 'probas'.
size_t VP8EstimateTokenSize(VP8TBuffer* const b, const uint8_t* const probas);

#endif  // !DISABLE_TOKEN_BUFFER

//------------------------------------------------------------------------------
// VP8Encoder

struct VP8Encoder {
  const WebPConfig* config;  // user configuration and parameters
  WebPPicture* pic;          // input / output picture

  // headers
  VP8EncFilterHeader filter_hdr;    // filtering information
  VP8EncSegmentHeader segment_hdr;  // segment information

  int profile;  // VP8's profile, deduced from Config.

  // dimension, in macroblock units.
  int mb_w, mb_h;
  int preds_w;  // stride of the *preds prediction plane (=4*mb_w + 1)

  // number of partitions (1, 2, 4 or 8 = MAX_NUM_PARTITIONS)
  int num_parts;

  // per-partition boolean decoders.
  VP8BitWriter bw;                         // part0
  VP8BitWriter parts[MAX_NUM_PARTITIONS];  // token partitions
  VP8TBuffer tokens;                       // token buffer

  int percent;  // for progress

  // transparency blob
  int has_alpha;
  uint8_t* alpha_data;  // non-NULL if transparency is present
  uint32_t alpha_data_size;
  WebPWorker alpha_worker;

  // quantization info (one set of DC/AC dequant factor per segment)
  VP8SegmentInfo dqm[NUM_MB_SEGMENTS];
  int base_quant;  // nominal quantizer value. Only used
                   // for relative coding of segments' quant.
  int alpha;       // global susceptibility (<=> complexity)
  int uv_alpha;    // U/V quantization susceptibility
  // global offset of quantizers, shared by all segments
  int dq_y1_dc;
  int dq_y2_dc, dq_y2_ac;
  int dq_uv_dc, dq_uv_ac;

  // probabilities and statistics
  VP8EncProba proba;
  uint64_t sse[4];     // sum of Y/U/V/A squared errors for all macroblocks
  uint64_t sse_count;  // pixel count for the sse[] stats
  int coded_size;
  int residual_bytes[3][4];
  int block_count[3];

  // quality/speed settings
  int method;               // 0=fastest, 6=best/slowest.
  VP8RDLevel rd_opt_level;  // Deduced from method.
  int max_i4_header_bits;   // partition #0 safeness factor
  int mb_header_limit;      // rough limit for header bits per MB
  int thread_level;         // derived from config->thread_level
  int do_search;            // derived from config->target_XXX
  int use_tokens;           // if true, use token buffer

  // Memory
  VP8MBInfo* mb_info;  // contextual macroblock infos (mb_w + 1)
  uint8_t* preds;      // predictions modes: (4*mb_w+1) * (4*mb_h+1)
  uint32_t* nz;        // non-zero bit context: mb_w+1
  uint8_t* y_top;      // top luma samples.
  uint8_t* uv_top;     // top u/v samples.
                       // U and V are packed into 16 bytes (8 U + 8 V)
  LFStats* lf_stats;   // autofilter stats (if NULL, autofilter is off)
  DError* top_derr;    // diffusion error (NULL if disabled)
};

//------------------------------------------------------------------------------
// internal functions. Not public.

// in tree.c
extern const uint8_t VP8CoeffsProba0[NUM_TYPES][NUM_BANDS][NUM_CTX][NUM_PROBAS];
extern const uint8_t VP8CoeffsUpdateProba[NUM_TYPES][NUM_BANDS][NUM_CTX]
                                         [NUM_PROBAS];
// Reset the token probabilities to their initial (default) values
void VP8DefaultProbas(VP8Encoder* const enc);
// Write the token probabilities
void VP8WriteProbas(VP8BitWriter* const bw, const VP8EncProba* const probas);
// Writes the partition #0 modes (that is: all intra modes)
void VP8CodeIntraModes(VP8Encoder* const enc);

// in syntax.c
// Generates the final bitstream by coding the partition0 and headers,
// and appending an assembly of all the pre-coded token partitions.
// Return true if everything is ok.
int VP8EncWrite(VP8Encoder* const enc);
// Release memory allocated for bit-writing in VP8EncLoop & seq.
void VP8EncFreeBitWriters(VP8Encoder* const enc);

// in frame.c
extern const uint8_t VP8Cat3[];
extern const uint8_t VP8Cat4[];
extern const uint8_t VP8Cat5[];
extern const uint8_t VP8Cat6[];

// Form all the four Intra16x16 predictions in the 'yuv_p' cache
void VP8MakeLuma16Preds(const VP8EncIterator* const it);
// Form all the four Chroma8x8 predictions in the 'yuv_p' cache
void VP8MakeChroma8Preds(const VP8EncIterator* const it);
// Rate calculation
int VP8GetCostLuma16(VP8EncIterator* const it, const VP8ModeScore* const rd);
int VP8GetCostLuma4(VP8EncIterator* const it, const int16_t levels[16]);
int VP8GetCostUV(VP8EncIterator* const it, const VP8ModeScore* const rd);
// Main coding calls
int VP8EncLoop(VP8Encoder* const enc);
int VP8EncTokenLoop(VP8Encoder* const enc);

// in webpenc.c
// Assign an error code to a picture. Return false for convenience.
int WebPEncodingSetError(const WebPPicture* const pic, WebPEncodingError error);
int WebPReportProgress(const WebPPicture* const pic, int percent,
                       int* const percent_store);

// in analysis.c
// Main analysis loop. Decides the segmentations and complexity.
// Assigns a first guess for Intra16 and 'uvmode' prediction modes.
int VP8EncAnalyze(VP8Encoder* const enc);

// in quant.c
// Sets up segment's quantization values, 'base_quant' and filter strengths.
void VP8SetSegmentParams(VP8Encoder* const enc, float quality);
// Pick best modes and fills the levels. Returns true if skipped.
int VP8Decimate(VP8EncIterator* WEBP_RESTRICT const it,
                VP8ModeScore* WEBP_RESTRICT const rd, VP8RDLevel rd_opt);

// in alpha.c
void VP8EncInitAlpha(VP8Encoder* const enc);   // initialize alpha compression
int VP8EncStartAlpha(VP8Encoder* const enc);   // start alpha coding process
int VP8EncFinishAlpha(VP8Encoder* const enc);  // finalize compressed data
int VP8EncDeleteAlpha(VP8Encoder* const enc);  // delete compressed data

// autofilter
void VP8InitFilter(VP8EncIterator* const it);
void VP8StoreFilterStats(VP8EncIterator* const it);
void VP8AdjustFilterStrength(VP8EncIterator* const it);

// returns the approximate filtering strength needed to smooth a edge
// step of 'delta', given a sharpness parameter 'sharpness'.
int VP8FilterStrengthFromDelta(int sharpness, int delta);

// misc utils for picture_*.c:

// Returns true if 'picture' is non-NULL and dimensions/colorspace are within
// their valid ranges. If returning false, the 'error_code' in 'picture' is
// updated.
int WebPValidatePicture(const WebPPicture* const picture);

// Remove reference to the ARGB/YUVA buffer (doesn't free anything).
void WebPPictureResetBuffers(WebPPicture* const picture);

// Allocates ARGB buffer according to set width/height (previous one is
// always free'd). Preserves the YUV(A) buffer. Returns false in case of error
// (invalid param, out-of-memory).
int WebPPictureAllocARGB(WebPPicture* const picture);

// Allocates YUVA buffer according to set width/height (previous one is always
// free'd). Uses picture->csp to determine whether an alpha buffer is needed.
// Preserves the ARGB buffer.
// Returns false in case of error (invalid param, out-of-memory).
int WebPPictureAllocYUVA(WebPPicture* const picture);

// Replace samples that are fully transparent by 'color' to help compressibility
// (no guarantee, though). Assumes pic->use_argb is true.
void WebPReplaceTransparentPixels(WebPPicture* const pic, uint32_t color);

//------------------------------------------------------------------------------

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // WEBP_ENC_VP8I_ENC_H_
