// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "lib/extras/dec/jxl.h"

#include <jxl/cms.h>
#include <jxl/codestream_header.h>
#include <jxl/color_encoding.h>
#include <jxl/decode.h>
#include <jxl/decode_cxx.h>
#include <jxl/types.h>

#include <cinttypes>  // PRIu32
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "lib/extras/common.h"
#include "lib/extras/dec/color_description.h"
#include "lib/extras/exif.h"
#include "lib/extras/packed_image.h"
#include "lib/extras/size_constraints.h"
#include "lib/jxl/base/byte_order.h"
#include "lib/jxl/base/common.h"
#include "lib/jxl/base/exif.h"
#include "lib/jxl/base/printf_macros.h"
#include "lib/jxl/base/status.h"
#include "lib/jxl/dec_preview_internal.h"

namespace jxl {
namespace extras {
namespace {

// Set JXL_PREVIEW_DEBUG=1 to log the header of each preview frame and the
// preview backend that was used. Building libjxl with -DJXL_DEBUG_PREVIEW=1
// additionally logs why the decoder chose its render method.
bool PreviewDebugEnabled() {
  static const bool enabled = []() {
    const char* v = std::getenv("JXL_PREVIEW_DEBUG");
    return v != nullptr && v[0] != '\0' && v[0] != '0';
  }();
  return enabled;
}

void LogPreviewFrame(const JxlBasicInfo& info, JxlFrameEncoding encoding,
                     const JxlFrameHeader& fh, size_t preview_downsampling) {
  if (!PreviewDebugEnabled()) return;
  fprintf(stderr,
          "[preview-debug] frame: enc=%s ds=%" PRIuS
          " dur=%u is_last=%d "
          "crop=%dx%d@(%d,%d) alpha_bits=%u extra_ch=%u xyb=%d\n",
          encoding == JXL_FRAME_ENCODING_VAR_DCT   ? "VarDCT"
          : encoding == JXL_FRAME_ENCODING_MODULAR ? "Modular"
                                                   : "Unknown",
          preview_downsampling, fh.duration, fh.is_last,
          static_cast<int>(fh.layer_info.xsize),
          static_cast<int>(fh.layer_info.ysize), fh.layer_info.crop_x0,
          fh.layer_info.crop_y0, info.alpha_bits, info.num_extra_channels,
          info.uses_original_profile ? 0 : 1);
}

struct BoxProcessor {
  explicit BoxProcessor(JxlDecoder* dec) : dec_(dec) { Reset(); }

  bool InitializeOutput(std::vector<uint8_t>* out) {
    if (out == nullptr) {
      fprintf(stderr, "internal: out == nullptr\n");
      return false;
    }
    box_data_ = out;
    return AddMoreOutput();
  }

  bool AddMoreOutput() {
    if (box_data_ == nullptr) {
      fprintf(stderr, "internal: box_data_ == nullptr\n");
      return false;
    }
    Flush();
    static const size_t kBoxOutputChunkSize = 1 << 16;
    box_data_->resize(box_data_->size() + kBoxOutputChunkSize);
    next_out_ = box_data_->data() + total_size_;
    avail_out_ = box_data_->size() - total_size_;
    if (JXL_DEC_SUCCESS !=
        JxlDecoderSetBoxBuffer(dec_, next_out_, avail_out_)) {
      fprintf(stderr, "JxlDecoderSetBoxBuffer failed\n");
      return false;
    }
    return true;
  }

  void FinalizeOutput() {
    if (box_data_ == nullptr) return;
    Flush();
    box_data_->resize(total_size_);
    Reset();
  }

 private:
  JxlDecoder* dec_;
  std::vector<uint8_t>* box_data_;
  uint8_t* next_out_;
  size_t avail_out_;
  size_t total_size_;

