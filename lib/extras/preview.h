// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

// One-shot preview decoder for JPEG XL.
//
// A wrapper above the streaming JxlDecoder API (through
// jxl::extras::DecodeImageJXL) for in-tree consumers needing a fast partial
// decode, such as thumbnails, gallery grids and progressive placeholders. It
// picks the cheapest available way to produce the requested output size: an
// embedded preview frame, a progression flush, or decoder-side downsampling
// (rendering from the DC image, at reduced resolution, or box-downsampling
// the full resolution rendering).
//
// Two entry points:
//   - JxlGetPreviewInfo: cheap header probe (size, channels, ICC).
//   - JxlGeneratePreview: full decode, configurable via JxlPreviewOptions.
//
// Both are thread-safe (reentrant). Concurrent calls must not share a
// JxlParallelRunner runner state.
//
// This is not part of the libjxl API and is not installed: it is built into
// the extras libraries (jxl_extras-internal and jxl_extras_codec). Programs
// outside the libjxl source tree get the same decoder-side downsampling from
// the public API with JxlDecoderSetImageOutDownsampling; see
// examples/decode_preview.cc.

#ifndef LIB_EXTRAS_PREVIEW_H_
#define LIB_EXTRAS_PREVIEW_H_

#include <jxl/color_encoding.h>
#include <jxl/decode.h>
#include <jxl/memory_manager.h>
#include <jxl/parallel_runner.h>
#include <jxl/types.h>
#include <stddef.h>
#include <stdint.h>

#include "lib/jxl/dec_preview_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Granular result code for the preview API.
 *
 * Returned by @ref JxlGetPreviewInfo and @ref JxlGeneratePreview in place of
 * @ref JxlDecoderStatus, which is too coarse for one-shot consumers.
 */
typedef enum {
  /** Operation completed successfully. */
  JXL_PREVIEW_SUCCESS = 0,
  /** A required pointer argument was NULL, a numeric argument was out of
   * range (e.g. `preview_downsampling` not in {0, 1, 2, 4, 8}), or the memory
   * manager or the color encoding is invalid. */
  JXL_PREVIEW_INVALID_ARGUMENT = 1,
  /** The input is not a JPEG XL bitstream, or is truncated/corrupt. */
  JXL_PREVIEW_CORRUPT_INPUT = 2,
  /** A memory allocation failed (either via the supplied
   * @ref JxlMemoryManager or via the system allocator). */
  JXL_PREVIEW_OUT_OF_MEMORY = 3,
  /** The caller supplied an output buffer (@ref JxlPreviewOptions.dst) that
   * was too small for the decoded preview, or a @ref JxlPreviewOptions.
   * dst_stride below its row size. Nothing was written to it; the outputs
   * describe the preview and the buffer it needs (see
   * @ref JxlGeneratePreview). */
  JXL_PREVIEW_BUFFER_TOO_SMALL = 4,
  /** The decode was cancelled via the @ref JxlPreviewOptions.cancel flag. */
  JXL_PREVIEW_CANCELLED = 5,
  /** The requested @ref JxlPixelFormat is not supported: its data type,
   * endianness, or a channel count above 4. */
  JXL_PREVIEW_UNSUPPORTED_FORMAT = 6,
  /** The bitstream could not be served by any backend in
   * @ref JxlPreviewOptions.allowed_backends (e.g. the caller required an
   * embedded preview frame but the file does not contain one). */
  JXL_PREVIEW_NO_BACKEND_AVAILABLE = 7,
  /** Internal decoder error not falling into any of the above categories. */
  JXL_PREVIEW_INTERNAL_ERROR = 8,
} JxlPreviewStatus;

/** Backend that the library used to satisfy a preview request.
 *
 * Returned through @ref JxlPreviewOptions.out_backend_used. Values 2, 4, 5
 * and 6 are the decoder's render methods (see
 * JxlDecoderGetImageOutDownsamplingMethod).
 */
