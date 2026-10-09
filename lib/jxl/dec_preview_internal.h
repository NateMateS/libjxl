/* Copyright (c) the JPEG XL Project Authors. All rights reserved.
 *
 * Use of this source code is governed by a BSD-style
 * license that can be found in the LICENSE file.
 */

/* PRIVATE libjxl header: internal preview controls of the decoder.
 *
 * Not installed and not part of the API. The functions behind these hooks are
 * deliberately not exported from the shared library. Code that links the
 * static jxl-internal library (in-tree tools, tests, benchmarks) obtains them
 * with jxl::GetDecoderPreviewHooks() and passes them to lib/extras through
 * JXLDecompressParams::preview_hooks. lib/extras includes this header for the
 * types only, so that it builds against the shared library too.
 */

#ifndef LIB_JXL_DEC_PREVIEW_INTERNAL_H_
#define LIB_JXL_DEC_PREVIEW_INTERNAL_H_

#include <jxl/decode.h>
#include <jxl/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Encoding of a frame. */
typedef enum {
  JXL_FRAME_ENCODING_UNKNOWN = 0,
  JXL_FRAME_ENCODING_VAR_DCT = 1,
  JXL_FRAME_ENCODING_MODULAR = 2,
} JxlFrameEncoding;

typedef struct {
  /* Restricts the methods other than FULL_RESOLUTION (see
   * JxlImageOutDownsamplingMethod) that the decoder may choose for the
   * current frame; by default all are allowed, and they are again for the
   * next frame. Must be called after JXL_DEC_FRAME and before the image
   * output buffer or callback, or an extra channel buffer, is set. The chosen
   * method is public: JxlDecoderGetImageOutDownsamplingMethod. */
  JxlDecoderStatus (*set_native_paths)(JxlDecoder* dec,
                                       JXL_BOOL allow_reduced_input,
                                       JXL_BOOL allow_fused_upsampling,
                                       JXL_BOOL allow_dc_only);
  /* The encoding of the current frame. Available after JXL_DEC_FRAME. */
  JxlDecoderStatus (*get_frame_encoding)(const JxlDecoder* dec,
                                         JxlFrameEncoding* encoding);
} JxlDecoderPreviewHooks;

#ifdef __cplusplus
}  // extern "C"

namespace jxl {
// Defined in decode.cc; not exported from the shared library.
const JxlDecoderPreviewHooks* GetDecoderPreviewHooks();
}  // namespace jxl
#endif

#endif /* LIB_JXL_DEC_PREVIEW_INTERNAL_H_ */
