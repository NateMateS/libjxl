// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "lib/jxl/dec_cache.h"

#include <jxl/memory_manager.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "lib/jxl/ac_strategy.h"
#include "lib/jxl/base/bits.h"
#include "lib/jxl/base/data_parallel.h"
#include "lib/jxl/base/printf_macros.h"
#include "lib/jxl/base/status.h"
#include "lib/jxl/blending.h"
#include "lib/jxl/cms/color_encoding_cms.h"
#include "lib/jxl/coeff_order.h"
#include "lib/jxl/color_encoding_internal.h"
#include "lib/jxl/common.h"  // JXL_HIGH_PRECISION
#include "lib/jxl/frame_dimensions.h"
#include "lib/jxl/frame_header.h"
#include "lib/jxl/image.h"
#include "lib/jxl/image_bundle.h"
#include "lib/jxl/image_metadata.h"
#include "lib/jxl/image_ops.h"
#include "lib/jxl/loop_filter.h"
#include "lib/jxl/memory_manager_internal.h"
#include "lib/jxl/render_pipeline/render_pipeline.h"
#include "lib/jxl/render_pipeline/stage_blending.h"
#include "lib/jxl/render_pipeline/stage_chroma_upsampling.h"
#include "lib/jxl/render_pipeline/stage_cms.h"
#include "lib/jxl/render_pipeline/stage_epf.h"
#include "lib/jxl/render_pipeline/stage_from_linear.h"
#include "lib/jxl/render_pipeline/stage_gaborish.h"
#include "lib/jxl/render_pipeline/stage_noise.h"
#include "lib/jxl/render_pipeline/stage_patches.h"
#include "lib/jxl/render_pipeline/stage_splines.h"
#include "lib/jxl/render_pipeline/stage_spot.h"
#include "lib/jxl/render_pipeline/stage_to_linear.h"
#include "lib/jxl/render_pipeline/stage_tone_mapping.h"
#include "lib/jxl/render_pipeline/stage_upsampling.h"
#include "lib/jxl/render_pipeline/stage_write.h"
#include "lib/jxl/render_pipeline/stage_xyb.h"
#include "lib/jxl/render_pipeline/stage_ycbcr.h"

// Set to 1 to log why a preview frame did not take a native render path, and
// which render path it took.
#ifndef JXL_DEBUG_PREVIEW
#define JXL_DEBUG_PREVIEW 0
#endif  // JXL_DEBUG_PREVIEW