typedef enum {
  /** No preview decoding took place: a full decode (downsampling == 1). */
  JXL_PREVIEW_BACKEND_NONE = 0,
  /** A preview frame embedded in the JXL container was used. */
  JXL_PREVIEW_BACKEND_EMBEDDED_PREVIEW = 1,
  /** Rendered from the DC image (1/8 of the full image) alone. */
  JXL_PREVIEW_BACKEND_NATIVE_DC_ONLY = 2,
  /** Decoded a reduced number of progressive passes; output flushed mid-frame.
   */
  JXL_PREVIEW_BACKEND_NATIVE_PROGRESSION_FLUSH = 3,
  /** Rendered at full resolution and box-downsampled by the decoder. */
  JXL_PREVIEW_BACKEND_FALLBACK_DOWNSAMPLE = 4,
  /** Rendered at reduced resolution instead of full-size pixels. */
  JXL_PREVIEW_BACKEND_NATIVE_REDUCED_INPUT = 5,
  /** Rendered a frame-upsampled image directly at preview scale. */
  JXL_PREVIEW_BACKEND_NATIVE_FUSED_UPSAMPLING = 6,
  /** Never reported: in @ref JxlPreviewOptions.allowed_backends, any of the
   * decoder's render methods 2, 4, 5 and 6. */
  JXL_PREVIEW_BACKEND_DECODER_DOWNSAMPLE = 7,
} JxlPreviewBackend;

/** Bit values for @ref JxlPreviewOptions.allowed_backends: bit `1 << b` for
 * the backend of value `b`.
 *
 * Combine with bitwise OR to restrict which backends @ref JxlGeneratePreview
 * is allowed to use. A value of 0 means "all backends allowed" (default).
 */
typedef enum {
  /** The full decode of factor 1 (@ref JXL_PREVIEW_BACKEND_NONE). */
  JXL_PREVIEW_BACKEND_BIT_FULL_DECODE = 1u << 0,
  JXL_PREVIEW_BACKEND_BIT_EMBEDDED_PREVIEW = 1u << 1,
  JXL_PREVIEW_BACKEND_BIT_NATIVE_DC_ONLY = 1u << 2,
  JXL_PREVIEW_BACKEND_BIT_NATIVE_PROGRESSION_FLUSH = 1u << 3,
  JXL_PREVIEW_BACKEND_BIT_FALLBACK_DOWNSAMPLE = 1u << 4,
  JXL_PREVIEW_BACKEND_BIT_NATIVE_REDUCED_INPUT = 1u << 5,
  JXL_PREVIEW_BACKEND_BIT_NATIVE_FUSED_UPSAMPLING = 1u << 6,
  JXL_PREVIEW_BACKEND_BIT_DECODER_DOWNSAMPLE = 1u << 7,
} JxlPreviewBackendBits;

/** Sentinel for @ref JxlPreviewOptions.display_nits meaning "do not perform
 * any HDR-to-SDR tone mapping; emit raw values in the source colorimetry".
 * With a NULL @ref JxlPreviewOptions.color_encoding, the output is in the
 * decoder's default output encoding: the source's, or linear sRGB for an
 * XYB-encoded image whose color space can only be described by an ICC
 * profile. CMYK sources are still converted to sRGB, since the output has no
 * black channel.
 *
 * The default value (0) instead means "auto-pick a sensible SDR target"
 * (currently 250 nits, matching common SDR display peak luminance), which
 * is what most consumers want.
 */
#define JXL_PREVIEW_NO_TONE_MAPPING (-1.0f)

/** Per-call options for @ref JxlGeneratePreview.
 *
 * All fields are optional. A zero-initialised struct produces sensible
 * defaults: factor-1 (full-resolution) decode into a freshly-allocated,
 * tightly-packed RGBA8 sRGB buffer, with auto SDR tone mapping for HDR
 * sources.
 *
 * @note Use @ref JxlPreviewOptionsInit to portably zero-initialise (it also
 * future-proofs callers against new fields being added with non-zero
 * defaults).
 */