  void Reset() {
    box_data_ = nullptr;
    next_out_ = nullptr;
    avail_out_ = 0;
    total_size_ = 0;
  }
  void Flush() {
    if (box_data_ == nullptr) return;
    size_t remaining = JxlDecoderReleaseBoxBuffer(dec_);
    size_t bytes_written = avail_out_ - remaining;
    next_out_ += bytes_written;
    avail_out_ -= bytes_written;
    total_size_ += bytes_written;
  }
};

void SetBitDepthFromDataType(JxlDataType data_type, uint32_t* bits_per_sample,
                             uint32_t* exponent_bits_per_sample) {
  switch (data_type) {
    case JXL_TYPE_UINT8:
      *bits_per_sample = 8;
      *exponent_bits_per_sample = 0;
      break;
    case JXL_TYPE_UINT16:
      *bits_per_sample = 16;
      *exponent_bits_per_sample = 0;
      break;
    case JXL_TYPE_FLOAT16:
      *bits_per_sample = 16;
      *exponent_bits_per_sample = 5;
      break;
    case JXL_TYPE_FLOAT:
      *bits_per_sample = 32;
      *exponent_bits_per_sample = 8;
      break;
  }
}

template <typename T>
void UpdateBitDepth(JxlBitDepth bit_depth, JxlDataType data_type, T* info) {
  if (bit_depth.type == JXL_BIT_DEPTH_FROM_PIXEL_FORMAT) {
    SetBitDepthFromDataType(data_type, &info->bits_per_sample,
                            &info->exponent_bits_per_sample);
  } else if (bit_depth.type == JXL_BIT_DEPTH_CUSTOM) {
    info->bits_per_sample = bit_depth.bits_per_sample;
    info->exponent_bits_per_sample = bit_depth.exponent_bits_per_sample;
  }
}

// Applies the requested bit depth to the image output just set (a frame's or
// the embedded preview's), and records it in `info`.
bool SetOutputBitDepth(JxlDecoder* dec, const JXLDecompressParams& dparams,
                       const JxlPixelFormat& format, JxlBasicInfo* info) {
  if (JXL_DEC_SUCCESS !=
      JxlDecoderSetImageOutBitDepth(dec, &dparams.output_bitdepth)) {
    fprintf(stderr, "JxlDecoderSetImageOutBitDepth failed\n");
    return false;
  }
  UpdateBitDepth(dparams.output_bitdepth, format.data_type, info);
  if (format.num_channels == 2 || format.num_channels == 4) {
    // Interleaved alpha channels has the same bit depth as color channels.
    info->alpha_bits = info->bits_per_sample;
    info->alpha_exponent_bits = info->exponent_bits_per_sample;
  }
  return true;
}

// Division rounding toward negative infinity, for crops (which can be
// negative) at the preview scale.
int32_t FloorDiv(int32_t a, int32_t b) {
  return a / b - ((a % b != 0 && a < 0) ? 1 : 0);
}

Status ValidatePreviewDownsampling(size_t factor) {
  if (factor == 0) {
    return JXL_FAILURE("preview_downsampling must be >= 1");
  }
  if (factor == 1 || factor == 2 || factor == 4 || factor == 8) {
    return true;
  }
  return JXL_FAILURE("preview_downsampling must be one of 1, 2, 4 or 8");
}

constexpr uint32_t PreviewBackendBit(JXLPreviewBackend backend) {
  return 1u << static_cast<uint32_t>(backend);
}

// The decoder's render methods, the set kDecoderDownsample stands for.
constexpr uint32_t kDecoderMethodBits =
    PreviewBackendBit(JXLPreviewBackend::kNativeDcOnly) |
    PreviewBackendBit(JXLPreviewBackend::kFallbackDownsample) |
    PreviewBackendBit(JXLPreviewBackend::kNativeReducedInput) |
    PreviewBackendBit(JXLPreviewBackend::kNativeFusedUpsampling);

// `preview_allowed_backends` as a plain bit set: 0 allows every backend, and
// kDecoderDownsample is equivalent to the set of methods it stands for, in
// both directions.
uint32_t NormalizeAllowedBackends(uint32_t mask) {
  if (mask == 0) return ~0u;
  const uint32_t decoder_bit =
      PreviewBackendBit(JXLPreviewBackend::kDecoderDownsample);
  if ((mask & decoder_bit) != 0) mask |= kDecoderMethodBits;
  if ((mask & kDecoderMethodBits) == kDecoderMethodBits) mask |= decoder_bit;
  return mask;
}

bool PreviewBackendAllowed(uint32_t allowed_backends,
                           JXLPreviewBackend backend) {
  return (allowed_backends & PreviewBackendBit(backend)) != 0;
}

bool FailWithReason(const JXLDecompressParams& dparams,
                    JXLPreviewFailureReason reason) {
  if (dparams.preview_failure_reason != nullptr) {
    *dparams.preview_failure_reason = reason;
  }
  return false;
}

JXLPreviewBackend PreviewBackendFromMethod(
    JxlImageOutDownsamplingMethod method) {
  switch (method) {
    case JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_NONE:
      return JXLPreviewBackend::kNone;
    case JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_FULL_RESOLUTION:
      return JXLPreviewBackend::kFallbackDownsample;
    case JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_REDUCED_INPUT:
      return JXLPreviewBackend::kNativeReducedInput;
    case JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_FUSED_UPSAMPLING:
      return JXLPreviewBackend::kNativeFusedUpsampling;
    case JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_DC_ONLY:
      return JXLPreviewBackend::kNativeDcOnly;
  }
  return JXLPreviewBackend::kNone;
}

// The decoder's render method of the current frame at the preview scale, or
// kNone if the frame is not (yet) rendered at a reduced scale.
JXLPreviewBackend DecoderPreviewBackend(const JxlDecoder* dec) {
  JxlImageOutDownsamplingMethod method = JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_NONE;
  if (JXL_DEC_SUCCESS !=
      JxlDecoderGetImageOutDownsamplingMethod(dec, &method)) {
    return JXLPreviewBackend::kNone;
  }
  return PreviewBackendFromMethod(method);
}

// Whether the decoder's render method of the current frame is allowed.
bool CurrentMethodAllowed(uint32_t allowed_backends, const JxlDecoder* dec) {
  const JXLPreviewBackend backend = DecoderPreviewBackend(dec);
  return backend == JXLPreviewBackend::kNone ||
         PreviewBackendAllowed(allowed_backends, backend);
}

}  // namespace

bool DecodeImageJXL(const uint8_t* bytes, size_t bytes_size,
                    const JXLDecompressParams& dparams, size_t* decoded_bytes,
                    PackedPixelFile* ppf, std::vector<uint8_t>* jpeg_bytes,
                    const SizeConstraints* constraints) {
  if (!ValidatePreviewDownsampling(dparams.preview_downsampling)) {
    fprintf(stderr, "Invalid preview_downsampling value\n");
    return false;
  }
  if (jpeg_bytes != nullptr && dparams.preview_downsampling != 1) {
    fprintf(stderr,
            "preview_downsampling is not supported together with JPEG "
            "reconstruction\n");
    return false;
  }
  if (dparams.preview_backend != nullptr) {
    *dparams.preview_backend = JXLPreviewBackend::kNone;
  }
  if (dparams.preview_failure_reason != nullptr) {
    *dparams.preview_failure_reason = JXLPreviewFailureReason::kNone;
  }
  const bool preview_requested = dparams.preview_downsampling > 1;
  const uint32_t allowed_backends =
      NormalizeAllowedBackends(dparams.preview_allowed_backends);
  // A full decode is the kNone backend; a preview needs one of the others.
  const uint32_t output_backends =
      preview_requested ? ~PreviewBackendBit(JXLPreviewBackend::kNone)
                        : PreviewBackendBit(JXLPreviewBackend::kNone);
  if ((allowed_backends & output_backends) == 0) {
    fprintf(stderr, "No allowed preview backend can produce this output\n");
    return FailWithReason(dparams,
                          JXLPreviewFailureReason::kNoBackendAvailable);
  }
  JxlSignature sig = JxlSignatureCheck(bytes, bytes_size);
  // silently return false if this is not a JXL file
  if (sig == JXL_SIG_INVALID) {
    return FailWithReason(dparams, JXLPreviewFailureReason::kCorruptInput);
  }

  auto decoder = JxlDecoderMake(dparams.memory_manager);
  JxlDecoder* dec = decoder.get();
  if (dec == nullptr) {
    fprintf(stderr, "JxlDecoderMake failed\n");
    return FailWithReason(dparams, JXLPreviewFailureReason::kOutOfMemory);
  }
  // Whether pixel decoding stops after the first displayed frame.
  const bool first_frame_only = preview_requested || dparams.first_frame_only;
  ppf->frames.clear();

  if (dparams.runner_opaque != nullptr &&
      JXL_DEC_SUCCESS != JxlDecoderSetParallelRunner(dec, dparams.runner,
                                                     dparams.runner_opaque)) {
    fprintf(stderr, "JxlEncoderSetParallelRunner failed\n");
    return false;
  }

  JxlPixelFormat format = {};  // Initialize to calm down clang-tidy.
  std::vector<JxlPixelFormat> accepted_formats = dparams.accepted_formats;

  JxlColorEncoding color_encoding;
  size_t num_color_channels = 0;
  bool set_colorspace = false;
  if (!dparams.color_space.empty()) {
    if (!jxl::ParseDescription(dparams.color_space, &color_encoding)) {
      fprintf(stderr, "Failed to parse color space %s.\n",
              dparams.color_space.c_str());
      return false;
    }
    set_colorspace = true;
    num_color_channels =
        color_encoding.color_space == JXL_COLOR_SPACE_GRAY ? 1 : 3;
  }

  bool can_reconstruct_jpeg = false;
  const size_t effective_max_downsampling = dparams.max_downsampling > 1
                                                ? dparams.max_downsampling
                                                : dparams.preview_downsampling;
  bool use_embedded_preview = false;
  bool returned_embedded_preview = false;
  JXLPreviewBackend preview_backend_used = JXLPreviewBackend::kNone;
  std::vector<uint8_t> jpeg_data_chunk;
  if (jpeg_bytes != nullptr) {
    // This bound is very likely to be enough to hold the entire
    // reconstructed JPEG, to avoid having to do expensive retries.
    jpeg_data_chunk.resize(bytes_size * 3 / 2 + 1024);
    jpeg_bytes->resize(0);
  }

  int events = (JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE);

  bool max_passes_defined =
      (dparams.max_passes < std::numeric_limits<uint32_t>::max());
  if (max_passes_defined || effective_max_downsampling > 1) {
    events |= JXL_DEC_FRAME_PROGRESSION;
    if (max_passes_defined) {
      JxlDecoderSetProgressiveDetail(dec, JxlProgressiveDetail::kPasses);
    } else {
      JxlDecoderSetProgressiveDetail(dec, JxlProgressiveDetail::kLastPasses);
    }
  }
  if (jpeg_bytes != nullptr) {
    events |= JXL_DEC_JPEG_RECONSTRUCTION;
  } else {
    events |= (JXL_DEC_COLOR_ENCODING | JXL_DEC_FRAME | JXL_DEC_PREVIEW_IMAGE |
               JXL_DEC_BOX);
    if (accepted_formats.empty()) {
      // decoding just the metadata, not the pixel data
      events ^= (JXL_DEC_FULL_IMAGE | JXL_DEC_PREVIEW_IMAGE);
    }
  }
  if (JXL_DEC_SUCCESS != JxlDecoderSubscribeEvents(dec, events)) {
    fprintf(stderr, "JxlDecoderSubscribeEvents failed\n");
    return false;
  }
  if (jpeg_bytes == nullptr) {
    if (JXL_DEC_SUCCESS != JxlDecoderSetRenderSpotcolors(
                               dec, TO_JXL_BOOL(dparams.render_spotcolors))) {
      fprintf(stderr, "JxlDecoderSetRenderSpotColors failed\n");
      return false;
    }
    if (JXL_DEC_SUCCESS != JxlDecoderSetKeepOrientation(
                               dec, TO_JXL_BOOL(dparams.keep_orientation))) {
      fprintf(stderr, "JxlDecoderSetKeepOrientation failed\n");
      return false;
    }
    if (JXL_DEC_SUCCESS != JxlDecoderSetUnpremultiplyAlpha(
                               dec, TO_JXL_BOOL(dparams.unpremultiply_alpha))) {
      fprintf(stderr, "JxlDecoderSetUnpremultiplyAlpha failed\n");
      return false;
    }
    if (JXL_DEC_SUCCESS !=
        JxlDecoderSetCoalescing(dec, TO_JXL_BOOL(dparams.coalescing))) {
      fprintf(stderr, "JxlDecoderSetCoalescing failed\n");
      return false;
    }
    if (dparams.display_nits > 0 &&
        JXL_DEC_SUCCESS !=
            JxlDecoderSetDesiredIntensityTarget(dec, dparams.display_nits)) {
      fprintf(stderr, "Decoder failed to set desired intensity target\n");
      return false;
    }
    if (JXL_DEC_SUCCESS != JxlDecoderSetDecompressBoxes(dec, JXL_TRUE)) {
      fprintf(stderr, "JxlDecoderSetDecompressBoxes failed\n");
      return false;
    }
  }
  // The whole input is supplied at once, also for previews: the decoder pauses
  // at each progression step regardless, and smaller pieces of input only split
  // group decoding into smaller parallel batches.
  if (JXL_DEC_SUCCESS != JxlDecoderSetInput(dec, bytes, bytes_size)) {
    fprintf(stderr, "Decoder failed to set input\n");
    return false;
  }
  uint32_t progression_index = 0;
  // Whether a frame progression step of the current frame already has the
  // requested detail (see the JXL_DEC_FRAME_PROGRESSION handler).
  bool progression_target_reached = false;
  bool codestream_done = jpeg_bytes == nullptr && accepted_formats.empty();
  bool image_output_set = false;
  // Whether decoding stops after the current frame (see JXL_DEC_FRAME).
  bool stop_after_frame = false;
  BoxProcessor boxes(dec);
  uint64_t total_pixel_count = 0;
  for (;;) {
    if (dparams.cancel != nullptr && *dparams.cancel != 0) {
      return false;
    }
    JxlDecoderStatus status = JxlDecoderProcessInput(dec);
    if (status == JXL_DEC_ERROR) {
      fprintf(stderr, "Failed to decode image\n");
      return FailWithReason(dparams, JXLPreviewFailureReason::kCorruptInput);
    } else if (status == JXL_DEC_NEED_MORE_INPUT) {
      if (codestream_done) {
        break;
      }
      if (dparams.allow_partial_input) {
        if (JXL_DEC_SUCCESS != JxlDecoderFlushImage(dec)) {
          fprintf(stderr,
                  "Input file is truncated and there is no preview "
                  "available yet.\n");
          return FailWithReason(dparams,
                                JXLPreviewFailureReason::kCorruptInput);
        }
        break;
      }
      size_t released_size = JxlDecoderReleaseInput(dec);
      fprintf(stderr,
              "Input file is truncated (total bytes: %" PRIuS
              ", processed bytes: %" PRIuS
              ") and --allow_partial_files is not present.\n",
              bytes_size, bytes_size - released_size);
      return FailWithReason(dparams, JXLPreviewFailureReason::kCorruptInput);
    } else if (status == JXL_DEC_BOX) {
      boxes.FinalizeOutput();
      JxlBoxType box_type;
      if (JXL_DEC_SUCCESS != JxlDecoderGetBoxType(dec, box_type, JXL_TRUE)) {
        fprintf(stderr, "JxlDecoderGetBoxType failed\n");
        return false;
      }
      std::vector<uint8_t>* box_data = nullptr;
      if (memcmp(box_type, "Exif", 4) == 0) {
        box_data = &ppf->metadata.exif;
      } else if (memcmp(box_type, "iptc", 4) == 0) {
        box_data = &ppf->metadata.iptc;
      } else if (memcmp(box_type, "jumb", 4) == 0) {
        box_data = &ppf->metadata.jumbf;
      } else if (memcmp(box_type, "jhgm", 4) == 0) {
        box_data = &ppf->metadata.jhgm;
      } else if (memcmp(box_type, "xml ", 4) == 0) {
        box_data = &ppf->metadata.xmp;
      }
      if (box_data) {
        if (!boxes.InitializeOutput(box_data)) {
          return false;
        }
      }
    } else if (status == JXL_DEC_BOX_NEED_MORE_OUTPUT) {
      if (!boxes.AddMoreOutput()) {
        return false;
      }
    } else if (status == JXL_DEC_JPEG_RECONSTRUCTION) {
      can_reconstruct_jpeg = true;
      // Decoding to JPEG.
      if (JXL_DEC_SUCCESS != JxlDecoderSetJPEGBuffer(dec,
                                                     jpeg_data_chunk.data(),
                                                     jpeg_data_chunk.size())) {
        fprintf(stderr, "Decoder failed to set JPEG Buffer\n");
        return false;
      }
    } else if (status == JXL_DEC_JPEG_NEED_MORE_OUTPUT) {
      if (jpeg_bytes == nullptr) {
        fprintf(stderr, "internal: jpeg_bytes == nullptr\n");
        return false;
      }
      // Decoded a chunk to JPEG.
      size_t used_jpeg_output =
          jpeg_data_chunk.size() - JxlDecoderReleaseJPEGBuffer(dec);
      jpeg_bytes->insert(jpeg_bytes->end(), jpeg_data_chunk.data(),
                         jpeg_data_chunk.data() + used_jpeg_output);
      if (used_jpeg_output == 0) {
        // Chunk is too small.
        jpeg_data_chunk.resize(jpeg_data_chunk.size() * 2);
      }
      if (JXL_DEC_SUCCESS != JxlDecoderSetJPEGBuffer(dec,
                                                     jpeg_data_chunk.data(),
                                                     jpeg_data_chunk.size())) {
        fprintf(stderr, "Decoder failed to set JPEG Buffer\n");
        return false;
      }
    } else if (status == JXL_DEC_BASIC_INFO) {
      if (JXL_DEC_SUCCESS != JxlDecoderGetBasicInfo(dec, &ppf->info)) {
        fprintf(stderr, "JxlDecoderGetBasicInfo failed\n");
        return false;
      }
      if (!VerifyDimensions(constraints, ppf->xsize(), ppf->ysize())) {
        fprintf(stderr, "Image too big\n");
        return false;
      }
      if (accepted_formats.empty()) continue;
      if (num_color_channels != 0) {
        // Mark the change in number of color channels due to the requested
        // color space.
        ppf->info.num_color_channels = num_color_channels;
      }
      if (dparams.output_bitdepth.type == JXL_BIT_DEPTH_CUSTOM) {
        // Select format based on custom bits per sample.
        ppf->info.bits_per_sample = dparams.output_bitdepth.bits_per_sample;
      }
      // Select format according to accepted formats.
      if (!jxl::extras::SelectFormat(accepted_formats, ppf->info, &format)) {
        fprintf(stderr, "SelectFormat failed\n");
        return false;
      }
      bool have_alpha = (format.num_channels == 2 || format.num_channels == 4);
      size_t format_color_channels = have_alpha ? format.num_channels - 1
                                                : format.num_channels;
      if (format_color_channels > ppf->info.num_color_channels) {
        ppf->info.num_color_channels = format_color_channels;
      }
      if (!have_alpha) {
        // Mark in the basic info that alpha channel was dropped.
        ppf->info.alpha_bits = 0;
      } else {
        if (dparams.unpremultiply_alpha) {
          // Mark in the basic info that alpha was unpremultiplied.
          ppf->info.alpha_premultiplied = JXL_FALSE;
        }
      }
      bool alpha_found = false;
      for (uint32_t i = 0; i < ppf->info.num_extra_channels; ++i) {
        JxlExtraChannelInfo eci;
        if (JXL_DEC_SUCCESS != JxlDecoderGetExtraChannelInfo(dec, i, &eci)) {
          fprintf(stderr, "JxlDecoderGetExtraChannelInfo failed\n");
          return false;
        }
        if (eci.type == JXL_CHANNEL_ALPHA && have_alpha && !alpha_found) {
          // Skip the first alpha channels because it is already present in the
          // interleaved image.
          alpha_found = true;
          continue;
        }
        if (eci.type == JXL_CHANNEL_BLACK && !set_colorspace &&
            !dparams.color_space_for_cmyk.empty()) {
          if (!jxl::ParseDescription(dparams.color_space_for_cmyk,
                                     &color_encoding)) {
            fprintf(stderr, "Failed to parse color space %s.\n",
                    dparams.color_space_for_cmyk.c_str());
            return false;
          }
          set_colorspace = true;
        }
        std::string name(eci.name_length + 1, 0);
        if (JXL_DEC_SUCCESS !=
            JxlDecoderGetExtraChannelName(
                dec, i, const_cast<char*>(name.data()), name.size())) {
          fprintf(stderr, "JxlDecoderGetExtraChannelName failed\n");
          return false;
        }
        name.resize(eci.name_length);
        ppf->extra_channels_info.push_back({eci, i, name});
      }
      // The embedded preview is returned only when it is exactly the requested
      // preview (its size is oriented like the image's) and the output has no
      // extra channel images, which the decoder does not output for it.
      if (preview_requested && ppf->info.have_preview &&
          ppf->extra_channels_info.empty() &&
          PreviewBackendAllowed(allowed_backends,
                                JXLPreviewBackend::kEmbeddedPreview)) {
        use_embedded_preview =
            ppf->info.preview.xsize ==
                DivCeil(ppf->info.xsize, dparams.preview_downsampling) &&
            ppf->info.preview.ysize ==
                DivCeil(ppf->info.ysize, dparams.preview_downsampling);
      }
    } else if (status == JXL_DEC_COLOR_ENCODING) {
      if (set_colorspace) {
        JxlDecoderSetCms(dec, *JxlGetDefaultCms());
        if (JXL_DEC_SUCCESS !=
            JxlDecoderSetOutputColorProfile(dec, &color_encoding, nullptr, 0)) {
          fprintf(stderr, "Failed to set color space.\n");
          return false;
        }
        ppf->color_encoding = color_encoding;
      }
      size_t icc_size = 0;
      JxlColorProfileTarget target = JXL_COLOR_PROFILE_TARGET_DATA;
      if (JXL_DEC_SUCCESS !=
          JxlDecoderGetICCProfileSize(dec, target, &icc_size)) {
        fprintf(stderr, "JxlDecoderGetICCProfileSize failed\n");
      }
      if (icc_size != 0) {
        ppf->icc.resize(icc_size);
        if (JXL_DEC_SUCCESS != JxlDecoderGetColorAsICCProfile(
                                   dec, target, ppf->icc.data(), icc_size)) {
          fprintf(stderr, "JxlDecoderGetColorAsICCProfile failed\n");
          return false;
        }
      }
      ppf->primary_color_representation =
          PackedPixelFile::kColorEncodingIsPrimary;
      if (JXL_DEC_SUCCESS != JxlDecoderGetColorAsEncodedProfile(
                                 dec, target, &ppf->color_encoding)) {
        ppf->color_encoding.color_space = JXL_COLOR_SPACE_UNKNOWN;
        ppf->primary_color_representation = PackedPixelFile::kIccIsPrimary;
      }

      icc_size = 0;
      target = JXL_COLOR_PROFILE_TARGET_ORIGINAL;
      if (JXL_DEC_SUCCESS !=
          JxlDecoderGetICCProfileSize(dec, target, &icc_size)) {
        fprintf(stderr, "JxlDecoderGetICCProfileSize failed\n");
      }
      if (icc_size != 0) {
        ppf->orig_icc.resize(icc_size);
        if (JXL_DEC_SUCCESS !=
            JxlDecoderGetColorAsICCProfile(dec, target, ppf->orig_icc.data(),
                                           icc_size)) {
          fprintf(stderr, "JxlDecoderGetColorAsICCProfile failed\n");
          return false;
        }
      }

    } else if (status == JXL_DEC_FRAME) {
      if (!VerifyDimensions(constraints, ppf->xsize(), ppf->ysize())) {
        fprintf(stderr, "Image too big\n");
        return false;
      }
      total_pixel_count += static_cast<uint64_t>(ppf->xsize()) * ppf->ysize();
      if (constraints && (total_pixel_count > constraints->dec_max_pixels)) {
        return JXL_FAILURE("Image too big");
      }

      JxlFrameHeader fh;
      if (JXL_DEC_SUCCESS != JxlDecoderGetFrameHeader(dec, &fh)) {
        fprintf(stderr, "JxlDecoderGetFrameHeader failed\n");
        return false;
      }
      JxlFrameEncoding frame_encoding = JXL_FRAME_ENCODING_UNKNOWN;
      if (dparams.preview_hooks != nullptr &&
          JXL_DEC_SUCCESS !=
              dparams.preview_hooks->get_frame_encoding(dec, &frame_encoding)) {
        frame_encoding = JXL_FRAME_ENCODING_UNKNOWN;
      }
      const bool pixel_output =
          jpeg_bytes == nullptr && !accepted_formats.empty();
      const bool preview_output = preview_requested && pixel_output;
      // The first displayed frame ends with this frame: with coalescing, every
      // frame is displayed; without, the layers up to the first with a duration
      // (or the last) make it up.
      stop_after_frame = first_frame_only && pixel_output &&
                         (dparams.coalescing || fh.duration > 0 || fh.is_last);
      if (preview_output) {
        if (dparams.preview_hooks != nullptr) {
          if (JXL_DEC_SUCCESS !=
              dparams.preview_hooks->set_native_paths(
                  dec,
                  TO_JXL_BOOL(PreviewBackendAllowed(
                      allowed_backends,
                      JXLPreviewBackend::kNativeReducedInput)),
                  TO_JXL_BOOL(PreviewBackendAllowed(
                      allowed_backends,
                      JXLPreviewBackend::kNativeFusedUpsampling)),
                  TO_JXL_BOOL(PreviewBackendAllowed(
                      allowed_backends, JXLPreviewBackend::kNativeDcOnly)))) {
            fprintf(stderr, "Setting the preview render methods failed\n");
            return false;
          }
        } else if (!PreviewBackendAllowed(
                       allowed_backends,
                       JXLPreviewBackend::kDecoderDownsample)) {
          fprintf(stderr,
                  "Restricting the decoder's preview render methods requires "
                  "the decoder preview hooks\n");
          return FailWithReason(dparams,
                                JXLPreviewFailureReason::kNoBackendAvailable);
        }
        if (JXL_DEC_SUCCESS != JxlDecoderSetImageOutDownsampling(
                                   dec, dparams.preview_downsampling)) {
          fprintf(stderr, "JxlDecoderSetImageOutDownsampling failed\n");
          return false;
        }
        // Decoding stops after this frame, so the decoder does not need to
        // keep it for reference by later frames (and refuses to go on).
        if (stop_after_frame &&
            JXL_DEC_SUCCESS !=
                JxlDecoderSetPreferPreviewInplaceFlush(dec, JXL_TRUE)) {
          fprintf(stderr, "JxlDecoderSetPreferPreviewInplaceFlush failed\n");
          return false;
        }
      }
      if (preview_requested) {
        LogPreviewFrame(ppf->info, frame_encoding, fh,
                        dparams.preview_downsampling);
      }

      // The frame header is at full resolution. At the preview scale the
      // decoder renders the frame (with coalescing, the image; without, the
      // layer) at DivCeil(size, factor), and a layer is placed at
      // floor(crop / factor).
      size_t frame_xsize = fh.layer_info.xsize;
      size_t frame_ysize = fh.layer_info.ysize;
      if (preview_output) {
        frame_xsize = DivCeil(frame_xsize, dparams.preview_downsampling);
        frame_ysize = DivCeil(frame_ysize, dparams.preview_downsampling);
      }
      StatusOr<jxl::extras::PackedFrame> created_frame =
          jxl::extras::PackedFrame::Create(frame_xsize, frame_ysize, format);
      if (!created_frame.ok()) {
        fprintf(stderr, "Failed to create image frame.\n");
        return FailWithReason(dparams, JXLPreviewFailureReason::kOutOfMemory);
      }
      jxl::extras::PackedFrame frame = std::move(created_frame).value_();
      if (JXL_DEC_SUCCESS != JxlDecoderGetFrameHeader(dec, &frame.frame_info)) {
        fprintf(stderr, "JxlDecoderGetFrameHeader failed\n");
        return false;
      }
      if (preview_output) {
        const int32_t factor =
            static_cast<int32_t>(dparams.preview_downsampling);
        frame.frame_info.layer_info.xsize = frame_xsize;
        frame.frame_info.layer_info.ysize = frame_ysize;
        frame.frame_info.layer_info.crop_x0 =
            FloorDiv(frame.frame_info.layer_info.crop_x0, factor);
        frame.frame_info.layer_info.crop_y0 =
            FloorDiv(frame.frame_info.layer_info.crop_y0, factor);
      }
      frame.name.resize(frame.frame_info.name_length + 1, 0);
      if (JXL_DEC_SUCCESS !=
          JxlDecoderGetFrameName(dec, const_cast<char*>(frame.name.data()),
                                 frame.name.size())) {
        fprintf(stderr, "JxlDecoderGetFrameName failed\n");
        return false;
      }
      frame.name.resize(frame.frame_info.name_length);
      ppf->frames.emplace_back(std::move(frame));
      progression_index = 0;
      progression_target_reached = false;
      image_output_set = false;
    } else if (status == JXL_DEC_FRAME_PROGRESSION) {
      size_t downsampling = JxlDecoderGetIntendedDownsamplingRatio(dec);
      // A flush is only acceptable once it has the requested detail: enough
      // passes, or a step at no more than effective_max_downsampling. Without
      // an explicit max_downsampling that is the preview factor itself, so a
      // factor 2 or 4 preview is not served from the 1/8-resolution DC step
      // (the decoder pauses there even when the whole file is available).
      // Callers that prefer a coarser but cheaper preview can raise
      // max_downsampling.
      if (max_passes_defined ? progression_index >= dparams.max_passes
                             : downsampling <= effective_max_downsampling) {
        progression_target_reached = true;
      }
      // Each frame is flushed at its target and the rest of it skipped. When
      // decoding stops after the first displayed frame, only its last layer
      // is: the layers before it are decoded whole, since later layers can
      // reference them.
      if (image_output_set && progression_target_reached &&
          (!first_frame_only || stop_after_frame) &&
          PreviewBackendAllowed(allowed_backends,
                                JXLPreviewBackend::kNativeProgressionFlush) &&
          CurrentMethodAllowed(allowed_backends, dec)) {
        if (JXL_DEC_SUCCESS != JxlDecoderFlushImage(dec)) {
          fprintf(stderr, "JxlDecoderFlushImage failed\n");
          return false;
        }
        if (preview_requested &&
            preview_backend_used == JXLPreviewBackend::kNone) {
          preview_backend_used = JXLPreviewBackend::kNativeProgressionFlush;
        }
        if (first_frame_only || ppf->frames.back().frame_info.is_last) {
          break;
        }
        if (JXL_DEC_SUCCESS != JxlDecoderSkipCurrentFrame(dec)) {
          fprintf(stderr, "JxlDecoderSkipCurrentFrame failed\n");
          return false;
        }
      }
      ++progression_index;
    } else if (status == JXL_DEC_NEED_PREVIEW_OUT_BUFFER) {
      size_t buffer_size;
      if (JXL_DEC_SUCCESS !=
          JxlDecoderPreviewOutBufferSize(dec, &format, &buffer_size)) {
        fprintf(stderr, "JxlDecoderPreviewOutBufferSize failed\n");
        return false;
      }
      StatusOr<jxl::extras::PackedImage> preview_image =
          jxl::extras::PackedImage::Create(ppf->info.preview.xsize,
                                           ppf->info.preview.ysize, format);
      if (!preview_image.ok()) {
        fprintf(stderr, "Failed to create preview image.\n");
        return FailWithReason(dparams, JXLPreviewFailureReason::kOutOfMemory);
      }
      ppf->preview_frame = jxl::make_unique<jxl::extras::PackedFrame>(
          std::move(preview_image).value_());
      ppf->preview_frame->frame_info.layer_info.xsize = ppf->info.preview.xsize;
      ppf->preview_frame->frame_info.layer_info.ysize = ppf->info.preview.ysize;
      if (buffer_size != ppf->preview_frame->color.pixels_size) {
        fprintf(stderr, "Invalid out buffer size %" PRIuS " %" PRIuS "\n",
                buffer_size, ppf->preview_frame->color.pixels_size);
        return false;
      }
      if (JXL_DEC_SUCCESS !=
          JxlDecoderSetPreviewOutBuffer(
              dec, &format, ppf->preview_frame->color.pixels(), buffer_size)) {
        fprintf(stderr, "JxlDecoderSetPreviewOutBuffer failed\n");
        return false;
      }
      if (!SetOutputBitDepth(dec, dparams, format, &ppf->info)) return false;
    } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
      if (jpeg_bytes != nullptr) {
        break;
      }
      size_t buffer_size;
      jxl::extras::PackedFrame& frame = ppf->frames.back();

      if (JXL_DEC_SUCCESS !=
          JxlDecoderImageOutBufferSize(dec, &format, &buffer_size)) {
        fprintf(stderr, "JxlDecoderImageOutBufferSize failed\n");
        return false;
      }
      if (buffer_size != frame.color.pixels_size) {
        fprintf(stderr, "Invalid out buffer size %" PRIuS " %" PRIuS "\n",
                buffer_size, frame.color.pixels_size);
        return false;
      }

      if (dparams.use_image_callback) {
        auto callback = [](void* opaque, size_t x, size_t y, size_t num_pixels,
                           const void* pixels) {
          auto* ppf = reinterpret_cast<jxl::extras::PackedPixelFile*>(opaque);
          jxl::extras::PackedImage& color = ppf->frames.back().color;
          uint8_t* pixels_buffer = reinterpret_cast<uint8_t*>(color.pixels());
          size_t sample_size = color.pixel_stride();
          memcpy(pixels_buffer + (color.stride * y + sample_size * x), pixels,
                 num_pixels * sample_size);
        };
        if (JXL_DEC_SUCCESS !=
            JxlDecoderSetImageOutCallback(dec, &format, callback, ppf)) {
          fprintf(stderr, "JxlDecoderSetImageOutCallback failed\n");
          return false;
        }
      } else {
        if (JXL_DEC_SUCCESS != JxlDecoderSetImageOutBuffer(dec, &format,
                                                           frame.color.pixels(),
                                                           buffer_size)) {
          fprintf(stderr, "JxlDecoderSetImageOutBuffer failed\n");
          return false;
        }
      }
      if (!SetOutputBitDepth(dec, dparams, format, &ppf->info)) return false;
      JxlPixelFormat ec_format = format;
      ec_format.num_channels = 1;
      for (auto& eci : ppf->extra_channels_info) {
        StatusOr<jxl::extras::PackedImage> image =
            jxl::extras::PackedImage::Create(frame.frame_info.layer_info.xsize,
                                             frame.frame_info.layer_info.ysize,
                                             ec_format);
        if (!image.ok()) {
          fprintf(stderr, "Failed to create extra channel image.\n");
          return FailWithReason(dparams, JXLPreviewFailureReason::kOutOfMemory);
        }
        frame.extra_channels.emplace_back(std::move(image).value_());
        auto& ec = frame.extra_channels.back();
        size_t ec_buffer_size;
        if (JXL_DEC_SUCCESS != JxlDecoderExtraChannelBufferSize(dec, &ec_format,
                                                                &ec_buffer_size,
                                                                eci.index)) {
          fprintf(stderr, "JxlDecoderExtraChannelBufferSize failed\n");
          return false;
        }
        if (ec_buffer_size != ec.pixels_size) {
          fprintf(stderr,
                  "Invalid extra channel buffer size"
                  " %" PRIuS " %" PRIuS "\n",
                  ec_buffer_size, ec.pixels_size);
          return false;
        }
        if (JXL_DEC_SUCCESS !=
            JxlDecoderSetExtraChannelBuffer(dec, &ec_format, ec.pixels(),
                                            ec_buffer_size, eci.index)) {
          fprintf(stderr, "JxlDecoderSetExtraChannelBuffer failed\n");
          return false;
        }
        UpdateBitDepth(dparams.output_bitdepth, ec_format.data_type,
                       &eci.ec_info);
      }
      image_output_set = true;
    } else if (status == JXL_DEC_SUCCESS) {
      // Decoding finished successfully.
      break;
    } else if (status == JXL_DEC_PREVIEW_IMAGE) {
      if (use_embedded_preview) {
        if (!ppf->preview_frame) {
          fprintf(stderr, "Embedded preview frame missing\n");
          return false;
        }
        ppf->frames.clear();
        jxl::extras::PackedFrame& preview = *ppf->preview_frame;
        preview.frame_info.is_last = JXL_TRUE;
        ppf->info.xsize = preview.color.xsize;
        ppf->info.ysize = preview.color.ysize;
        // The output is the preview, which has no preview of its own.
        ppf->info.have_preview = JXL_FALSE;
        ppf->info.preview = {};
        ppf->frames.emplace_back(std::move(preview));
        ppf->preview_frame.reset();
        preview_backend_used = JXLPreviewBackend::kEmbeddedPreview;
        returned_embedded_preview = true;
        codestream_done = true;
        break;
      }
    } else if (status == JXL_DEC_FULL_IMAGE) {
      // The decoder rendered the frame whole (a flushed frame ends at its
      // flush): its render method must be allowed. Of several layers, the
      // first one's is reported.
      if (preview_requested) {
        const JXLPreviewBackend backend = DecoderPreviewBackend(dec);
        if (backend != JXLPreviewBackend::kNone &&
            !PreviewBackendAllowed(allowed_backends, backend)) {
          return FailWithReason(dparams,
                                JXLPreviewFailureReason::kNoBackendAvailable);
        }
        if (preview_backend_used == JXLPreviewBackend::kNone) {
          preview_backend_used = backend;
        }
      }
      if (jpeg_bytes != nullptr || ppf->frames.back().frame_info.is_last ||
          stop_after_frame) {
        codestream_done = true;
      }
      if (stop_after_frame) break;
    } else {
      fprintf(stderr, "Error: unexpected status: %d\n",
              static_cast<int>(status));
      return false;
    }
  }
  boxes.FinalizeOutput();
  if (!ppf->metadata.exif.empty()) {
    // Verify that Exif box has a valid TIFF header at the specified offset.
    // Discard bytes preceding the header.
    // 16 = 4 + 12 = offset + min EXIF payload.
    if (ppf->metadata.exif.size() >= 16) {
      uint32_t offset = LoadBE32(ppf->metadata.exif.data());
      if (offset <= ppf->metadata.exif.size() - 16) {
        std::vector<uint8_t> exif(ppf->metadata.exif.begin() + 4 + offset,
                                  ppf->metadata.exif.end());
        bool bigendian;
        if (IsExif(exif, &bigendian)) {
          ppf->metadata.exif = std::move(exif);
          if (jpeg_bytes == nullptr && !dparams.keep_orientation) {
            // when decoding to pixels and orientation is undone during decode,
            // reset exif orientation to avoid double orientation
            ResetExifOrientation(ppf->metadata.exif);
          }
        } else {
          fprintf(stderr, "Warning: invalid TIFF header in Exif\n");
        }
      } else {
        fprintf(stderr, "Warning: invalid Exif offset: %" PRIu32 "\n", offset);
      }
    } else {
      fprintf(stderr, "Warning: invalid Exif length: %" PRIuS "\n",
              ppf->metadata.exif.size());
    }
  }
  if (jpeg_bytes != nullptr) {
    if (!can_reconstruct_jpeg) return false;
    size_t used_jpeg_output =
        jpeg_data_chunk.size() - JxlDecoderReleaseJPEGBuffer(dec);
    jpeg_bytes->insert(jpeg_bytes->end(), jpeg_data_chunk.data(),
                       jpeg_data_chunk.data() + used_jpeg_output);
  }
  if (preview_requested && preview_backend_used == JXLPreviewBackend::kNone &&
      !returned_embedded_preview && jpeg_bytes == nullptr &&
      !accepted_formats.empty() && !ppf->frames.empty()) {
    // A frame flushed from partial input without a progression event.
    preview_backend_used = DecoderPreviewBackend(dec);
    if (preview_backend_used != JXLPreviewBackend::kNone &&
        !PreviewBackendAllowed(allowed_backends, preview_backend_used)) {
      return FailWithReason(dparams,
                            JXLPreviewFailureReason::kNoBackendAvailable);
    }
  }
  if (PreviewDebugEnabled() && preview_requested) {
    const char* name = "kNone";
    switch (preview_backend_used) {
      case JXLPreviewBackend::kNone:
        name = "kNone";
        break;
      case JXLPreviewBackend::kEmbeddedPreview:
        name = "kEmbeddedPreview";
        break;
      case JXLPreviewBackend::kNativeDcOnly:
        name = "kNativeDcOnly";
        break;
      case JXLPreviewBackend::kNativeProgressionFlush:
        name = "kNativeProgressionFlush";
        break;
      case JXLPreviewBackend::kNativeReducedInput:
        name = "kNativeReducedInput";
        break;
      case JXLPreviewBackend::kNativeFusedUpsampling:
        name = "kNativeFusedUpsampling";
        break;
      case JXLPreviewBackend::kFallbackDownsample:
        name = "kFallbackDownsample";
        break;
      case JXLPreviewBackend::kDecoderDownsample:
        name = "kDecoderDownsample";
        break;
    }
    fprintf(stderr, "[preview-debug] backend=%s\n", name);
  }
  if (dparams.preview_backend != nullptr) {
    *dparams.preview_backend = preview_backend_used;
  }
  if (preview_requested && !returned_embedded_preview &&
      jpeg_bytes == nullptr && !accepted_formats.empty()) {
    // The canvas at the preview scale; without coalescing, the frames are
    // layers on it.
    ppf->info.xsize = DivCeil(ppf->info.xsize, dparams.preview_downsampling);
    ppf->info.ysize = DivCeil(ppf->info.ysize, dparams.preview_downsampling);
  }
  if (decoded_bytes) {
    *decoded_bytes = bytes_size - JxlDecoderReleaseInput(dec);
  }
  return true;
}

}  // namespace extras
}  // namespace jxl
