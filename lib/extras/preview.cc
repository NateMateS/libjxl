// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "lib/extras/preview.h"

#include <jxl/codestream_header.h>
#include <jxl/color_encoding.h>
#include <jxl/decode.h>
#include <jxl/decode_cxx.h>
#include <jxl/encode.h>
#include <jxl/memory_manager.h>
#include <jxl/types.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "lib/extras/dec/jxl.h"
#include "lib/extras/packed_image.h"
#include "lib/jxl/cms/color_encoding_cms.h"
#include "lib/jxl/cms/jxl_cms_internal.h"

namespace {

// Default SDR display peak luminance, in nits, used when display_nits == 0.
// Matches the value used by tools/preview_benchmark and benchmark_core.
constexpr float kPreviewSdrAutoNits = 250.0f;

void* PreviewAlloc(JxlMemoryManager* memory_manager, size_t size) {
  if (memory_manager != nullptr && memory_manager->alloc != nullptr) {
    return memory_manager->alloc(memory_manager->opaque, size);
  }
  return std::malloc(size);
}

void PreviewFree(JxlMemoryManager* memory_manager, void* ptr) {
  if (memory_manager != nullptr && memory_manager->free != nullptr) {
    memory_manager->free(memory_manager->opaque, ptr);
  } else {
    std::free(ptr);
  }
}

// libjxl's rule (memory_manager.h): alloc and free are both set or both NULL.
bool ValidMemoryManager(const JxlMemoryManager* memory_manager) {
  return memory_manager == nullptr || ((memory_manager->alloc == nullptr) ==
                                       (memory_manager->free == nullptr));
}

// Too few bytes to tell is not a JPEG XL bitstream either.
bool HasJxlSignature(const uint8_t* input, size_t input_size) {
  const JxlSignature signature = JxlSignatureCheck(input, input_size);
  return signature == JXL_SIG_CODESTREAM || signature == JXL_SIG_CONTAINER;
}

// Forwards to the caller's memory manager and records failed allocations,
// which the decoder reports as decoding errors.
struct AllocationTracker {
  JxlMemoryManager* inner = nullptr;
  std::atomic<bool> failed{false};

  JxlMemoryManager MemoryManager() {
    JxlMemoryManager memory_manager;
    memory_manager.opaque = this;
    memory_manager.alloc = &Alloc;
    memory_manager.free = &Free;
    return memory_manager;
  }

  bool Failed() const { return failed.load(std::memory_order_relaxed); }

 private:
  static void* Alloc(void* opaque, size_t size) {
    auto* tracker = static_cast<AllocationTracker*>(opaque);
    void* address = PreviewAlloc(tracker->inner, size);
    if (address == nullptr) {
      tracker->failed.store(true, std::memory_order_relaxed);
    }
    return address;
  }
  static void Free(void* opaque, void* address) {
    PreviewFree(static_cast<AllocationTracker*>(opaque)->inner, address);
  }
};

bool AllFinite(const double* values, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (!std::isfinite(values[i])) return false;
  }
  return true;
}

// Whether the decoder can output `c`: an RGB or grey color space with a known
// transfer function and valid fields.
bool ValidColorEncoding(const JxlColorEncoding& c) {
  if (c.color_space != JXL_COLOR_SPACE_RGB &&
      c.color_space != JXL_COLOR_SPACE_GRAY) {
    return false;
  }
  if (c.transfer_function == JXL_TRANSFER_FUNCTION_UNKNOWN) return false;
  // The custom values in use must be finite: the range checks below let NaN
  // through.
  if (c.white_point == JXL_WHITE_POINT_CUSTOM &&
      !AllFinite(c.white_point_xy, 2)) {
    return false;
  }
  if (c.color_space == JXL_COLOR_SPACE_RGB &&
      c.primaries == JXL_PRIMARIES_CUSTOM &&
      !(AllFinite(c.primaries_red_xy, 2) &&
        AllFinite(c.primaries_green_xy, 2) &&
        AllFinite(c.primaries_blue_xy, 2))) {
    return false;
  }
  if (c.transfer_function == JXL_TRANSFER_FUNCTION_GAMMA &&
      !std::isfinite(c.gamma)) {
    return false;
  }
  // The checks the decoder applies to an output color profile, without
  // logging the description of invalid enum values.
  jxl::cms::ColorEncoding encoding;
  return static_cast<bool>(encoding.FromExternal(c));
}