typedef struct {
  /* --- Output sizing ----------------------------------------------------- */

  /** Downsampling factor. Must be 0, 1, 2, 4, or 8.
   *  - `0` means "auto-pick from @ref target_xsize / @ref target_ysize".
   *    If those are also 0, defaults to 1 (full-resolution decode).
   *  - Non-zero overrides @ref target_xsize / @ref target_ysize.
   */
  uint32_t preview_downsampling;

  /** Target output width in pixels. Used when @ref preview_downsampling is 0
   * to pick the largest factor in {1, 2, 4, 8} whose output is still at least
   * @ref target_xsize wide (and @ref target_ysize high): the smallest output
   * that covers the target, or factor 1 if the image is smaller than it. The
   * actual output may be larger; further box-downsampling is not performed.
   * 0 means "no width constraint". */
  uint32_t target_xsize;
  /** Target output height. See @ref target_xsize. */
  uint32_t target_ysize;

  /* --- Output buffer ----------------------------------------------------- */

  /** Requested output pixel format.
   *  - If `format.num_channels == 0`, the whole format defaults to RGBA8
   *    (`{4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0}`), whatever its other
   *    fields hold.
   *  - Otherwise `format.num_channels` must be 1 to 4, but does not select
   *    the layout: the output has the channels of the source (grey or color,
   *    in the output color space, plus alpha if the source has it), e.g. an
   *    RGB image always produces a 3-channel output even when 4 was
   *    requested. The actual format is reported via @ref out_format and the
   *    row stride via @ref out_stride.
   *  - `format.data_type` is honored: pick `JXL_TYPE_UINT8`,
   *    `JXL_TYPE_UINT16`, `JXL_TYPE_FLOAT16`, or `JXL_TYPE_FLOAT`.
   *  - `format.endianness` is honored (native, little or big).
   *  - `format.align` is ignored: allocated output is tightly packed, and
   *    the rows of a caller-supplied buffer are laid out by @ref dst_stride.
   * Anything else is @ref JXL_PREVIEW_UNSUPPORTED_FORMAT.
   */
  JxlPixelFormat format;

  /** Optional caller-supplied destination buffer.
   *  - If non-NULL, the preview is copied into `dst` row by row, with rows
   *    @ref dst_stride bytes apart, once it has been decoded; it is not
   *    decoded in place. The caller MUST set @ref dst_size to the buffer
   *    capacity in bytes; if too small, @ref JXL_PREVIEW_BUFFER_TOO_SMALL is
   *    returned without writing to `dst`.
   *  - If NULL, the decoder allocates a buffer via @ref memory_manager
   *    (or `malloc` if none) and stores its pointer in
   *    @ref out_pixels. The caller takes ownership and MUST free it with
   *    the matching free function.
   */
  uint8_t* dst;
  /** Capacity of @ref dst in bytes. Ignored when `dst == NULL`. */
  size_t dst_size;
  /** Row stride in bytes for @ref dst. 0 means "tightly packed"
   * (stride = `out_xsize * bytes_per_pixel`). A stride below that row size
   * (which depends on the source's channels) is
   * @ref JXL_PREVIEW_BUFFER_TOO_SMALL. Ignored when `dst == NULL`; allocated
   * buffers are always tightly packed. */
  size_t dst_stride;

  /* --- Color management -------------------------------------------------- */

  /** Requested output color encoding. NULL = sRGB D65 SDR Rel (grey sRGB for
   * grayscale sources), unless @ref display_nits is
   * @ref JXL_PREVIEW_NO_TONE_MAPPING. Otherwise it must describe an RGB or
   * grey color space with a known transfer function and valid fields
   * (@ref JXL_PREVIEW_INVALID_ARGUMENT otherwise).
   * The pointed-to struct is consulted only during the call; not retained. */
  const JxlColorEncoding* color_encoding;
  /** Display peak luminance for HDR-to-SDR tone mapping, in nits.
   *  - `0` (default): auto-pick a sensible SDR target (currently 250 nits).
   *  - @ref JXL_PREVIEW_NO_TONE_MAPPING - skip tone mapping entirely.
   *  - From 2^-24 to 65504: tone-map to this peak luminance.
   *  - Anything else: @ref JXL_PREVIEW_INVALID_ARGUMENT.
   */
  float display_nits;

  /* --- Resource control -------------------------------------------------- */

  /** Optional parallel runner for multi-threaded decode, used only when
   * @ref runner_opaque is non-NULL. NULL = single threaded. The runner state
   * must not be shared between concurrent calls. */
  JxlParallelRunner runner;
  /** Opaque pointer passed to @ref runner. */
  void* runner_opaque;
  /** Optional memory manager for the decoder and the returned buffer. NULL =
   * system `malloc`/`free`. `alloc` and `free` must be both set or both NULL
   * (@ref JXL_PREVIEW_INVALID_ARGUMENT otherwise). The decoded images held
   * during the call are allocated with the system allocator. */
  JxlMemoryManager* memory_manager;

  /* --- Backend selection ------------------------------------------------- */

  /** Bitmask of allowed backends, built from @ref JxlPreviewBackendBits.
   * 0 means "all backends allowed" (default); a non-zero mask allows only
   * its backends, so the reported backend is always one of them (factor 1
   * needs @ref JXL_PREVIEW_BACKEND_BIT_FULL_DECODE). Backend selection
   * honors this before choosing a preview path; for example, if native fused
   * upsampling is disallowed but fallback downsampling is allowed, the
   * decoder uses fallback instead. @ref
   * JXL_PREVIEW_BACKEND_BIT_DECODER_DOWNSAMPLE stands for the set of the
   * decoder's render methods (bits 2, 4, 5 and 6): allowing it allows each of
   * them, and allowing all four allows it. Restricting the methods individually
   * requires @ref preview_hooks; without them, the mask must allow the whole
   * set. If the bitstream cannot be served by any allowed backend, @ref
   * JXL_PREVIEW_NO_BACKEND_AVAILABLE is returned. */
  uint32_t allowed_backends;

  /** Optional: the decoder's internal preview controls, which only code that
   * links the static jxl-internal library can provide, as
   * jxl::GetDecoderPreviewHooks(). They let @ref allowed_backends restrict
   * the decoder's render methods individually. */
  const JxlDecoderPreviewHooks* preview_hooks;

  /* --- Cancellation ------------------------------------------------------ */

  /** Optional cancellation flag. If non-NULL, the decoder reads `*cancel`
   * at decode-loop boundaries and aborts with @ref JXL_PREVIEW_CANCELLED if
   * non-zero.
   *
   * Threading contract: the integer is treated as a monotonic 0 -> non-zero
   * hint and the decoder reads it without synchronization. Writers from a
   * different thread should use platform atomic store semantics, e.g.
   * `std::atomic<int>` accessed as `reinterpret_cast<int*>(&flag)`,
   * `__atomic_store_n(&flag, 1, __ATOMIC_RELAXED)`, or
   * `_InterlockedExchange`. The cancel takes effect at the next decode-loop
   * boundary; decoding a frame up to its next progression step happens
   * between two boundaries. */
  int* cancel;

  /* --- Outputs (filled by JxlGeneratePreview) ---------------------------- */

  /** Receives the actual preview width in pixels. Non-NULL required. */
  uint32_t* out_xsize;
  /** Receives the actual preview height in pixels. Non-NULL required. */
  uint32_t* out_ysize;
  /** Receives the row stride in bytes of the produced output (matches
   * @ref dst_stride when @ref dst is supplied; for allocated buffers,
   * equals `out_xsize * bytes_per_pixel`). May be NULL. */
  size_t* out_stride;
  /** When @ref dst is NULL, receives the newly-allocated pixel buffer; the
   * caller takes ownership. When @ref dst is non-NULL, set to @ref dst.
   * Non-NULL required. */
  uint8_t** out_pixels;
  /** Receives the pixel buffer size in bytes. May be NULL. */
  size_t* out_pixels_size;
  /** Receives the actual output pixel format (`num_channels` may differ
   * from the requested format when the source has fewer/more channels;
   * `data_type` is preserved; `align` is 0). May be NULL. */
  JxlPixelFormat* out_format;
  /** Receives the backend actually used. May be NULL. */
  JxlPreviewBackend* out_backend_used;
  /** Receives the actual downsampling factor used (1, 2, 4, or 8).
   * When @ref preview_downsampling is 0, this is the auto-selected factor.
   * May be NULL. */
  uint32_t* out_downsampling;
  /** Receives the color encoding of the output pixels. A color space of
   * `JXL_COLOR_SPACE_UNKNOWN` means the output is described by an ICC profile
   * only: the one @ref JxlGetPreviewInfo returns through
   * @ref JxlPreviewInfoQuery.out_icc. That happens only for such sources
   * decoded with @ref JXL_PREVIEW_NO_TONE_MAPPING and a NULL
   * @ref color_encoding. May be NULL. */
  JxlColorEncoding* out_color_encoding;
} JxlPreviewOptions;

