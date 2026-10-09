// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#ifndef LIB_EXTRAS_DEC_JXL_H_
#define LIB_EXTRAS_DEC_JXL_H_

// Decodes JPEG XL images in memory.

#include <jxl/memory_manager.h>
#include <jxl/parallel_runner.h>
#include <jxl/types.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "lib/extras/size_constraints.h"
#include "lib/jxl/dec_preview_internal.h"

namespace jxl {
namespace extras {

class PackedPixelFile;

enum class JXLPreviewBackend : uint32_t {
  kNone = 0,
  kEmbeddedPreview = 1,
  kNativeDcOnly = 2,
  kNativeProgressionFlush = 3,
  kFallbackDownsample = 4,
  kNativeReducedInput = 5,
  kNativeFusedUpsampling = 6,
  // Only for `preview_allowed_backends`, never reported: any of the methods
  // the decoder may choose to render a frame at the preview scale
  // (kNativeDcOnly, kNativeReducedInput, kNativeFusedUpsampling and
  // kFallbackDownsample).
  kDecoderDownsample = 7,
};

enum class JXLPreviewFailureReason : uint32_t {
  kNone = 0,
  kNoBackendAvailable = 1,
  // Not a JPEG XL file, truncated, or rejected by the decoder. The decoder
  // reports its own allocation failures as decoding errors too: a caller that
  // must tell them apart can record them in its `memory_manager`.
  kCorruptInput = 2,
  // The decoder or an output image could not be allocated.
  kOutOfMemory = 3,
};

struct JXLDecompressParams {
  // If empty, little endian float formats will be accepted.
  std::vector<JxlPixelFormat> accepted_formats;

  // Requested output color space description.
  std::string color_space;
  // Requested output color space description in case of CMYK images.
  std::string color_space_for_cmyk;
  // If set, performs tone mapping to this intensity target luminance.
  float display_nits = 0.0;
  // Whether spot colors are rendered on the image.
  bool render_spotcolors = true;
  // Whether to keep or undo the orientation given in the header.
  bool keep_orientation = false;
  // Coalescing or not
  bool coalescing = true;

  // If runner_opaque is set, the decoder uses this parallel runner.
  JxlParallelRunner runner;
  void* runner_opaque = nullptr;

  // If memory_manager is set, decoder uses it.
  JxlMemoryManager* memory_manager = nullptr;

  // Whether truncated input should be treated as an error.
  bool allow_partial_input = false;

  // How many passes to decode at most. By default, decode everything.
  uint32_t max_passes = std::numeric_limits<uint32_t>::max();

  // Alternatively, one can specify the maximum tolerable downscaling factor
  // with respect to the full size of the image. By default, nothing less than
  // the full size is requested.
  size_t max_downsampling = 1;

  // If greater than 1, the decoded output is downsampled by this factor before
  // being returned. This is intended for thumbnail / preview generation. The
  // decode budget is still controlled independently by `max_downsampling`.
  // A preview has the shape of a full decode at the reduced size: the image
  // size is DivCeil(size, factor), and without coalescing each layer has the
  // size DivCeil(layer size, factor) at the crop floor(crop / factor). Only the
  // first displayed frame is decoded (as with `first_frame_only`).
  size_t preview_downsampling = 1;
  // Optional output. If non-null, receives the backend used to satisfy the
  // preview request: kNone for a full decode (`preview_downsampling == 1`).
  // For a frame made of several layers (no coalescing), every layer's backend
  // is allowed and the first layer's is reported.
  JXLPreviewBackend* preview_backend = nullptr;
  // Bitmask of allowed preview backends. Uses 1u << JXLPreviewBackend value;
  // 0 means all backends are allowed. Bit 0 (kNone) allows the full decode
  // that `preview_downsampling == 1` asks for. kDecoderDownsample stands for
  // the set of the decoder's four render methods: allowing it allows each of
  // them, and allowing all four allows it. Allowing only some of the four
  // needs `preview_hooks`; without them the restriction must allow the whole
  // set.
  uint32_t preview_allowed_backends = 0;
  // Optional: the decoder's internal preview controls. Only code that links
  // the static jxl-internal library can provide them, as
  // jxl::GetDecoderPreviewHooks(). They let `preview_allowed_backends`
  // restrict the decoder's render methods, and the preview debug log name the
  // frame encoding.
  const JxlDecoderPreviewHooks* preview_hooks = nullptr;
  // Optional output. If non-null, receives a structured reason when decoding
  // fails: no allowed backend, corrupt input, or out of memory (kNone for
  // other failures).
  JXLPreviewFailureReason* preview_failure_reason = nullptr;
  // Stop after the first displayed frame (with coalescing, the first frame;
  // without, the layers up to the first with a duration or the last), also
  // without `preview_downsampling` (previews always stop there).
  bool first_frame_only = false;

  // Whether to use the image callback or the image buffer to get the output.
  bool use_image_callback = true;
  // Whether to unpremultiply colors for associated alpha channels.
  bool unpremultiply_alpha = false;

  // Controls the effective bit depth of the output pixels.
  JxlBitDepth output_bitdepth = {JXL_BIT_DEPTH_FROM_PIXEL_FORMAT, 0, 0};

  // Optional cancellation flag. If non-null, the decoder polls `*cancel` at
  // each iteration of the JxlDecoderProcessInput loop and aborts (returning
  // false) when it observes a non-zero value. The integer is treated as a
  // monotonic 0 -> non-zero hint; the decoder reads it without
  // synchronization. Writers from another thread should use atomic store
  // semantics (e.g. `std::atomic<int>` with `reinterpret_cast<int*>(&flag)`,
  // `__atomic_store_n` with `__ATOMIC_RELAXED`, or the platform's
  // interlocked store) to ensure timely visibility. The cancel takes effect at
  // the next iteration; one iteration can decode a frame up to its next
  // progression step.
  int* cancel = nullptr;
};

bool DecodeImageJXL(const uint8_t* bytes, size_t bytes_size,
                    const JXLDecompressParams& dparams, size_t* decoded_bytes,
                    PackedPixelFile* ppf,
                    std::vector<uint8_t>* jpeg_bytes = nullptr,
                    const SizeConstraints* constraints = nullptr);

}  // namespace extras
}  // namespace jxl

#endif  // LIB_EXTRAS_DEC_JXL_H_