bool ValidDisplayNits(float nits) {
  // The range of JxlDecoderSetDesiredIntensityTarget: smaller targets make
  // the tone mapping output NaN.
  return nits == 0.0f || nits == JXL_PREVIEW_NO_TONE_MAPPING ||
         (nits >= 1.0f / (1 << 24) && nits <= 65504.0f);
}

// The bytes per pixel of a supported output format, 0 for any other.
size_t BytesPerPixelOrZero(const JxlPixelFormat& fmt) {
  if (fmt.num_channels < 1 || fmt.num_channels > 4) return 0;
  switch (fmt.endianness) {
    case JXL_NATIVE_ENDIAN:
    case JXL_LITTLE_ENDIAN:
    case JXL_BIG_ENDIAN:
      break;
    default:
      return 0;
  }
  size_t bps = 0;
  switch (fmt.data_type) {
    case JXL_TYPE_UINT8:
      bps = 1;
      break;
    case JXL_TYPE_UINT16:
      bps = 2;
      break;
    case JXL_TYPE_FLOAT16:
      bps = 2;
      break;
    case JXL_TYPE_FLOAT:
      bps = 4;
      break;
    default:
      return 0;
  }
  return bps * fmt.num_channels;
}

JxlPreviewBackend MapBackend(jxl::extras::JXLPreviewBackend backend) {
  switch (backend) {
    case jxl::extras::JXLPreviewBackend::kNone:
      return JXL_PREVIEW_BACKEND_NONE;
    case jxl::extras::JXLPreviewBackend::kEmbeddedPreview:
      return JXL_PREVIEW_BACKEND_EMBEDDED_PREVIEW;
    case jxl::extras::JXLPreviewBackend::kNativeDcOnly:
      return JXL_PREVIEW_BACKEND_NATIVE_DC_ONLY;
    case jxl::extras::JXLPreviewBackend::kNativeProgressionFlush:
      return JXL_PREVIEW_BACKEND_NATIVE_PROGRESSION_FLUSH;
    case jxl::extras::JXLPreviewBackend::kFallbackDownsample:
      return JXL_PREVIEW_BACKEND_FALLBACK_DOWNSAMPLE;
    case jxl::extras::JXLPreviewBackend::kNativeReducedInput:
      return JXL_PREVIEW_BACKEND_NATIVE_REDUCED_INPUT;
    case jxl::extras::JXLPreviewBackend::kNativeFusedUpsampling:
      return JXL_PREVIEW_BACKEND_NATIVE_FUSED_UPSAMPLING;
    case jxl::extras::JXLPreviewBackend::kDecoderDownsample:
      return JXL_PREVIEW_BACKEND_DECODER_DOWNSAMPLE;
  }
  return JXL_PREVIEW_BACKEND_NONE;
}

// Picks the largest factor in {1, 2, 4, 8} whose output is still at least
// target_x x target_y pixels: the smallest output that covers the target. A
// target dimension of 0 acts as "no constraint" on that axis. If both targets
// are zero, or the image is smaller than the target, returns 1.
uint32_t PickFactorForTarget(uint32_t xsize, uint32_t ysize, uint32_t target_x,
                             uint32_t target_y) {
  if (target_x == 0 && target_y == 0) return 1;
  for (uint32_t factor : {8u, 4u, 2u, 1u}) {
    const uint32_t rx = (xsize + factor - 1) / factor;
    const uint32_t ry = (ysize + factor - 1) / factor;
    const bool ok_x = (target_x == 0) || (rx >= target_x);
    const bool ok_y = (target_y == 0) || (ry >= target_y);
    if (ok_x && ok_y) return factor;
  }
  return 1;
}

bool ValidateFactor(uint32_t factor) {
  return factor == 0 || factor == 1 || factor == 2 || factor == 4 ||
         factor == 8;
}