/** Header-only metadata about a JPEG XL bitstream, returned by
 * @ref JxlGetPreviewInfo without performing a codestream decode.
 */
typedef struct {
  /** Full image width in pixels (before any preview downsampling). */
  uint32_t xsize;
  /** Full image height in pixels. */
  uint32_t ysize;
  /** Number of color channels (1 for grayscale, 3 for RGB). */
  uint32_t num_color_channels;
  /** Non-zero if the image has an alpha channel. */
  int has_alpha;
  /** Non-zero if the image header declares an animation (which may still
   * have a single frame). */
  int is_animated;
  /** Original intensity target in nits (e.g. 255 for SDR, 1000+ for HDR). */
  float intensity_target;
  /** Largest factor in {1, 2, 4, 8} whose output is still at least
   * `query.target_xsize` × `query.target_ysize` pixels: the smallest output
   * that covers the target. 1 if the caller did not request a target size,
   * or if the image is smaller than the target. */
  uint32_t recommended_factor;
  /** Computed output width in pixels when using @ref recommended_factor. */
  uint32_t recommended_xsize;
  /** Computed output height in pixels when using @ref recommended_factor. */
  uint32_t recommended_ysize;
} JxlPreviewInfo;

/** Optional inputs for @ref JxlGetPreviewInfo. NULL is equivalent to a
 * zero-initialised struct (no target size, no ICC retrieval). */