namespace jxl {

Status GroupDecCache::InitOnce(JxlMemoryManager* memory_manager,
                               size_t num_passes, size_t used_acs) {
  for (size_t i = 0; i < num_passes; i++) {
    if (num_nzeroes[i].xsize() == 0) {
      // Allocate enough for a whole group - partial groups on the
      // right/bottom border just use a subset. The valid size is passed via
      // Rect.

      JXL_ASSIGN_OR_RETURN(num_nzeroes[i],
                           Image3I::Create(memory_manager, kGroupDimInBlocks,
                                           kGroupDimInBlocks));
    }
  }
  size_t max_block_area = 0;

  for (uint8_t o = 0; o < AcStrategy::kNumValidStrategies; ++o) {
    AcStrategy acs = AcStrategy::FromRawStrategy(o);
    if ((used_acs & (1 << o)) == 0) continue;
    size_t area =
        acs.covered_blocks_x() * acs.covered_blocks_y() * kDCTBlockSize;
    max_block_area = std::max(area, max_block_area);
  }

  if (max_block_area > max_block_area_) {
    max_block_area_ = max_block_area;
    // Native reduced path needs less, but fallback to TransformToPixels
    // requires the full 4*max_block_area. Since max_block_area is quite small,
    // we allocate the conservative amount for the fallback.
    const size_t scratch_floats = 4 * max_block_area_;
    JXL_ASSIGN_OR_RETURN(
        float_memory_,
        AlignedMemory::Create(
            memory_manager,
            (max_block_area_ * 3 + scratch_floats) * sizeof(float)));
    // We need 3x int32 or int16 blocks for quantized coefficients.
    JXL_ASSIGN_OR_RETURN(
        int32_memory_,
        AlignedMemory::Create(memory_manager,
                              max_block_area_ * 3 * sizeof(int32_t)));
    JXL_ASSIGN_OR_RETURN(
        int16_memory_,
        AlignedMemory::Create(memory_manager,
                              max_block_area_ * 3 * sizeof(int16_t)));
  }

  dec_group_block = float_memory_.address<float>();
  scratch_space = dec_group_block + max_block_area_ * 3;
  dec_group_qblock = int32_memory_.address<int32_t>();
  dec_group_qblock16 = int16_memory_.address<int16_t>();
  return true;
}

// Initialize the decoder state after all of DC is decoded.
Status PassesDecoderState::InitForAC(size_t num_passes, ThreadPool* pool) {
  shared_storage.coeff_order_size = 0;
  for (uint8_t o = 0; o < AcStrategy::kNumValidStrategies; ++o) {
    if (((1 << o) & used_acs) == 0) continue;
    uint8_t ord = kStrategyOrder[o];
    shared_storage.coeff_order_size =
        std::max(kCoeffOrderOffset[3 * (ord + 1)] * kDCTBlockSize,
                 shared_storage.coeff_order_size);
  }
  size_t sz = num_passes * shared_storage.coeff_order_size;
  if (sz > shared_storage.coeff_orders.size()) {
    shared_storage.coeff_orders.resize(sz);
  }
  return true;
}

namespace {

struct PreviewPipelinePlan {
  size_t stored_downsampling = 1;
  size_t effective_frame_upsampling = 1;
  size_t writer_downsampling = 1;
  JxlImageOutDownsamplingMethod method = JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_NONE;
};

bool IsSupportedPreviewFactor(size_t factor) {
  return factor == 2 || factor == 4 || factor == 8;
}

FrameDimensions BuildPreviewPipelineFrameDimensions(
    const FrameDimensions& original, size_t stored_downsampling,
    size_t effective_frame_upsampling, size_t output_downsampling) {
  FrameDimensions dim = original;
  if (stored_downsampling > 1) {
    const size_t factor = stored_downsampling;
    JXL_DASSERT(original.group_dim % factor == 0);
    dim.xsize = DivCeil(original.xsize, factor);
    dim.ysize = DivCeil(original.ysize, factor);
    dim.xsize_padded = DivCeil(original.xsize_padded, factor);
    dim.ysize_padded = DivCeil(original.ysize_padded, factor);
    dim.xsize_blocks = DivCeil(original.xsize_blocks, factor);
    dim.ysize_blocks = DivCeil(original.ysize_blocks, factor);
    dim.group_dim = original.group_dim / factor;
    dim.dc_group_dim = original.dc_group_dim / factor;
  }
  dim.xsize_upsampled = DivCeil(original.xsize_upsampled, output_downsampling);
  dim.ysize_upsampled = DivCeil(original.ysize_upsampled, output_downsampling);
  dim.xsize_upsampled_padded = dim.xsize_padded * effective_frame_upsampling;
  dim.ysize_upsampled_padded = dim.ysize_padded * effective_frame_upsampling;
  // The group counts are copied from `original`: the pipeline addresses groups
  // by the frame's group ids, so the reduced sizes must form the same grids.
  JXL_DASSERT(DivCeil(dim.xsize, dim.group_dim) == original.xsize_groups);
  JXL_DASSERT(DivCeil(dim.ysize, dim.group_dim) == original.ysize_groups);
  JXL_DASSERT(DivCeil(dim.xsize_blocks, dim.group_dim) ==
              original.xsize_dc_groups);
  JXL_DASSERT(DivCeil(dim.ysize_blocks, dim.group_dim) ==
              original.ysize_dc_groups);
  return dim;
}

// Returns whether the image has an alpha channel, and its pipeline channel.
bool FindAlphaChannel(const ImageMetadata& metadata, size_t* alpha_c) {
  for (size_t i = 0; i < metadata.extra_channel_info.size(); i++) {
    if (metadata.extra_channel_info[i].type == ExtraChannel::kAlpha) {
      *alpha_c = 3 + i;
      return true;
    }
  }
  return false;
}

std::unique_ptr<RenderPipelineStage> GetOutputStage(
    PassesDecoderState* state, const ImageMetadata& metadata) {
  size_t alpha_c = 0;
  const bool has_alpha = FindAlphaChannel(metadata, &alpha_c);
  const size_t full_width =
      state->full_output_width == 0 ? state->width : state->full_output_width;
  const size_t full_height = state->full_output_height == 0
                                 ? state->height
                                 : state->full_output_height;
  return GetWriteToOutputStage(
      state->main_output, state->width, state->height, full_width, full_height,
      state->writer_downsampling, has_alpha, state->unpremul_alpha, alpha_c,
      state->undo_orientation, state->extra_output, state->memory_manager());
}
}  // namespace

Status PassesDecoderState::PreparePipeline(const FrameHeader& frame_header,
                                           const ImageMetadata* metadata,
                                           ImageBundle* decoded,
                                           PipelineOptions options) {
  JxlMemoryManager* memory_manager = this->memory_manager();
  const bool reduced_preview =
      output_downsampling > 1 &&
      (main_output.callback.IsPresent() || main_output.buffer != nullptr);
  // The decoder only downsamples the output of displayed frames: the shortcuts
  // below (skipped filters and noise, reduced input) would corrupt the data of
  // DC and reference-only frames, which later frames read.
  JXL_ENSURE(!reduced_preview ||
             frame_header.frame_type == FrameType::kRegularFrame ||
             frame_header.frame_type == FrameType::kSkipProgressive);
  // Native preview: feed the pipeline at preview/intermediate resolution so
  // that fewer pixels flow through the pipeline stages and WriteToOutputStage.
  // For VarDCT: TransformToReducedPixels produces reduced-resolution IDCT
  // output directly (factors 2, 4, and 8).
  // For Modular: FinalizeDecoding box-downsamples the decoded channels after
  // undo_transforms, then feeds the small image through the pipeline.
  // For frame-upsampled images, fused preview reduces or removes the frame
  // upsampling stage so preview downsampling remains relative to the final
  // displayed image without rendering full displayed pixels first.
  // Factors 2, 4, and 8 are supported for modular (pixel-level averaging).
  // Requires compatible extra channels, no blending, no patches/splines, and
  // 4:4:4 chroma (no chroma upsampling to skip).
  // When the caller has promised they won't decode any subsequent frames
  // (preview-only thumbnail use case), `CanBeReferenced()` no longer matters
  // because the saved reference would never be consumed.  We also skip the
  // reference save below to keep state consistent with that promise.
  const bool ignore_reference_save =
      options.preview_no_future_frames && reduced_preview;
  const bool gate_ds_supported = IsSupportedPreviewFactor(output_downsampling);
  const bool gate_encoding = (frame_header.encoding == FrameEncoding::kVarDCT ||
                              frame_header.encoding == FrameEncoding::kModular);
  // Allow no extra channels, or a single alpha channel whose per-frame
  // upsampling matches the (already gated ==1) frame upsampling. The modular
  // path box-downsamples all stored channels (color + alpha for Modular, alpha
  // only for VarDCT) together, so alpha travels at reduced resolution just
  // like color. Non-alpha extras (spot/depth/CFA/...) don't box-downsample
  // meaningfully and must take the fallback path.
  bool gate_no_extra_channels = (metadata->num_extra_channels == 0);
  if (!gate_no_extra_channels && metadata->num_extra_channels == 1 &&
      !metadata->extra_channel_info.empty() &&
      metadata->extra_channel_info[0].type == ExtraChannel::kAlpha &&
      (frame_header.extra_channel_upsampling.empty() ||
       frame_header.extra_channel_upsampling[0] == frame_header.upsampling)) {
    gate_no_extra_channels = true;
  }
  const bool gate_no_blending = !NeedsBlending(frame_header);
  // Whether this frame's pixels reach only this output: every shortcut that
  // changes them (reduced input, skipped filters or noise) requires it.
  const bool gate_no_referenceable =
      (!frame_header.CanBeReferenced() || ignore_reference_save);
  const bool gate_no_patches = !(frame_header.flags & FrameHeader::kPatches);
  const bool gate_no_splines = !(frame_header.flags & FrameHeader::kSplines);
  const bool gate_chroma_444 = frame_header.chroma_subsampling.Is444();
  const bool native_preview_safe =
      reduced_preview && gate_ds_supported && gate_encoding &&
      gate_no_extra_channels && gate_no_blending && gate_no_referenceable &&
      gate_no_patches && gate_no_splines && gate_chroma_444;
  // DC-only preview: at 1/8 output of a VarDCT frame without frame upsampling,
  // the DC image holds exactly one value per output pixel, so the pipeline is
  // fed from the DC (FrameDecoder::Flush) and the AC sections are never
  // decoded. The gates are those of the reduced-input path, with two
  // differences. Chroma subsampling is allowed: the DC stores chroma at the
  // subsampled resolution and the chroma upsampling stages below handle it.
  // Extra channels are not: they are modular data in the AC sections and
  // would be zero-filled. kSkipProgressive frames are never flushed early,
  // and JPEG reconstruction does not render pixels.
  const bool dc_only_safe =
      reduced_preview && output_downsampling == 8 &&
      options.allow_native_dc_only &&
      frame_header.encoding == FrameEncoding::kVarDCT &&
      frame_header.frame_type == FrameType::kRegularFrame &&
      frame_header.upsampling == 1 && metadata->num_extra_channels == 0 &&
      extra_output.empty() && gate_no_blending && gate_no_referenceable &&
      gate_no_patches && gate_no_splines && !decoded->IsJPEG();

  PreviewPipelinePlan preview_plan;
  preview_plan.effective_frame_upsampling = frame_header.upsampling;
  preview_plan.writer_downsampling = output_downsampling;
  if (dc_only_safe) {
    preview_plan.stored_downsampling = output_downsampling;
    preview_plan.effective_frame_upsampling = 1;
    preview_plan.writer_downsampling = 1;
    preview_plan.method = JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_DC_ONLY;
  } else if (reduced_preview) {
    preview_plan.method = JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_FULL_RESOLUTION;
    if (native_preview_safe && frame_header.upsampling == 1 &&
        options.allow_native_reduced_input) {
      preview_plan.stored_downsampling = output_downsampling;
      preview_plan.effective_frame_upsampling = 1;
      preview_plan.writer_downsampling = 1;
      preview_plan.method = JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_REDUCED_INPUT;
    } else if (native_preview_safe && frame_header.upsampling > 1 &&
               options.allow_native_fused_upsampling) {
      if (output_downsampling > frame_header.upsampling) {
        JXL_DASSERT(output_downsampling % frame_header.upsampling == 0);
        preview_plan.stored_downsampling =
            output_downsampling / frame_header.upsampling;
        preview_plan.effective_frame_upsampling = 1;
      } else if (output_downsampling == frame_header.upsampling) {
        preview_plan.stored_downsampling = 1;
        preview_plan.effective_frame_upsampling = 1;
      } else {
        JXL_DASSERT(frame_header.upsampling % output_downsampling == 0);
        preview_plan.stored_downsampling = 1;
        preview_plan.effective_frame_upsampling =
            frame_header.upsampling / output_downsampling;
      }
      preview_plan.writer_downsampling = 1;
      preview_plan.method = JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_FUSED_UPSAMPLING;
    }
  }

  const bool use_native_preview =
      preview_plan.method == JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_REDUCED_INPUT ||
      preview_plan.method ==
          JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_FUSED_UPSAMPLING ||
      preview_plan.method == JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_DC_ONLY;
  JXL_DEBUG(
      JXL_DEBUG_PREVIEW && reduced_preview && !use_native_preview,
      "native preview rejected: ds=%" PRIuS
      " enc=%s ds_supported=%d encoding_ok=%d no_extra_ch=%d(num=%u) "
      "no_blending=%d no_referenceable=%d(can_ref=%d ignore=%d) "
      "no_patches=%d no_splines=%d ups=%u chroma_444=%d "
      "allow_reduced=%d allow_fused=%d allow_dc=%d",
      output_downsampling,
      frame_header.encoding == FrameEncoding::kVarDCT ? "VarDCT" : "Modular",
      gate_ds_supported, gate_encoding, gate_no_extra_channels,
      metadata->num_extra_channels, gate_no_blending, gate_no_referenceable,
      frame_header.CanBeReferenced(), ignore_reference_save, gate_no_patches,
      gate_no_splines, frame_header.upsampling, gate_chroma_444,
      options.allow_native_reduced_input, options.allow_native_fused_upsampling,
      options.allow_native_dc_only);
  JXL_DEBUG(JXL_DEBUG_PREVIEW && reduced_preview,
            "preview downsampling method=%d ds=%" PRIuS,
            static_cast<int>(preview_plan.method), output_downsampling);
  // When using reduced-input preview, postprocess stages (EPF, Gaborish) must
  // also be skipped because they assume full-resolution neighborhood data.
  const bool skip_preview_postprocess =
      (reduced_preview && output_downsampling >= 4 && gate_no_blending &&
       gate_no_referenceable) ||
      preview_plan.stored_downsampling > 1;
  const bool skip_preview_noise =
      reduced_preview && gate_no_blending && gate_no_referenceable;
  pipeline_input_downsampling = preview_plan.stored_downsampling;
  effective_frame_upsampling = preview_plan.effective_frame_upsampling;
  writer_downsampling = preview_plan.writer_downsampling;
  downsampling_method = preview_plan.method;
  // WriteToOutputStage accumulates each writer_downsampling-sized box on the
  // thread that renders it; it declares Settings::rect_alignment so that the
  // pipeline never splits a box between rects (see LowMemoryRenderPipeline).
  // After blending the writer runs in image coordinates, where the frame's
  // edges are rect edges too, so a frame that is not aligned in the image is
  // rendered into `decoded` and written by WriteOutputFromImage instead.
  output_from_image =
      reduced_preview && writer_downsampling > 1 && options.coalescing &&
      NeedsBlending(frame_header) &&
      !FrameIsAlignedInImage(
          frame_header.frame_origin, shared->frame_dim.xsize_upsampled,
          shared->frame_dim.ysize_upsampled,
          frame_header.nonserialized_metadata->xsize(),
          frame_header.nonserialized_metadata->ysize(), writer_downsampling);
  size_t num_c = 3 + frame_header.nonserialized_metadata->m.num_extra_channels;
  bool render_noise =
      (options.render_noise && (frame_header.flags & FrameHeader::kNoise) != 0);
  size_t num_tmp_c = (render_noise && !skip_preview_noise) ? 3 : 0;
  pipeline_has_noise_channels = num_tmp_c != 0;

  if (frame_header.CanBeReferenced() && !ignore_reference_save) {
    // Necessary so that SetInputSizes() can allocate output buffers as needed.
    frame_storage_for_referencing = ImageBundle(memory_manager, metadata);
  }

  RenderPipeline::Builder builder(memory_manager, num_c + num_tmp_c);

  if (options.use_slow_render_pipeline) {
    builder.UseSimpleImplementation();
  }

  if (!frame_header.chroma_subsampling.Is444()) {
    for (size_t c = 0; c < 3; c++) {
      if (frame_header.chroma_subsampling.HShift(c) != 0) {
        JXL_RETURN_IF_ERROR(
            builder.AddStage(GetChromaUpsamplingStage(c, /*horizontal=*/true)));
      }
      if (frame_header.chroma_subsampling.VShift(c) != 0) {
        JXL_RETURN_IF_ERROR(builder.AddStage(
            GetChromaUpsamplingStage(c, /*horizontal=*/false)));
      }
    }
  }

  if (!skip_preview_postprocess && frame_header.loop_filter.gab) {
    JXL_RETURN_IF_ERROR(
        builder.AddStage(GetGaborishStage(frame_header.loop_filter)));
  }

  {
    const LoopFilter& lf = frame_header.loop_filter;
    if (!skip_preview_postprocess && lf.epf_iters >= 3) {
      JXL_RETURN_IF_ERROR(
          builder.AddStage(GetEPFStage(lf, sigma, EpfStage::Zero)));
    }
    if (!skip_preview_postprocess && lf.epf_iters >= 1) {
      JXL_RETURN_IF_ERROR(
          builder.AddStage(GetEPFStage(lf, sigma, EpfStage::One)));
    }
    if (!skip_preview_postprocess && lf.epf_iters >= 2) {
      JXL_RETURN_IF_ERROR(
          builder.AddStage(GetEPFStage(lf, sigma, EpfStage::Two)));
    }
  }

  bool late_ec_upsample = effective_frame_upsampling != 1;
  for (auto ecups : frame_header.extra_channel_upsampling) {
    if (ecups != frame_header.upsampling) {
      late_ec_upsample = false;
    }
  }

  if (!late_ec_upsample) {
    for (size_t ec = 0; ec < frame_header.extra_channel_upsampling.size();
         ec++) {
      size_t ec_upsampling = frame_header.extra_channel_upsampling[ec];
      if (use_native_preview && ec_upsampling == frame_header.upsampling) {
        ec_upsampling = effective_frame_upsampling;
      }
      if (ec_upsampling != 1) {
        JXL_RETURN_IF_ERROR(builder.AddStage(GetUpsamplingStage(
            memory_manager, frame_header.nonserialized_metadata->transform_data,
            3 + ec, CeilLog2Nonzero(ec_upsampling))));
      }
    }
  }

  if ((frame_header.flags & FrameHeader::kPatches) != 0) {
    JXL_RETURN_IF_ERROR(builder.AddStage(GetPatchesStage(
        &shared->image_features.patches,
        &frame_header.nonserialized_metadata->m.extra_channel_info)));
  }
  if ((frame_header.flags & FrameHeader::kSplines) != 0) {
    JXL_RETURN_IF_ERROR(
        builder.AddStage(GetSplineStage(&shared->image_features.splines)));
  }

  if (effective_frame_upsampling != 1) {
    size_t nb_channels =
        3 +
        (late_ec_upsample ? frame_header.extra_channel_upsampling.size() : 0);
    for (size_t c = 0; c < nb_channels; c++) {
      JXL_RETURN_IF_ERROR(builder.AddStage(GetUpsamplingStage(
          memory_manager, frame_header.nonserialized_metadata->transform_data,
          c, CeilLog2Nonzero(effective_frame_upsampling))));
    }
  }
  // Starting from this line all the stages considered to have zero xextra.
  // Upsampling does not have xextra as well (even if it happens before
  // splines/patches for EC).
  if (render_noise && !skip_preview_noise) {
    JXL_RETURN_IF_ERROR(builder.AddStage(GetConvolveNoiseStage(num_c)));
    JXL_RETURN_IF_ERROR(builder.AddStage(GetAddNoiseStage(
        shared->image_features.noise_params, shared->cmap.base(), num_c)));
  }
  if (frame_header.dc_level != 0) {
    JXL_RETURN_IF_ERROR(builder.AddStage(GetWriteToImage3FStage(
        memory_manager, &shared_storage.dc_frames[frame_header.dc_level - 1])));
  }

  if (frame_header.CanBeReferenced() && !ignore_reference_save &&
      frame_header.save_before_color_transform) {
    JXL_RETURN_IF_ERROR(builder.AddStage(GetWriteToImageBundleStage(
        &frame_storage_for_referencing, &metadata->color_encoding)));
  }

  // The fast stage replaces the whole color conversion and write chain and
  // writes pipeline pixels to the output 1:1, so it cannot be used when the
  // writer downsamples (the pipeline then runs above output resolution).
  if (fast_xyb_srgb8_conversion && writer_downsampling == 1) {
#if !JXL_HIGH_PRECISION
    JXL_ENSURE(!NeedsBlending(frame_header));
    JXL_ENSURE(!frame_header.CanBeReferenced() || ignore_reference_save ||
               frame_header.save_before_color_transform);
    JXL_ENSURE(!options.render_spotcolors ||
               !metadata->Find(ExtraChannel::kSpotColor));
    size_t alpha_c = 0;
    const bool has_alpha = FindAlphaChannel(*metadata, &alpha_c);
    bool is_rgba = (main_output.format.num_channels == 4);
    uint8_t* rgb_output = reinterpret_cast<uint8_t*>(main_output.buffer);
    JXL_RETURN_IF_ERROR(builder.AddStage(
        GetFastXYBTosRGB8Stage(rgb_output, main_output.stride, width, height,
                               is_rgba, has_alpha, alpha_c)));
#endif
  } else {
    bool linear = false;
    if (frame_header.color_transform == ColorTransform::kYCbCr) {
      JXL_RETURN_IF_ERROR(builder.AddStage(GetYCbCrStage()));
    } else if (frame_header.color_transform == ColorTransform::kXYB) {
      JXL_RETURN_IF_ERROR(builder.AddStage(GetXYBStage(output_encoding_info)));
      if (output_encoding_info.color_encoding.GetColorSpace() !=
          ColorSpace::kXYB) {
        linear = true;
      }
    }  // Nothing to do for kNone.

    if (options.coalescing && NeedsBlending(frame_header)) {
      if (linear) {
        JXL_RETURN_IF_ERROR(
            builder.AddStage(GetFromLinearStage(output_encoding_info)));
        linear = false;
      }
      JXL_RETURN_IF_ERROR(builder.AddStage(GetBlendingStage(
          frame_header, this, output_encoding_info.color_encoding)));
    }

    if (options.coalescing && frame_header.CanBeReferenced() &&
        !ignore_reference_save && !frame_header.save_before_color_transform) {
      if (linear) {
        JXL_RETURN_IF_ERROR(
            builder.AddStage(GetFromLinearStage(output_encoding_info)));
        linear = false;
      }
      JXL_RETURN_IF_ERROR(builder.AddStage(GetWriteToImageBundleStage(
          &frame_storage_for_referencing, &metadata->color_encoding)));
    }

    if (options.render_spotcolors &&
        frame_header.nonserialized_metadata->m.Find(ExtraChannel::kSpotColor)) {
      for (size_t i = 0; i < metadata->extra_channel_info.size(); i++) {
        // Don't use Find() because there may be multiple spot color channels.
        const ExtraChannelInfo& eci = metadata->extra_channel_info[i];
        if (eci.type == ExtraChannel::kSpotColor) {
          JXL_RETURN_IF_ERROR(
              builder.AddStage(GetSpotColorStage(i, eci.spot_color)));
        }
      }
    }

    auto tone_mapping_stage = GetToneMappingStage(output_encoding_info);
    if (tone_mapping_stage) {
      if (!linear) {
        auto to_linear_stage = GetToLinearStage(output_encoding_info);
        if (!to_linear_stage) {
          if (!output_encoding_info.cms_set) {
            return JXL_FAILURE("Cannot tonemap this colorspace without a CMS");
          }
          auto cms_stage = GetCmsStage(output_encoding_info);
          if (cms_stage) {
            JXL_RETURN_IF_ERROR(builder.AddStage(std::move(cms_stage)));
          }
        } else {
          JXL_RETURN_IF_ERROR(builder.AddStage(std::move(to_linear_stage)));
        }
        linear = true;
      }
      JXL_RETURN_IF_ERROR(builder.AddStage(std::move(tone_mapping_stage)));
    }

    if (linear) {
      const size_t channels_src =
          (output_encoding_info.orig_color_encoding.IsCMYK()
               ? 4
               : output_encoding_info.orig_color_encoding.Channels());
      const size_t channels_dst =
          output_encoding_info.color_encoding.Channels();
      bool mixing_color_and_grey = (channels_dst != channels_src);
      if ((output_encoding_info.color_encoding_is_original) ||
          (!output_encoding_info.cms_set) || mixing_color_and_grey) {
        // in those cases we only need a linear stage in other cases we attempt
        // to obtain a cms stage: the cases are
        // - output_encoding_info.color_encoding_is_original: no cms stage
        // needed because it would be a no-op
        // - !output_encoding_info.cms_set: can't use the cms, so no point in
        // trying to add a cms stage
        // - mixing_color_and_grey: cms stage can't handle that
        // TODO(firsching): remove "mixing_color_and_grey" condition after
        // adding support for greyscale to cms stage.
        JXL_RETURN_IF_ERROR(
            builder.AddStage(GetFromLinearStage(output_encoding_info)));
      } else {
        if (!output_encoding_info.linear_color_encoding.CreateICC()) {
          return JXL_FAILURE("Failed to create ICC");
        }
        auto cms_stage = GetCmsStage(output_encoding_info);
        if (cms_stage) {
          JXL_RETURN_IF_ERROR(builder.AddStage(std::move(cms_stage)));
        }
      }
      linear = false;
    } else {
      auto cms_stage = GetCmsStage(output_encoding_info, false);
      if (cms_stage) {
        JXL_RETURN_IF_ERROR(builder.AddStage(std::move(cms_stage)));
      }
    }
    (void)linear;

    if ((main_output.callback.IsPresent() || main_output.buffer) &&
        !output_from_image) {
      JXL_RETURN_IF_ERROR(builder.AddStage(GetOutputStage(this, *metadata)));
    } else {
      JXL_RETURN_IF_ERROR(builder.AddStage(GetWriteToImageBundleStage(
          decoded, &output_encoding_info.color_encoding)));
    }
  }
  FrameDimensions pipeline_frame_dim =
      use_native_preview ? BuildPreviewPipelineFrameDimensions(
                               shared->frame_dim, pipeline_input_downsampling,
                               effective_frame_upsampling, output_downsampling)
                         : shared->frame_dim;
  JXL_ASSIGN_OR_RETURN(render_pipeline,
                       std::move(builder).Finalize(pipeline_frame_dim));
  return render_pipeline->IsInitialized();
}

Status PassesDecoderState::WriteOutputFromImage(const ImageBundle& image,
                                                ThreadPool* pool) {
  JXL_ENSURE(output_from_image);
  const ImageMetadata& metadata = *image.metadata();
  const size_t num_c = 3 + metadata.num_extra_channels;
  JXL_ENSURE(image.extra_channels().size() >= metadata.num_extra_channels);
  const size_t xsize = image.xsize();
  const size_t ysize = image.ysize();
  RenderPipeline::Builder builder(memory_manager(), num_c);
  JXL_RETURN_IF_ERROR(builder.AddStage(GetOutputStage(this, metadata)));
  // The image starts at the origin and has no blending stage, so every rect
  // is aligned to the writer's factor.
  FrameDimensions frame_dim;
  frame_dim.Set(xsize, ysize, /*group_size_shift=*/1, /*max_hshift=*/0,
                /*max_vshift=*/0, /*modular_mode=*/false, /*upsampling=*/1);
  JXL_ASSIGN_OR_RETURN(std::unique_ptr<RenderPipeline> pipeline,
                       std::move(builder).Finalize(frame_dim));
  JXL_RETURN_IF_ERROR(pipeline->IsInitialized());
  const auto prepare = [&pipeline](size_t num_threads) -> Status {
    return pipeline->PrepareForThreads(num_threads, /*use_group_ids=*/false);
  };
  const auto write_group = [&](uint32_t group, size_t thread) -> Status {
    RenderPipelineInput input = pipeline->GetInputBuffers(group, thread);
    const size_t gx = group % frame_dim.xsize_groups;
    const size_t gy = group / frame_dim.xsize_groups;
    const Rect rect(gx * frame_dim.group_dim, gy * frame_dim.group_dim,
                    frame_dim.group_dim, frame_dim.group_dim, xsize, ysize);
    for (size_t c = 0; c < num_c; c++) {
      const ImageF& plane =
          c < 3 ? image.color().Plane(c) : image.extra_channels()[c - 3];
      const std::pair<ImageF*, Rect>& buffer = input.GetBuffer(c);
      JXL_RETURN_IF_ERROR(
          CopyImageTo(rect, plane, buffer.second, buffer.first));
    }
    return input.Done();
  };
  return RunOnPool(pool, 0, frame_dim.num_groups, prepare, write_group,
                   "WriteOutputFromImage");
}

}  // namespace jxl