// Maps a DecodeImageJXL failure to a preview status: first the reasons the
// caller's own choices explain, then allocation failures (after which the
// decoder reports an error too), then the input.
JxlPreviewStatus MapInnerFailure(
    const JxlPreviewOptions& options,
    jxl::extras::JXLPreviewFailureReason failure_reason,
    bool allocation_failed) {
  using Reason = jxl::extras::JXLPreviewFailureReason;
  if (failure_reason == Reason::kNoBackendAvailable) {
    return JXL_PREVIEW_NO_BACKEND_AVAILABLE;
  }
  if (options.cancel != nullptr && *options.cancel != 0) {
    return JXL_PREVIEW_CANCELLED;
  }
  if (allocation_failed || failure_reason == Reason::kOutOfMemory) {
    return JXL_PREVIEW_OUT_OF_MEMORY;
  }
  if (failure_reason == Reason::kCorruptInput) {
    return JXL_PREVIEW_CORRUPT_INPUT;
  }
  return JXL_PREVIEW_INTERNAL_ERROR;
}

void ResetOutputs(const JxlPreviewOptions& options) {
  if (options.out_xsize != nullptr) *options.out_xsize = 0;
  if (options.out_ysize != nullptr) *options.out_ysize = 0;
  if (options.out_stride != nullptr) *options.out_stride = 0;
  if (options.out_pixels != nullptr) *options.out_pixels = nullptr;
  if (options.out_pixels_size != nullptr) *options.out_pixels_size = 0;
  if (options.out_format != nullptr) *options.out_format = {};
  if (options.out_backend_used != nullptr) {
    *options.out_backend_used = JXL_PREVIEW_BACKEND_NONE;
  }
  if (options.out_downsampling != nullptr) *options.out_downsampling = 0;
  if (options.out_color_encoding != nullptr) {
    *options.out_color_encoding = {};
    options.out_color_encoding->color_space = JXL_COLOR_SPACE_UNKNOWN;
  }
}

}  // namespace