typedef struct {
  /** Target output width in pixels (used to compute `recommended_factor`). */
  uint32_t target_xsize;
  /** Target output height. */
  uint32_t target_ysize;
  /** If non-NULL, receives the ICC profile of the pixels the decoder outputs
   * by default (`JXL_COLOR_PROFILE_TARGET_DATA`): for images that are not
   * XYB-encoded, the embedded profile. Allocated via @ref memory_manager, or
   * `malloc` if NULL; the caller takes ownership. Set to NULL on input to skip
   * ICC retrieval. */
  uint8_t** out_icc;
  /** Receives the size of @ref out_icc in bytes. Required when
   * `out_icc != NULL`, else may be NULL. */
  size_t* out_icc_size;
  /** Optional memory manager for the decoder and the ICC allocation. `alloc`
   * and `free` must be both set or both NULL. */
  JxlMemoryManager* memory_manager;
} JxlPreviewInfoQuery;

/** Probe a JPEG XL bitstream's header without performing a codestream
 * decode. Cheap (parses only signature + image header).
 *
 * @param input  Pointer to the JPEG XL bitstream.
 * @param input_size  Size of `input` in bytes.
 * @param query  Optional query parameters. NULL = no target size, no ICC.
 * @param info  Receives the populated @ref JxlPreviewInfo. Non-NULL required.
 * @return @ref JXL_PREVIEW_SUCCESS on success, otherwise an error code.
 */
JxlPreviewStatus JxlGetPreviewInfo(const uint8_t* input, size_t input_size,
                                   const JxlPreviewInfoQuery* query,
                                   JxlPreviewInfo* info);

/** One-shot preview decode driven by @ref JxlPreviewOptions.
 *
 * Decodes the first frame of the given JPEG XL bitstream (only the first
 * frame of an animation) at the resolution implied by
 * `options->preview_downsampling` (or `options->target_xsize` /
 * `target_ysize`), automatically choosing the cheapest backend allowed by
 * `options->allowed_backends`. HDR sources are tone-mapped to SDR by
 * default; see @ref JxlPreviewOptions.display_nits to override.
 *
 * @param input  Pointer to the JPEG XL bitstream.
 * @param input_size  Size of `input` in bytes.
 * @param options  Per-call options. Non-NULL required. Use
 *     @ref JxlPreviewOptionsInit to construct.
 * @return @ref JXL_PREVIEW_SUCCESS on success, otherwise an error code.
 *     Every non-NULL output is reset first. On error, no output buffer is
 *     allocated and `*options->out_pixels` is NULL. On
 *     @ref JXL_PREVIEW_BUFFER_TOO_SMALL the other outputs describe the
 *     decoded preview, `out_stride` receives the stride the buffer needs
 *     (`dst_stride`, or the row size when that is 0 or smaller) and
 *     `out_pixels_size` the size it needs at that stride (`SIZE_MAX` if no
 *     buffer can have it); the preview is not kept, so a second call
 *     decodes again.
 */
JxlPreviewStatus JxlGeneratePreview(const uint8_t* input, size_t input_size,
                                    JxlPreviewOptions* options);

/** Initialise a @ref JxlPreviewOptions struct to the documented defaults.
 *
 * Equivalent to zero-initialisation today, but keeps callers correct if
 * fields with non-zero defaults are added. This is source compatibility
 * only: the preview API is built into the extras libraries and has no
 * stable ABI.
 */
void JxlPreviewOptionsInit(JxlPreviewOptions* options);

#ifdef __cplusplus
}
#endif

#endif  // LIB_EXTRAS_PREVIEW_H_