extern "C" {

void JxlPreviewOptionsInit(JxlPreviewOptions* options) {
  if (options == nullptr) return;
  std::memset(options, 0, sizeof(*options));
}

JxlPreviewStatus JxlGetPreviewInfo(const uint8_t* input, size_t input_size,
                                   const JxlPreviewInfoQuery* query,
                                   JxlPreviewInfo* info) {
  if (info == nullptr) return JXL_PREVIEW_INVALID_ARGUMENT;
  std::memset(info, 0, sizeof(*info));
  const bool want_icc = (query != nullptr) && (query->out_icc != nullptr);
  if (want_icc) *query->out_icc = nullptr;
  if (query != nullptr && query->out_icc_size != nullptr) {
    *query->out_icc_size = 0;
  }

  JxlMemoryManager* mm = (query != nullptr) ? query->memory_manager : nullptr;
  if (input == nullptr || input_size == 0 ||
      (want_icc && query->out_icc_size == nullptr) || !ValidMemoryManager(mm)) {
    return JXL_PREVIEW_INVALID_ARGUMENT;
  }
  if (!HasJxlSignature(input, input_size)) return JXL_PREVIEW_CORRUPT_INPUT;

  AllocationTracker allocations;
  allocations.inner = mm;
  const JxlMemoryManager tracked_mm = allocations.MemoryManager();
  auto dec_owner = JxlDecoderMake(&tracked_mm);
  JxlDecoder* dec = dec_owner.get();
  if (dec == nullptr) return JXL_PREVIEW_OUT_OF_MEMORY;
  const auto decoding_error = [&allocations]() {
    return allocations.Failed() ? JXL_PREVIEW_OUT_OF_MEMORY
                                : JXL_PREVIEW_CORRUPT_INPUT;
  };

  int events = JXL_DEC_BASIC_INFO;
  if (want_icc) events |= JXL_DEC_COLOR_ENCODING;
  if (JxlDecoderSubscribeEvents(dec, events) != JXL_DEC_SUCCESS) {
    return JXL_PREVIEW_INTERNAL_ERROR;
  }
  if (JxlDecoderSetInput(dec, input, input_size) != JXL_DEC_SUCCESS) {
    return JXL_PREVIEW_INTERNAL_ERROR;
  }
  JxlDecoderCloseInput(dec);

  JxlBasicInfo basic;
  bool got_basic = false;
  bool got_color = !want_icc;  // already satisfied if not requested
  for (;;) {
    JxlDecoderStatus status = JxlDecoderProcessInput(dec);
    if (status == JXL_DEC_ERROR) return decoding_error();
    if (status == JXL_DEC_NEED_MORE_INPUT) return JXL_PREVIEW_CORRUPT_INPUT;
    if (status == JXL_DEC_BASIC_INFO) {
      if (JxlDecoderGetBasicInfo(dec, &basic) != JXL_DEC_SUCCESS) {
        return JXL_PREVIEW_CORRUPT_INPUT;
      }
      got_basic = true;
      if (got_color) break;
    } else if (status == JXL_DEC_COLOR_ENCODING) {
      size_t icc_size = 0;
      if (JxlDecoderGetICCProfileSize(dec, JXL_COLOR_PROFILE_TARGET_DATA,
                                      &icc_size) != JXL_DEC_SUCCESS) {
        return JXL_PREVIEW_INTERNAL_ERROR;
      }
      uint8_t* icc_buf = static_cast<uint8_t*>(PreviewAlloc(mm, icc_size));
      if (icc_buf == nullptr) return JXL_PREVIEW_OUT_OF_MEMORY;
      if (JxlDecoderGetColorAsICCProfile(dec, JXL_COLOR_PROFILE_TARGET_DATA,
                                         icc_buf,
                                         icc_size) != JXL_DEC_SUCCESS) {
        PreviewFree(mm, icc_buf);
        return decoding_error();
      }
      *query->out_icc = icc_buf;
      *query->out_icc_size = icc_size;
      got_color = true;
      if (got_basic) break;
    } else if (status == JXL_DEC_SUCCESS) {
      break;
    } else {
      // Unexpected event — should not happen given our subscription mask.
      return JXL_PREVIEW_INTERNAL_ERROR;
    }
  }
  if (!got_basic) return JXL_PREVIEW_CORRUPT_INPUT;

  info->xsize = basic.xsize;
  info->ysize = basic.ysize;
  info->num_color_channels = basic.num_color_channels;
  info->has_alpha = basic.alpha_bits != 0 ? 1 : 0;
  info->is_animated = basic.have_animation ? 1 : 0;
  info->intensity_target = basic.intensity_target;

  uint32_t target_x = (query != nullptr) ? query->target_xsize : 0;
  uint32_t target_y = (query != nullptr) ? query->target_ysize : 0;
  uint32_t factor =
      PickFactorForTarget(basic.xsize, basic.ysize, target_x, target_y);
  info->recommended_factor = factor;
  info->recommended_xsize = (basic.xsize + factor - 1) / factor;
  info->recommended_ysize = (basic.ysize + factor - 1) / factor;
  return JXL_PREVIEW_SUCCESS;
}

JxlPreviewStatus JxlGeneratePreview(const uint8_t* input, size_t input_size,
                                    JxlPreviewOptions* options) {
  if (options == nullptr) return JXL_PREVIEW_INVALID_ARGUMENT;
  ResetOutputs(*options);
  if (input == nullptr || input_size == 0 || options->out_xsize == nullptr ||
      options->out_ysize == nullptr || options->out_pixels == nullptr) {
    return JXL_PREVIEW_INVALID_ARGUMENT;
  }
  if (!ValidateFactor(options->preview_downsampling) ||
      !ValidMemoryManager(options->memory_manager) ||
      !ValidDisplayNits(options->display_nits)) {
    return JXL_PREVIEW_INVALID_ARGUMENT;
  }
  const bool tone_mapping =
      options->display_nits != JXL_PREVIEW_NO_TONE_MAPPING;
  std::string color_space;
  if (options->color_encoding != nullptr) {
    if (!ValidColorEncoding(*options->color_encoding)) {
      return JXL_PREVIEW_INVALID_ARGUMENT;
    }
    color_space = jxl::ColorEncodingDescription(*options->color_encoding);
  }

  // Resolve output pixel format with default RGBA8.
  JxlPixelFormat format = options->format;
  if (format.num_channels == 0) {
    format.num_channels = 4;
    format.data_type = JXL_TYPE_UINT8;
    format.endianness = JXL_NATIVE_ENDIAN;
    format.align = 0;
  }
  const size_t bpp = BytesPerPixelOrZero(format);
  if (bpp == 0) return JXL_PREVIEW_UNSUPPORTED_FORMAT;

  if (!HasJxlSignature(input, input_size)) return JXL_PREVIEW_CORRUPT_INPUT;

  // Resolve preview factor. If 0, derive from target dims (requires a header
  // probe). If both are 0, default to 1 (full-resolution decode).
  uint32_t factor = options->preview_downsampling;
  const bool factor_from_target =
      factor == 0 && (options->target_xsize != 0 || options->target_ysize != 0);
  if (factor == 0 && !factor_from_target) factor = 1;
  // A NULL color_encoding means sRGB, grey or RGB as the source is, unless
  // tone mapping is off (see JXL_PREVIEW_NO_TONE_MAPPING).
  const bool default_srgb = options->color_encoding == nullptr && tone_mapping;
  JxlPreviewInfo probe = {};
  if (factor_from_target || default_srgb) {
    JxlPreviewInfoQuery q = {};
    q.target_xsize = options->target_xsize;
    q.target_ysize = options->target_ysize;
    q.memory_manager = options->memory_manager;
    JxlPreviewStatus ps = JxlGetPreviewInfo(input, input_size, &q, &probe);
    if (ps != JXL_PREVIEW_SUCCESS) return ps;
    if (factor_from_target) factor = probe.recommended_factor;
  }

  // Build inner-decoder params.
  jxl::extras::JXLDecompressParams dparams;
  // Push 1/2/3/4-channel variants of the requested data_type so the inner
  // SelectFormat can pick whichever matches the source's color/alpha layout.
  // The caller's requested num_channels is treated as a preference, not a
  // hard constraint; out_format reports what was actually produced. Rows are
  // decoded tightly packed and laid out in the output by dst_stride.
  for (uint32_t nc : {1u, 2u, 3u, 4u}) {
    JxlPixelFormat fmt = format;
    fmt.num_channels = nc;
    fmt.align = 0;
    dparams.accepted_formats.push_back(fmt);
  }
  dparams.preview_downsampling = factor;
  // Also at factor 1: the API returns the first frame.
  dparams.first_frame_only = true;
  dparams.runner = options->runner;
  dparams.runner_opaque = options->runner_opaque;
  AllocationTracker allocations;
  allocations.inner = options->memory_manager;
  JxlMemoryManager tracked_mm = allocations.MemoryManager();
  dparams.memory_manager = &tracked_mm;
  dparams.cancel = options->cancel;

  if (!tone_mapping) {
    dparams.display_nits = 0.0f;
  } else if (options->display_nits == 0.0f) {
    dparams.display_nits = kPreviewSdrAutoNits;
  } else {
    dparams.display_nits = options->display_nits;
  }

  // Output color encoding -> color_space description string.
  JxlColorEncoding srgb;
  if (options->color_encoding != nullptr) {
    dparams.color_space = std::move(color_space);
  } else if (default_srgb) {
    JxlColorEncodingSetToSRGB(&srgb,
                              TO_JXL_BOOL(probe.num_color_channels == 1));
    dparams.color_space = jxl::ColorEncodingDescription(srgb);
  } else {
    // The decoder's default output encoding, except for CMYK: the output has
    // no channel for black.
    JxlColorEncodingSetToSRGB(&srgb, JXL_FALSE);
    dparams.color_space_for_cmyk = jxl::ColorEncodingDescription(srgb);
  }

  jxl::extras::JXLPreviewBackend backend_used =
      jxl::extras::JXLPreviewBackend::kNone;
  jxl::extras::JXLPreviewFailureReason failure_reason =
      jxl::extras::JXLPreviewFailureReason::kNone;
  dparams.preview_backend = &backend_used;
  dparams.preview_allowed_backends = options->allowed_backends;
  dparams.preview_hooks = options->preview_hooks;
  dparams.preview_failure_reason = &failure_reason;

  jxl::extras::PackedPixelFile ppf;
  size_t decoded_bytes = 0;
  if (!jxl::extras::DecodeImageJXL(input, input_size, dparams, &decoded_bytes,
                                   &ppf)) {
    return MapInnerFailure(*options, failure_reason, allocations.Failed());
  }
  if (ppf.frames.empty()) return JXL_PREVIEW_INTERNAL_ERROR;

  const jxl::extras::PackedImage& image = ppf.frames.front().color;
  if (image.xsize == 0 || image.ysize == 0 || image.pixels() == nullptr ||
      image.pixels_size == 0) {
    return JXL_PREVIEW_INTERNAL_ERROR;
  }

  // The actual format SelectFormat picked may have a different channel
  // count than the caller requested (e.g. RGB source vs RGBA request).
  const JxlPixelFormat actual_format = image.format;
  const size_t actual_bpp = BytesPerPixelOrZero(actual_format);
  if (actual_bpp == 0) return JXL_PREVIEW_INTERNAL_ERROR;

  const uint32_t out_xsize = static_cast<uint32_t>(image.xsize);
  const uint32_t out_ysize = static_cast<uint32_t>(image.ysize);
  const size_t row_bytes = static_cast<size_t>(out_xsize) * actual_bpp;

  // Everything but the pixels is known now, and also reported when the
  // caller's buffer is too small.
  *options->out_xsize = out_xsize;
  *options->out_ysize = out_ysize;
  if (options->out_format != nullptr) *options->out_format = actual_format;
  if (options->out_backend_used != nullptr) {
    // DecodeImageJXL has applied the allowed-backends policy.
    *options->out_backend_used = MapBackend(backend_used);
  }
  if (options->out_downsampling != nullptr) *options->out_downsampling = factor;
  if (options->out_color_encoding != nullptr) {
    *options->out_color_encoding = ppf.color_encoding;
  }

  uint8_t* dst = options->dst;
  size_t dst_stride = options->dst_stride;
  size_t produced_size = 0;

  if (dst != nullptr) {
    // A stride below the row size (which depends on the source's channels)
    // is a buffer too small, as is a size below the last row's end:
    // (out_ysize - 1) * dst_stride + row_bytes, compared without overflow.
    const bool stride_fits = dst_stride >= row_bytes || dst_stride == 0;
    if (dst_stride < row_bytes) dst_stride = row_bytes;
    const bool fits =
        stride_fits && options->dst_size >= row_bytes &&
        (out_ysize <= 1 ||
         (options->dst_size - row_bytes) / (out_ysize - 1) >= dst_stride);
    if (!fits) {
      if (options->out_stride != nullptr) *options->out_stride = dst_stride;
      if (options->out_pixels_size != nullptr) {
        const size_t max = std::numeric_limits<size_t>::max();
        *options->out_pixels_size =
            (out_ysize > 1 && dst_stride > (max - row_bytes) / (out_ysize - 1))
                ? max
                : (out_ysize - 1) * dst_stride + row_bytes;
      }
      return JXL_PREVIEW_BUFFER_TOO_SMALL;
    }
    produced_size = static_cast<size_t>(out_ysize - 1) * dst_stride + row_bytes;
  } else {
    // Allocate tightly-packed buffer via memory manager.
    const size_t needed = static_cast<size_t>(out_ysize) * row_bytes;
    dst = static_cast<uint8_t*>(PreviewAlloc(options->memory_manager, needed));
    if (dst == nullptr) {
      ResetOutputs(*options);
      return JXL_PREVIEW_OUT_OF_MEMORY;
    }
    dst_stride = row_bytes;
    produced_size = needed;
  }
  // Copy row by row: the decoded rows are image.stride bytes apart.
  const uint8_t* src = static_cast<const uint8_t*>(image.pixels());
  for (uint32_t y = 0; y < out_ysize; ++y) {
    std::memcpy(dst + static_cast<size_t>(y) * dst_stride,
                src + static_cast<size_t>(y) * image.stride, row_bytes);
  }

  *options->out_pixels = dst;
  if (options->out_pixels_size != nullptr) {
    *options->out_pixels_size = produced_size;
  }
  if (options->out_stride != nullptr) *options->out_stride = dst_stride;
  return JXL_PREVIEW_SUCCESS;
}

}  // extern "C"
