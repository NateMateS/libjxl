// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "lib/extras/dec/decode.h"

#include <jxl/cms.h>
#include <jxl/codestream_header.h>
#include <jxl/color_encoding.h>
#include <jxl/decode.h>
#include <jxl/decode_cxx.h>
#include <jxl/memory_manager.h>
#include <jxl/parallel_runner.h>
#include <jxl/resizable_parallel_runner.h>
#include <jxl/resizable_parallel_runner_cxx.h>
#include <jxl/thread_parallel_runner.h>
#include <jxl/thread_parallel_runner_cxx.h>
#include <jxl/types.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "lib/extras/dec/color_description.h"
#include "lib/extras/dec/jxl.h"
#include "lib/extras/enc/encode.h"
#include "lib/extras/enc/jpg.h"
#include "lib/extras/enc/jxl.h"
#include "lib/extras/packed_image.h"
#include "lib/extras/preview.h"
#include "lib/jxl/base/byte_order.h"
#include "lib/jxl/base/common.h"
#include "lib/jxl/base/compiler_specific.h"
#include "lib/jxl/base/override.h"
#include "lib/jxl/base/span.h"
#include "lib/jxl/butteraugli/butteraugli.h"
#include "lib/jxl/chroma_from_luma.h"
#include "lib/jxl/cms/color_encoding_cms.h"
#include "lib/jxl/color_encoding_internal.h"
#include "lib/jxl/common.h"  // SpeedTier
#include "lib/jxl/dec_bit_reader.h"
#include "lib/jxl/dec_external_image.h"
#include "lib/jxl/dec_preview_internal.h"
#include "lib/jxl/enc_aux_out.h"
#include "lib/jxl/enc_external_image.h"
#include "lib/jxl/enc_fields.h"
#include "lib/jxl/enc_frame.h"
#include "lib/jxl/enc_icc_codec.h"
#include "lib/jxl/enc_params.h"
#include "lib/jxl/enc_progressive_split.h"
#include "lib/jxl/encode_internal.h"
#include "lib/jxl/fields.h"
#include "lib/jxl/frame_dimensions.h"
#include "lib/jxl/frame_header.h"
#include "lib/jxl/headers.h"
#include "lib/jxl/image.h"
#include "lib/jxl/image_bundle.h"
#include "lib/jxl/image_metadata.h"
#include "lib/jxl/image_ops.h"
#if JPEGXL_ENABLE_BOXES
#include "lib/jxl/box_content_decoder.h"
#endif  // JPEGXL_ENABLE_BOXES
#include "lib/jxl/jpeg/enc_jpeg_data.h"
#include "lib/jxl/jpeg/jpeg_data.h"
#include "lib/jxl/padded_bytes.h"
#include "lib/jxl/splines.h"
#include "lib/jxl/test_image.h"
#include "lib/jxl/test_memory_manager.h"
#include "lib/jxl/test_utils.h"
#include "lib/jxl/testing.h"
#include "lib/jxl/toc.h"
using ::jxl::test::GetIccTestProfile;
////////////////////////////////////////////////////////////////////////////////

namespace {
void AppendU32BE(uint32_t u32, std::vector<uint8_t>* bytes) {
  bytes->push_back(u32 >> 24);
  bytes->push_back(u32 >> 16);
  bytes->push_back(u32 >> 8);
  bytes->push_back(u32 >> 0);
}

// What type of codestream format in the boxes to use for testing
enum CodeStreamBoxFormat {
  // Do not use box format at all, only pure codestream
  kCSBF_None,
  // Have a single codestream box, with its actual size given in the box
  kCSBF_Single,
  // Have a single codestream box, with box size 0 (final box running to end)
  kCSBF_Single_Zero_Terminated,
  // Single codestream box, with another unknown box behind it
  kCSBF_Single_Other,
  // Have multiple partial codestream boxes
  kCSBF_Multi,
  // Have multiple partial codestream boxes, with final box size 0 (running
  // to end)
  kCSBF_Multi_Zero_Terminated,
  // Have multiple partial codestream boxes, terminated by non-codestream box
  kCSBF_Multi_Other_Terminated,
  // Have multiple partial codestream boxes, terminated by non-codestream box
  // that has its size set to 0 (running to end)
  kCSBF_Multi_Other_Zero_Terminated,
  // Have multiple partial codestream boxes, and the first one has a content
  // of zero length
  kCSBF_Multi_First_Empty,
  // Have multiple partial codestream boxes, and the last one has a content
  // of zero length and there is an unknown empty box at the end
  kCSBF_Multi_Last_Empty_Other,
  // Have a compressed exif box before a regular codestream box
  kCSBF_Brob_Exif,
  // Not a value but used for counting amount of enum entries
  kCSBF_NUM_ENTRIES,
};

// Unknown boxes for testing
const char* unk1_box_type = "unk1";
const char* unk1_box_contents = "abcdefghijklmnopqrstuvwxyz";
const size_t unk1_box_size = strlen(unk1_box_contents);
const char* unk2_box_type = "unk2";
const char* unk2_box_contents = "0123456789";
const size_t unk2_box_size = strlen(unk2_box_contents);
const char* unk3_box_type = "unk3";
const char* unk3_box_contents = "ABCDEF123456";
const size_t unk3_box_size = strlen(unk3_box_contents);
// Box with brob-compressed exif, including header
const uint8_t* box_brob_exif = reinterpret_cast<const uint8_t*>(
    "\0\0\0@brobExif\241\350\2\300\177\244v\2525\304\360\27=?\267{"
    "\33\37\314\332\214QX17PT\"\256\0\0\202s\214\313t\333\310\320k\20\276\30"
    "\204\277l$\326c#\1\b");
size_t box_brob_exif_size = 64;
// The uncompressed Exif data from the brob box
const uint8_t* exif_uncompressed = reinterpret_cast<const uint8_t*>(
    "\0\0\0\0MM\0*"
    "\0\0\0\b\0\5\1\22\0\3\0\0\0\1\0\5\0\0\1\32\0\5\0\0\0\1\0\0\0J\1\33\0\5\0\0"
    "\0\1\0\0\0R\1("
    "\0\3\0\0\0\1\0\1\0\0\2\23\0\3\0\0\0\1\0\1\0\0\0\0\0\0\0\0\0\1\0\0\0\1\0\0"
    "\0\1\0\0\0\1");
size_t exif_uncompressed_size = 94;

}  // namespace

namespace jxl {
namespace {

void AppendTestBox(const char* type, const char* contents, size_t contents_size,
                   bool unbounded, std::vector<uint8_t>* bytes) {
  AppendU32BE(contents_size + 8, bytes);
  bytes->push_back(type[0]);
  bytes->push_back(type[1]);
  bytes->push_back(type[2]);
  bytes->push_back(type[3]);
  const uint8_t* contents_u = reinterpret_cast<const uint8_t*>(contents);
  Bytes(contents_u, contents_size).AppendTo(*bytes);
}

enum PreviewMode {
  kNoPreview,
  kSmallPreview,
  kBigPreview,
  kNumPreviewModes,
};

void GeneratePreview(PreviewMode preview_mode, ImageBundle* ib) {
  JxlMemoryManager* memory_manager = jxl::test::MemoryManager();
  if (preview_mode == kSmallPreview) {
    ASSERT_TRUE(ib->ShrinkTo(ib->xsize() / 7, ib->ysize() / 7));
  } else if (preview_mode == kBigPreview) {
    auto upsample7 = [&](const ImageF& in, ImageF* out) {
      for (size_t y = 0; y < out->ysize(); ++y) {
        for (size_t x = 0; x < out->xsize(); ++x) {
          out->Row(y)[x] = in.ConstRow(y / 7)[x / 7];
        }
      }
    };
    JXL_TEST_ASSIGN_OR_DIE(
        Image3F preview,
        Image3F::Create(memory_manager, ib->xsize() * 7, ib->ysize() * 7));
    for (size_t c = 0; c < 3; ++c) {
      upsample7(ib->color()->Plane(c), &preview.Plane(c));
    }
    std::vector<ImageF> extra_channels;
    for (size_t i = 0; i < ib->extra_channels().size(); ++i) {
      JXL_TEST_ASSIGN_OR_DIE(
          ImageF ec,
          ImageF::Create(memory_manager, ib->xsize() * 7, ib->ysize() * 7));
      upsample7(ib->extra_channels()[i], &ec);
      extra_channels.emplace_back(std::move(ec));
    }
    ib->RemoveColor();
    ib->ClearExtraChannels();
    ASSERT_TRUE(ib->SetFromImage(std::move(preview), ib->c_current()));
    ASSERT_TRUE(ib->SetExtraChannels(std::move(extra_channels)));
  }
}

struct TestCodestreamParams {
  CompressParams cparams;
  CodeStreamBoxFormat box_format = kCSBF_None;
  JxlOrientation orientation = JXL_ORIENT_IDENTITY;
  PreviewMode preview_mode = kNoPreview;
  bool add_intrinsic_size = false;
  bool add_icc_profile = false;
  float intensity_target = 0.0;
  std::string color_space;
  std::vector<uint8_t>* jpeg_codestream = nullptr;
};

// Input pixels always given as 16-bit RGBA, 8 bytes per pixel.
// include_alpha determines if the encoded image should contain the alpha
// channel.
// add_icc_profile: if false, encodes the image as sRGB using the JXL fields,
// for grayscale or RGB images. If true, encodes the image using the ICC profile
// returned by GetIccTestProfile, without the JXL fields, this requires the
// image is RGB, not grayscale.
// Providing jpeg_codestream will populate the jpeg_codestream with compressed
// JPEG bytes, and make it possible to reconstruct those exact JPEG bytes using
// the return value _if_ add_container indicates a box format.
std::vector<uint8_t> CreateTestJXLCodestream(
    Span<const uint8_t> pixels, size_t xsize, size_t ysize, size_t num_channels,
    const TestCodestreamParams& params) {
  JxlMemoryManager* memory_manager = jxl::test::MemoryManager();
  // Compress the pixels with JPEG XL.
  bool grayscale = (num_channels <= 2);
  bool have_alpha = ((num_channels & 1) == 0);
  bool include_alpha = have_alpha && params.jpeg_codestream == nullptr;
  size_t bitdepth = params.jpeg_codestream == nullptr ? 16 : 8;
  auto io = jxl::make_unique<CodecInOut>(memory_manager);
  EXPECT_TRUE(io->SetSize(xsize, ysize));
  ColorEncoding color_encoding;
  if (params.add_icc_profile) {
    // the hardcoded ICC profile we attach requires RGB.
    EXPECT_EQ(false, grayscale);
    EXPECT_TRUE(params.color_space.empty());
    EXPECT_TRUE(color_encoding.SetICC(GetIccTestProfile(), JxlGetDefaultCms()));
  } else if (!params.color_space.empty()) {
    JxlColorEncoding c;
    EXPECT_TRUE(jxl::ParseDescription(params.color_space, &c));
    EXPECT_TRUE(color_encoding.FromExternal(c));
    EXPECT_EQ(color_encoding.IsGray(), grayscale);
  } else {
    color_encoding = jxl::ColorEncoding::SRGB(/*is_gray=*/grayscale);
  }
  io->metadata.m.SetUintSamples(bitdepth);
  if (include_alpha) {
    io->metadata.m.SetAlphaBits(bitdepth);
  }
  if (params.intensity_target != 0) {
    io->metadata.m.SetIntensityTarget(params.intensity_target);
  }
  JxlPixelFormat format = {static_cast<uint32_t>(num_channels), JXL_TYPE_UINT16,
                           JXL_BIG_ENDIAN, 0};
  // Make the grayscale-ness of the io metadata color_encoding and the packed
  // image match.
  io->metadata.m.color_encoding = color_encoding;
  EXPECT_TRUE(ConvertFromExternal(pixels, xsize, ysize, color_encoding,
                                  /*bits_per_sample=*/16, format,
                                  /* pool */ nullptr, &io->Main(),
                                  include_alpha));
  std::vector<uint8_t> encoded_jpeg_bytes;
  if (params.jpeg_codestream != nullptr) {
    if (jxl::extras::CanDecode(jxl::extras::Codec::kJPG)) {
      std::vector<uint8_t> jpeg_bytes;
      extras::PackedPixelFile ppf;
      JXL_TEST_ASSIGN_OR_DIE(extras::PackedFrame frame,
                             extras::PackedFrame::Create(xsize, ysize, format));
      EXPECT_TRUE(frame.color.pixels_size == pixels.size());
      memcpy(frame.color.pixels(0, 0, 0), pixels.data(), pixels.size());
      ppf.frames.emplace_back(std::move(frame));
      ppf.info.xsize = xsize;
      ppf.info.ysize = ysize;
      ppf.info.num_color_channels = grayscale ? 1 : 3;
      ppf.info.bits_per_sample = 16;
      auto encoder = extras::GetJPEGEncoder();
      encoder->SetOption("q", "70");
      extras::EncodedImage encoded;
      EXPECT_TRUE(encoder->Encode(ppf, &encoded, nullptr));
      jpeg_bytes = encoded.bitstreams[0];
      Bytes(jpeg_bytes).AppendTo(*params.jpeg_codestream);
      JXL_TEST_ASSIGN_OR_DIE(
          std::unique_ptr<jxl::jpeg::JPEGData> jpeg_data,
          jxl::jpeg::ParseJPG(memory_manager, jxl::Bytes(jpeg_bytes)));
      EXPECT_TRUE(
          jxl::test::JpegDataToCodecInOut(std::move(jpeg_data), io.get()));
      EXPECT_TRUE(EncodeJPEGData(memory_manager, *io->Main().jpeg_data,
                                 &encoded_jpeg_bytes, params.cparams));
      io->metadata.m.xyb_encoded = false;
    } else {
      ADD_FAILURE();
    }
  }
  if (params.preview_mode) {
    JXL_TEST_ASSIGN_OR_DIE(io->preview_frame, io->Main().Copy());
    GeneratePreview(params.preview_mode, &io->preview_frame);
    io->metadata.m.have_preview = true;
    EXPECT_TRUE(io->metadata.m.preview_size.Set(io->preview_frame.xsize(),
                                                io->preview_frame.ysize()));
  }
  if (params.add_intrinsic_size) {
    EXPECT_TRUE(io->metadata.m.intrinsic_size.Set(xsize / 3, ysize / 3));
  }
  io->metadata.m.orientation = params.orientation;
  std::vector<uint8_t> compressed;
  EXPECT_TRUE(test::EncodeFile(params.cparams, io.get(), &compressed));
  CodeStreamBoxFormat add_container = params.box_format;
  if (add_container != kCSBF_None) {
    // Header with signature box and ftyp box.
    const uint8_t header[] = {0,    0,    0,    0xc,  0x4a, 0x58, 0x4c, 0x20,
                              0xd,  0xa,  0x87, 0xa,  0,    0,    0,    0x14,
                              0x66, 0x74, 0x79, 0x70, 0x6a, 0x78, 0x6c, 0x20,
                              0,    0,    0,    0,    0x6a, 0x78, 0x6c, 0x20};

    bool is_multi = add_container == kCSBF_Multi ||
                    add_container == kCSBF_Multi_Zero_Terminated ||
                    add_container == kCSBF_Multi_Other_Terminated ||
                    add_container == kCSBF_Multi_Other_Zero_Terminated ||
                    add_container == kCSBF_Multi_First_Empty ||
                    add_container == kCSBF_Multi_Last_Empty_Other;

    if (is_multi) {
      size_t third = compressed.size() / 3;
      std::vector<uint8_t> compressed0(compressed.data(),
                                       compressed.data() + third);
      std::vector<uint8_t> compressed1(compressed.data() + third,
                                       compressed.data() + 2 * third);
      std::vector<uint8_t> compressed2(compressed.data() + 2 * third,
                                       compressed.data() + compressed.size());

      std::vector<uint8_t> c;
      Bytes(header).AppendTo(c);
      if (params.jpeg_codestream != nullptr) {
        jxl::AppendBoxHeader(jxl::MakeBoxType("jbrd"),
                             encoded_jpeg_bytes.size(), false, &c);
        Bytes(encoded_jpeg_bytes).AppendTo(c);
      }
      uint32_t jxlp_index = 0;
      if (add_container == kCSBF_Multi_First_Empty) {
        // Empty placeholder codestream part
        AppendU32BE(12, &c);
        c.push_back('j');
        c.push_back('x');
        c.push_back('l');
        c.push_back('p');
        AppendU32BE(jxlp_index++, &c);
      }
      // First codestream part
      AppendU32BE(compressed0.size() + 12, &c);
      c.push_back('j');
      c.push_back('x');
      c.push_back('l');
      c.push_back('p');
      AppendU32BE(jxlp_index++, &c);
      Bytes(compressed0).AppendTo(c);
      // A few non-codestream boxes in between
      AppendTestBox(unk1_box_type, unk1_box_contents, unk1_box_size, false, &c);
      AppendTestBox(unk2_box_type, unk2_box_contents, unk2_box_size, false, &c);
      // Empty placeholder codestream part
      AppendU32BE(12, &c);
      c.push_back('j');
      c.push_back('x');
      c.push_back('l');
      c.push_back('p');
      AppendU32BE(jxlp_index++, &c);
      // Second codestream part
      AppendU32BE(compressed1.size() + 12, &c);
      c.push_back('j');
      c.push_back('x');
      c.push_back('l');
      c.push_back('p');
      AppendU32BE(jxlp_index++, &c);
      Bytes(compressed1).AppendTo(c);
      // Third (last) codestream part
      AppendU32BE(add_container == kCSBF_Multi_Zero_Terminated
                      ? 0
                      : (compressed2.size() + 12),
                  &c);
      c.push_back('j');
      c.push_back('x');
      c.push_back('l');
      c.push_back('p');
      if (add_container != kCSBF_Multi_Last_Empty_Other) {
        AppendU32BE(jxlp_index++ | 0x80000000, &c);
      } else {
        AppendU32BE(jxlp_index++, &c);
      }
      Bytes(compressed2).AppendTo(c);
      if (add_container == kCSBF_Multi_Last_Empty_Other) {
        // Empty placeholder codestream part
        AppendU32BE(12, &c);
        c.push_back('j');
        c.push_back('x');
        c.push_back('l');
        c.push_back('p');
        AppendU32BE(jxlp_index++ | 0x80000000, &c);
        AppendTestBox(unk3_box_type, unk3_box_contents, unk3_box_size, false,
                      &c);
      }
      if (add_container == kCSBF_Multi_Other_Terminated) {
        AppendTestBox(unk3_box_type, unk3_box_contents, unk3_box_size, false,
                      &c);
      }
      if (add_container == kCSBF_Multi_Other_Zero_Terminated) {
        AppendTestBox(unk3_box_type, unk3_box_contents, unk3_box_size, true,
                      &c);
      }
      compressed.swap(c);
    } else {
      std::vector<uint8_t> c;
      Bytes(header).AppendTo(c);
      if (params.jpeg_codestream != nullptr) {
        jxl::AppendBoxHeader(jxl::MakeBoxType("jbrd"),
                             encoded_jpeg_bytes.size(), false, &c);
        Bytes(encoded_jpeg_bytes).AppendTo(c);
      }
      if (add_container == kCSBF_Brob_Exif) {
        Bytes(box_brob_exif, box_brob_exif_size).AppendTo(c);
      }
      AppendU32BE(add_container == kCSBF_Single_Zero_Terminated
                      ? 0
                      : (compressed.size() + 8),
                  &c);
      c.push_back('j');
      c.push_back('x');
      c.push_back('l');
      c.push_back('c');
      Bytes(compressed).AppendTo(c);
      if (add_container == kCSBF_Single_Other) {
        AppendTestBox(unk1_box_type, unk1_box_contents, unk1_box_size, false,
                      &c);
      }
      compressed.swap(c);
    }
  }

  return compressed;
}

JxlDecoderStatus ProcessInputIgnoreBoxes(JxlDecoder* dec) {
  JxlDecoderStatus status = JXL_DEC_BOX;
  while (status == JXL_DEC_BOX) {
    status = JxlDecoderProcessInput(dec);
  }
  return status;
}

// Decodes one-shot with the API for non-streaming decoding tests.
std::vector<uint8_t> DecodeWithAPI(JxlDecoder* dec,
                                   Span<const uint8_t> compressed,
                                   const JxlPixelFormat& format,
                                   bool use_callback, bool set_buffer_early,
                                   bool use_resizable_runner,
                                   bool require_boxes, bool expect_success,
                                   std::vector<uint8_t>* icc = nullptr) {
  JxlThreadParallelRunnerPtr runner_fixed;
  JxlResizableParallelRunnerPtr runner_resizable;
  JxlParallelRunner runner_fn;
  void* runner;

  if (use_resizable_runner) {
    runner_resizable = JxlResizableParallelRunnerMake(nullptr);
    runner = runner_resizable.get();
    runner_fn = JxlResizableParallelRunner;
  } else {
    size_t hw_threads = JxlThreadParallelRunnerDefaultNumWorkerThreads();
    runner_fixed =
        JxlThreadParallelRunnerMake(nullptr, std::min<size_t>(hw_threads, 16));
    runner = runner_fixed.get();
    runner_fn = JxlThreadParallelRunner;
  }
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSetParallelRunner(dec, runner_fn, runner));

  auto process_input =
      require_boxes ? ProcessInputIgnoreBoxes : JxlDecoderProcessInput;

  EXPECT_EQ(
      JXL_DEC_SUCCESS,
      JxlDecoderSubscribeEvents(
          dec, JXL_DEC_BASIC_INFO | (set_buffer_early ? JXL_DEC_FRAME : 0) |
                   JXL_DEC_PREVIEW_IMAGE | JXL_DEC_FULL_IMAGE |
                   (require_boxes ? JXL_DEC_BOX : 0) |
                   (icc != nullptr ? JXL_DEC_COLOR_ENCODING : 0)));

  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSetInput(dec, compressed.data(), compressed.size()));
  EXPECT_EQ(JXL_DEC_BASIC_INFO, process_input(dec));
  size_t buffer_size;
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderImageOutBufferSize(dec, &format, &buffer_size));
  JxlBasicInfo info;
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBasicInfo(dec, &info));
  if (use_resizable_runner) {
    JxlResizableParallelRunnerSetThreads(
        runner,
        JxlResizableParallelRunnerSuggestThreads(info.xsize, info.ysize));
  }

  std::vector<uint8_t> pixels(buffer_size);
  size_t bytes_per_pixel = format.num_channels *
                           test::GetDataBits(format.data_type) /
                           jxl::kBitsPerByte;
  size_t stride = bytes_per_pixel * info.xsize;
  EXPECT_TRUE(SafeRoundUpTo(stride, format.align, stride));
  auto callback = [&](size_t x, size_t y, size_t num_pixels,
                      const void* pixels_row) {
    memcpy(pixels.data() + stride * y + bytes_per_pixel * x, pixels_row,
           num_pixels * bytes_per_pixel);
  };

  JxlDecoderStatus status = process_input(dec);

  if (status == JXL_DEC_COLOR_ENCODING) {
    size_t icc_size = 0;
    EXPECT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderGetICCProfileSize(dec, JXL_COLOR_PROFILE_TARGET_DATA,
                                          &icc_size));
    icc->resize(icc_size);
    EXPECT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderGetColorAsICCProfile(dec, JXL_COLOR_PROFILE_TARGET_DATA,
                                             icc->data(), icc_size));

    status = process_input(dec);
  }

  std::vector<uint8_t> preview;
  if (status == JXL_DEC_NEED_PREVIEW_OUT_BUFFER) {
    size_t preview_buffer_size;
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderPreviewOutBufferSize(
                                   dec, &format, &preview_buffer_size));
    preview.resize(preview_buffer_size);
    EXPECT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderSetPreviewOutBuffer(dec, &format, preview.data(),
                                            preview.size()));
    EXPECT_EQ(JXL_DEC_PREVIEW_IMAGE, process_input(dec));

    status = process_input(dec);
  }

  if (set_buffer_early) {
    EXPECT_EQ(JXL_DEC_FRAME, status);
  } else {
    EXPECT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER, status);
  }

  if (use_callback) {
    EXPECT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderSetImageOutCallback(
                  dec, &format,
                  [](void* opaque, size_t x, size_t y, size_t xsize,
                     const void* pixels_row) {
                    auto cb = static_cast<decltype(&callback)>(opaque);
                    (*cb)(x, y, xsize, pixels_row);
                  },
                  /*opaque=*/&callback));
  } else {
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetImageOutBuffer(
                                   dec, &format, pixels.data(), pixels.size()));
  }

  EXPECT_EQ(JXL_DEC_FULL_IMAGE, process_input(dec));

  // After the full image was output, JxlDecoderProcessInput should return
  // success to indicate all is done, unless we requested boxes and the last
  // box was not a terminal unbounded box, in which case it should ask for
  // more input.
  JxlDecoderStatus expected_status =
      expect_success ? JXL_DEC_SUCCESS : JXL_DEC_NEED_MORE_INPUT;
  EXPECT_EQ(expected_status, process_input(dec));

  return pixels;
}

// Decodes one-shot with the API for non-streaming decoding tests.
std::vector<uint8_t> DecodeWithAPI(Span<const uint8_t> compressed,
                                   const JxlPixelFormat& format,
                                   bool use_callback, bool set_buffer_early,
                                   bool use_resizable_runner,
                                   bool require_boxes, bool expect_success) {
  JxlDecoder* dec = JxlDecoderCreate(nullptr);
  std::vector<uint8_t> pixels =
      DecodeWithAPI(dec, compressed, format, use_callback, set_buffer_early,
                    use_resizable_runner, require_boxes, expect_success);
  JxlDecoderDestroy(dec);
  return pixels;
}

}  // namespace
}  // namespace jxl

////////////////////////////////////////////////////////////////////////////////

using ::jxl::Image3F;
using ::jxl::ImageF;
using ::jxl::test::BoolToCStr;
using ::jxl::test::ButteraugliDistance;

TEST(DecodeTest, JxlSignatureCheckTest) {
  std::vector<std::pair<int, std::vector<uint8_t>>> tests = {
      // No JPEGXL header starts with 'a'.
      {JXL_SIG_INVALID, {'a'}},
      {JXL_SIG_INVALID, {'a', 'b', 'c', 'd', 'e', 'f'}},

      // Empty file is not enough bytes.
      {JXL_SIG_NOT_ENOUGH_BYTES, {}},

      // JPEGXL headers.
      {JXL_SIG_NOT_ENOUGH_BYTES, {0xff}},  // Part of a signature.
      {JXL_SIG_INVALID, {0xff, 0xD8}},     // JPEG-1
      {JXL_SIG_CODESTREAM, {0xff, 0x0a}},

      // JPEGXL container file.
      {JXL_SIG_CONTAINER,
       {0, 0, 0, 0xc, 'J', 'X', 'L', ' ', 0xD, 0xA, 0x87, 0xA}},
      // Ending with invalid byte.
      {JXL_SIG_INVALID, {0, 0, 0, 0xc, 'J', 'X', 'L', ' ', 0xD, 0xA, 0x87, 0}},
      // Part of signature.
      {JXL_SIG_NOT_ENOUGH_BYTES,
       {0, 0, 0, 0xc, 'J', 'X', 'L', ' ', 0xD, 0xA, 0x87}},
      {JXL_SIG_NOT_ENOUGH_BYTES, {0}},
  };
  for (const auto& test : tests) {
    EXPECT_EQ(test.first,
              JxlSignatureCheck(test.second.data(), test.second.size()))
        << "Where test data is " << ::testing::PrintToString(test.second);
  }
}

TEST(DecodeTest, DefaultAllocTest) {
  JxlDecoder* dec = JxlDecoderCreate(nullptr);
  EXPECT_NE(nullptr, dec);
  JxlDecoderDestroy(dec);
}

TEST(DecodeTest, CustomAllocTest) {
  struct CalledCounters {
    int allocs = 0;
    int frees = 0;
  } counters;

  JxlMemoryManager mm;
  mm.opaque = &counters;
  mm.alloc = [](void* opaque, size_t size) {
    reinterpret_cast<CalledCounters*>(opaque)->allocs++;
    return malloc(size);
  };
  mm.free = [](void* opaque, void* address) {
    reinterpret_cast<CalledCounters*>(opaque)->frees++;
    free(address);
  };

  JxlDecoder* dec = JxlDecoderCreate(&mm);
  EXPECT_NE(nullptr, dec);
  EXPECT_LE(1, counters.allocs);
  EXPECT_EQ(0, counters.frees);
  JxlDecoderDestroy(dec);
  EXPECT_LE(1, counters.frees);
}

// TODO(lode): add multi-threaded test when multithreaded pixel decoding from
// API is implemented.
TEST(DecodeTest, DefaultParallelRunnerTest) {
  JxlDecoder* dec = JxlDecoderCreate(nullptr);
  EXPECT_NE(nullptr, dec);
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSetParallelRunner(dec, nullptr, nullptr));
  JxlDecoderDestroy(dec);
}

// Creates the header of a JPEG XL file with various custom parameters for
// testing.
// xsize, ysize: image dimensions to store in the SizeHeader, max 512.
// bits_per_sample, orientation: a selection of header parameters to test with.
// orientation: image orientation to set in the metadata
// alpha_bits: if non-0, alpha extra channel bits to set in the metadata. Also
//   gives the alpha channel the name "alpha_test"
// have_container: add box container format around the codestream.
// metadata_default: if true, ImageMetadata is set to default and
//   bits_per_sample, orientation and alpha_bits are ignored.
// insert_box: insert an extra box before the codestream box, making the header
// farther away from the front than is ideal. Only used if have_container.
std::vector<uint8_t> GetTestHeader(size_t xsize, size_t ysize,
                                   size_t bits_per_sample, size_t orientation,
                                   size_t alpha_bits, bool xyb_encoded,
                                   bool have_container, bool metadata_default,
                                   bool insert_extra_box,
                                   const jxl::IccBytes& icc_profile) {
  JxlMemoryManager* memory_manager = jxl::test::MemoryManager();
  jxl::BitWriter writer{memory_manager};
  EXPECT_TRUE(writer.WithMaxBits(
      65536,  // Large enough
      jxl::LayerType::Header, nullptr, [&] {
        if (have_container) {
          const std::vector<uint8_t> signature_box = {
              0, 0, 0, 0xc, 'J', 'X', 'L', ' ', 0xd, 0xa, 0x87, 0xa};
          const std::vector<uint8_t> filetype_box = {
              0,   0,   0, 0x14, 'f', 't', 'y', 'p', 'j', 'x',
              'l', ' ', 0, 0,    0,   0,   'j', 'x', 'l', ' '};
          const std::vector<uint8_t> extra_box_header = {0,   0,   0,   0xff,
                                                         't', 'e', 's', 't'};
          // Beginning of codestream box, with an arbitrary size certainly large
          // enough to contain the header
          const std::vector<uint8_t> codestream_box_header = {
              0, 0, 0, 0xff, 'j', 'x', 'l', 'c'};

          for (uint8_t c : signature_box) {
            writer.Write(8, c);
          }
          for (uint8_t c : filetype_box) {
            writer.Write(8, c);
          }
          if (insert_extra_box) {
            for (uint8_t c : extra_box_header) {
              writer.Write(8, c);
            }
            for (size_t i = 0; i < 255 - 8; i++) {
              writer.Write(8, 0);
            }
          }
          for (uint8_t c : codestream_box_header) {
            writer.Write(8, c);
          }
        }

        // JXL signature
        writer.Write(8, 0xff);
        writer.Write(8, 0x0a);

        // SizeHeader
        auto metadata = jxl::make_unique<jxl::CodecMetadata>();
        EXPECT_TRUE(metadata->size.Set(xsize, ysize));
        EXPECT_TRUE(WriteSizeHeader(metadata->size, &writer,
                                    jxl::LayerType::Header, nullptr));

        if (!metadata_default) {
          metadata->m.SetUintSamples(bits_per_sample);
          metadata->m.orientation = orientation;
          metadata->m.SetAlphaBits(alpha_bits);
          metadata->m.xyb_encoded = xyb_encoded;
          if (alpha_bits != 0) {
            metadata->m.extra_channel_info[0].name = "alpha_test";
          }
        }

        if (!icc_profile.empty()) {
          jxl::IccBytes copy = icc_profile;
          EXPECT_TRUE(metadata->m.color_encoding.SetICC(std::move(copy),
                                                        JxlGetDefaultCms()));
        }

        EXPECT_TRUE(jxl::Bundle::Write(metadata->m, &writer,
                                       jxl::LayerType::Header, nullptr));
        metadata->transform_data.nonserialized_xyb_encoded =
            metadata->m.xyb_encoded;
        EXPECT_TRUE(jxl::Bundle::Write(metadata->transform_data, &writer,
                                       jxl::LayerType::Header, nullptr));

        if (!icc_profile.empty()) {
          EXPECT_TRUE(metadata->m.color_encoding.WantICC());
          EXPECT_TRUE(jxl::WriteICC(jxl::Span<const uint8_t>(icc_profile),
                                    &writer, jxl::LayerType::Header, nullptr));
        }

        writer.ZeroPadToByte();
        return true;
      }));
  jxl::Bytes bytes = writer.GetSpan();
  return std::vector<uint8_t>(bytes.data(), bytes.data() + bytes.size());
}

TEST(DecodeTest, BasicInfoTest) {
  size_t xsize[2] = {50, 33};
  size_t ysize[2] = {50, 77};
  size_t bits_per_sample[2] = {8, 23};
  size_t orientation[2] = {3, 5};
  size_t alpha_bits[2] = {0, 8};
  bool have_container[2] = {false, true};
  bool xyb_encoded = false;

  std::vector<std::vector<uint8_t>> test_samples;
  // Test with direct codestream
  test_samples.push_back(GetTestHeader(
      xsize[0], ysize[0], bits_per_sample[0], orientation[0], alpha_bits[0],
      xyb_encoded, have_container[0], /*metadata_default=*/false,
      /*insert_extra_box=*/false, {}));
  // Test with container and different parameters
  test_samples.push_back(GetTestHeader(
      xsize[1], ysize[1], bits_per_sample[1], orientation[1], alpha_bits[1],
      xyb_encoded, have_container[1], /*metadata_default=*/false,
      /*insert_extra_box=*/false, {}));

  for (size_t i = 0; i < test_samples.size(); ++i) {
    const std::vector<uint8_t>& data = test_samples[i];
    // Test decoding too small header first, until we reach the final byte.
    for (size_t size = 0; size <= data.size(); ++size) {
      // Test with a new decoder for each tested byte size.
      JxlDecoder* dec = JxlDecoderCreate(nullptr);
      EXPECT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO));
      const uint8_t* next_in = data.data();
      size_t avail_in = size;
      EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, next_in, avail_in));
      JxlDecoderStatus status = JxlDecoderProcessInput(dec);

      JxlBasicInfo info;
      JxlDecoderStatus bi_status = JxlDecoderGetBasicInfo(dec, &info);
      bool have_basic_info = (bi_status == JXL_DEC_SUCCESS);

      if (size == data.size()) {
        EXPECT_EQ(JXL_DEC_BASIC_INFO, status);

        // All header bytes given so the decoder must have the basic info.
        EXPECT_EQ(true, have_basic_info);
        EXPECT_EQ(have_container[i], FROM_JXL_BOOL(info.have_container));
        EXPECT_EQ(alpha_bits[i], info.alpha_bits);
        // Orientations 5..8 swap the dimensions
        if (orientation[i] >= 5) {
          EXPECT_EQ(xsize[i], info.ysize);
          EXPECT_EQ(ysize[i], info.xsize);
        } else {
          EXPECT_EQ(xsize[i], info.xsize);
          EXPECT_EQ(ysize[i], info.ysize);
        }
        // The API should set the orientation to identity by default since it
        // already applies the transformation internally by default.
        EXPECT_EQ(1u, info.orientation);

        EXPECT_EQ(3u, info.num_color_channels);

        if (alpha_bits[i] != 0) {
          // Expect an extra channel
          EXPECT_EQ(1u, info.num_extra_channels);
          JxlExtraChannelInfo extra;
          EXPECT_EQ(0, JxlDecoderGetExtraChannelInfo(dec, 0, &extra));
          EXPECT_EQ(alpha_bits[i], extra.bits_per_sample);
          EXPECT_EQ(JXL_CHANNEL_ALPHA, extra.type);
          EXPECT_EQ(0, extra.alpha_premultiplied);
          // Verify the name "alpha_test" given to the alpha channel
          EXPECT_EQ(10u, extra.name_length);
          char name[11];
          EXPECT_EQ(0,
                    JxlDecoderGetExtraChannelName(dec, 0, name, sizeof(name)));
          EXPECT_EQ(std::string("alpha_test"), std::string(name));
        } else {
          EXPECT_EQ(0u, info.num_extra_channels);
        }

        EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderProcessInput(dec));
      } else {
        // If we did not give the full header, the basic info should not be
        // available. Allow a few bytes of slack due to some bits for default
        // opsinmatrix/extension bits.
        if (size + 2 < data.size()) {
          EXPECT_EQ(false, have_basic_info);
          EXPECT_EQ(JXL_DEC_NEED_MORE_INPUT, status);
        }
      }

      // Test that decoder doesn't allow setting a setting required at beginning
      // unless it's reset
      EXPECT_EQ(JXL_DEC_ERROR,
                JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO));
      JxlDecoderReset(dec);
      EXPECT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO));

      JxlDecoderDestroy(dec);
    }
  }
}

TEST(DecodeTest, BufferSizeTest) {
  size_t xsize = 33;
  size_t ysize = 77;
  size_t bits_per_sample = 8;
  size_t orientation = 1;
  size_t alpha_bits = 8;
  bool have_container = false;
  bool xyb_encoded = false;

  std::vector<uint8_t> header =
      GetTestHeader(xsize, ysize, bits_per_sample, orientation, alpha_bits,
                    xyb_encoded, have_container, /*metadata_default=*/false,
                    /*insert_extra_box=*/false, {});

  JxlDecoder* dec = JxlDecoderCreate(nullptr);
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO));
  const uint8_t* next_in = header.data();
  size_t avail_in = header.size();
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, next_in, avail_in));
  JxlDecoderStatus status = JxlDecoderProcessInput(dec);
  EXPECT_EQ(JXL_DEC_BASIC_INFO, status);

  JxlBasicInfo info;
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBasicInfo(dec, &info));
  EXPECT_EQ(xsize, info.xsize);
  EXPECT_EQ(ysize, info.ysize);

  JxlPixelFormat format = {4, JXL_TYPE_UINT8, JXL_LITTLE_ENDIAN, 0};
  size_t image_out_size;
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderImageOutBufferSize(dec, &format, &image_out_size));
  EXPECT_EQ(xsize * ysize * 4, image_out_size);

  JxlDecoderDestroy(dec);
}

TEST(DecodeTest, BasicInfoSizeHintTest) {
  // Test on a file where the size hint is too small initially due to inserting
  // a box before the codestream (something that is normally not recommended)
  size_t xsize = 50;
  size_t ysize = 50;
  size_t bits_per_sample = 16;
  size_t orientation = 1;
  size_t alpha_bits = 0;
  bool xyb_encoded = false;
  std::vector<uint8_t> data = GetTestHeader(
      xsize, ysize, bits_per_sample, orientation, alpha_bits, xyb_encoded,
      /*have_container=*/true, /*metadata_default=*/false,
      /*insert_extra_box=*/true, {});

  JxlDecoderStatus status;
  JxlDecoder* dec = JxlDecoderCreate(nullptr);
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO));

  size_t hint0 = JxlDecoderSizeHintBasicInfo(dec);
  // Test that the test works as intended: we construct a file on purpose to
  // be larger than the first hint by having that extra box.
  EXPECT_LT(hint0, data.size());
  const uint8_t* next_in = data.data();
  // Do as if we have only as many bytes as indicated by the hint available
  size_t avail_in = std::min(hint0, data.size());
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, next_in, avail_in));
  status = JxlDecoderProcessInput(dec);
  EXPECT_EQ(JXL_DEC_NEED_MORE_INPUT, status);
  // Basic info cannot be available yet due to the extra inserted box.
  EXPECT_EQ(false, !JxlDecoderGetBasicInfo(dec, nullptr));

  size_t num_read = avail_in - JxlDecoderReleaseInput(dec);
  EXPECT_LT(num_read, data.size());

  size_t hint1 = JxlDecoderSizeHintBasicInfo(dec);
  // The hint must be larger than the previous hint (taking already processed
  // bytes into account, the hint is a hint for the next avail_in) since the
  // decoder now knows there is a box in between.
  EXPECT_GT(hint1 + num_read, hint0);
  avail_in = std::min<size_t>(hint1, data.size() - num_read);
  next_in += num_read;

  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, next_in, avail_in));
  status = JxlDecoderProcessInput(dec);
  EXPECT_EQ(JXL_DEC_BASIC_INFO, status);
  JxlBasicInfo info;
  // We should have the basic info now, since we only added one box in-between,
  // and the decoder should have known its size, its implementation can return
  // a correct hint.
  EXPECT_EQ(true, !JxlDecoderGetBasicInfo(dec, &info));

  // Also test if the basic info is correct.
  EXPECT_EQ(1, info.have_container);
  EXPECT_EQ(xsize, info.xsize);
  EXPECT_EQ(ysize, info.ysize);
  EXPECT_EQ(orientation, info.orientation);
  EXPECT_EQ(bits_per_sample, info.bits_per_sample);

  JxlDecoderDestroy(dec);
}

std::vector<uint8_t> GetIccTestHeader(const jxl::IccBytes& icc_profile,
                                      bool xyb_encoded) {
  size_t xsize = 50;
  size_t ysize = 50;
  size_t bits_per_sample = 16;
  size_t orientation = 1;
  size_t alpha_bits = 0;
  return GetTestHeader(xsize, ysize, bits_per_sample, orientation, alpha_bits,
                       xyb_encoded,
                       /*have_container=*/false, /*metadata_default=*/false,
                       /*insert_extra_box=*/false, icc_profile);
}

// Tests the case where pixels and metadata ICC profile are the same
TEST(DecodeTest, IccProfileTestOriginal) {
  jxl::IccBytes icc_profile = GetIccTestProfile();
  bool xyb_encoded = false;
  std::vector<uint8_t> data = GetIccTestHeader(icc_profile, xyb_encoded);

  JxlDecoder* dec = JxlDecoderCreate(nullptr);
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSubscribeEvents(
                dec, JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING));
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, data.data(), data.size()));

  EXPECT_EQ(JXL_DEC_BASIC_INFO, JxlDecoderProcessInput(dec));

  // Expect the opposite of xyb_encoded for uses_original_profile
  JxlBasicInfo info;
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBasicInfo(dec, &info));
  EXPECT_EQ(JXL_TRUE, info.uses_original_profile);

  EXPECT_EQ(JXL_DEC_COLOR_ENCODING, JxlDecoderProcessInput(dec));

  // the encoded color profile expected to be not available, since the image
  // has an ICC profile instead
  EXPECT_EQ(JXL_DEC_ERROR,
            JxlDecoderGetColorAsEncodedProfile(
                dec, JXL_COLOR_PROFILE_TARGET_ORIGINAL, nullptr));

  size_t dec_profile_size;
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderGetICCProfileSize(dec, JXL_COLOR_PROFILE_TARGET_ORIGINAL,
                                        &dec_profile_size));

  // Check that can get return status with NULL size
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderGetICCProfileSize(dec, JXL_COLOR_PROFILE_TARGET_ORIGINAL,
                                        nullptr));

  // The profiles must be equal. This requires they have equal size, and if
  // they do, we can get the profile and compare the contents.
  EXPECT_EQ(icc_profile.size(), dec_profile_size);
  if (icc_profile.size() == dec_profile_size) {
    jxl::IccBytes icc_profile2(icc_profile.size());
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetColorAsICCProfile(
                                   dec, JXL_COLOR_PROFILE_TARGET_ORIGINAL,
                                   icc_profile2.data(), icc_profile2.size()));
    EXPECT_EQ(icc_profile, icc_profile2);
  }

  // the data is not xyb_encoded, so same result expected for the pixel data
  // color profile
  EXPECT_EQ(JXL_DEC_ERROR, JxlDecoderGetColorAsEncodedProfile(
                               dec, JXL_COLOR_PROFILE_TARGET_DATA, nullptr));

  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderGetICCProfileSize(dec, JXL_COLOR_PROFILE_TARGET_DATA,
                                        &dec_profile_size));
  EXPECT_EQ(icc_profile.size(), dec_profile_size);

  JxlDecoderDestroy(dec);
}

// Tests the case where pixels and metadata ICC profile are different
TEST(DecodeTest, IccProfileTestXybEncoded) {
  jxl::IccBytes icc_profile = GetIccTestProfile();
  bool xyb_encoded = true;
  std::vector<uint8_t> data = GetIccTestHeader(icc_profile, xyb_encoded);

  JxlDecoder* dec = JxlDecoderCreate(nullptr);
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSubscribeEvents(
                dec, JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING));

  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, data.data(), data.size()));
  EXPECT_EQ(JXL_DEC_BASIC_INFO, JxlDecoderProcessInput(dec));

  // Expect the opposite of xyb_encoded for uses_original_profile
  JxlBasicInfo info;
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBasicInfo(dec, &info));
  EXPECT_EQ(JXL_FALSE, info.uses_original_profile);

  EXPECT_EQ(JXL_DEC_COLOR_ENCODING, JxlDecoderProcessInput(dec));

  // the encoded color profile expected to be not available, since the image
  // has an ICC profile instead
  EXPECT_EQ(JXL_DEC_ERROR,
            JxlDecoderGetColorAsEncodedProfile(
                dec, JXL_COLOR_PROFILE_TARGET_ORIGINAL, nullptr));

  // Check that can get return status with NULL size
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderGetICCProfileSize(dec, JXL_COLOR_PROFILE_TARGET_ORIGINAL,
                                        nullptr));

  size_t dec_profile_size;
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderGetICCProfileSize(dec, JXL_COLOR_PROFILE_TARGET_ORIGINAL,
                                        &dec_profile_size));

  // The profiles must be equal. This requires they have equal size, and if
  // they do, we can get the profile and compare the contents.
  EXPECT_EQ(icc_profile.size(), dec_profile_size);
  if (icc_profile.size() == dec_profile_size) {
    jxl::IccBytes icc_profile2(icc_profile.size());
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetColorAsICCProfile(
                                   dec, JXL_COLOR_PROFILE_TARGET_ORIGINAL,
                                   icc_profile2.data(), icc_profile2.size()));
    EXPECT_EQ(icc_profile, icc_profile2);
  }

  // Data is xyb_encoded, so the data profile is a different profile, encoded
  // as structured profile.
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetColorAsEncodedProfile(
                                 dec, JXL_COLOR_PROFILE_TARGET_DATA, nullptr));
  JxlColorEncoding pixel_encoding;
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderGetColorAsEncodedProfile(
                dec, JXL_COLOR_PROFILE_TARGET_DATA, &pixel_encoding));
  EXPECT_EQ(JXL_PRIMARIES_SRGB, pixel_encoding.primaries);
  // The API returns LINEAR by default when the colorspace cannot be represented
  // by enum values.
  EXPECT_EQ(JXL_TRANSFER_FUNCTION_LINEAR, pixel_encoding.transfer_function);

  // Test the same but with integer format.
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderGetColorAsEncodedProfile(
                dec, JXL_COLOR_PROFILE_TARGET_DATA, &pixel_encoding));
  EXPECT_EQ(JXL_PRIMARIES_SRGB, pixel_encoding.primaries);
  EXPECT_EQ(JXL_TRANSFER_FUNCTION_LINEAR, pixel_encoding.transfer_function);

  // Test after setting the preferred color profile to non-linear sRGB:
  // for XYB images with ICC profile, this setting is expected to take effect.
  jxl::ColorEncoding temp_jxl_srgb = jxl::ColorEncoding::SRGB(false);
  JxlColorEncoding pixel_encoding_srgb = temp_jxl_srgb.ToExternal();
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSetPreferredColorProfile(dec, &pixel_encoding_srgb));
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderGetColorAsEncodedProfile(
                dec, JXL_COLOR_PROFILE_TARGET_DATA, &pixel_encoding));
  EXPECT_EQ(JXL_TRANSFER_FUNCTION_SRGB, pixel_encoding.transfer_function);

  // The decoder can also output this as a generated ICC profile anyway, and
  // we're certain that it will differ from the above defined profile since
  // the sRGB data should not have swapped R/G/B primaries.

  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderGetICCProfileSize(dec, JXL_COLOR_PROFILE_TARGET_DATA,
                                        &dec_profile_size));
  // We don't need to dictate exactly what size the generated ICC profile
  // must be (since there are many ways to represent the same color space),
  // but it should not be zero.
  EXPECT_NE(0u, dec_profile_size);
  jxl::IccBytes icc_profile2(dec_profile_size);
  if (0 != dec_profile_size) {
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetColorAsICCProfile(
                                   dec, JXL_COLOR_PROFILE_TARGET_DATA,
                                   icc_profile2.data(), icc_profile2.size()));
    // expected not equal
    EXPECT_NE(icc_profile, icc_profile2);
  }

  // Test setting another different preferred profile, to verify that the
  // returned JXL_COLOR_PROFILE_TARGET_DATA ICC profile is correctly
  // updated.

  jxl::ColorEncoding temp_jxl_linear = jxl::ColorEncoding::LinearSRGB(false);
  JxlColorEncoding pixel_encoding_linear = temp_jxl_linear.ToExternal();

  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSetPreferredColorProfile(dec, &pixel_encoding_linear));
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderGetColorAsEncodedProfile(
                dec, JXL_COLOR_PROFILE_TARGET_DATA, &pixel_encoding));
  EXPECT_EQ(JXL_TRANSFER_FUNCTION_LINEAR, pixel_encoding.transfer_function);
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderGetICCProfileSize(dec, JXL_COLOR_PROFILE_TARGET_DATA,
                                        &dec_profile_size));
  EXPECT_NE(0u, dec_profile_size);
  jxl::IccBytes icc_profile3(dec_profile_size);
  if (0 != dec_profile_size) {
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetColorAsICCProfile(
                                   dec, JXL_COLOR_PROFILE_TARGET_DATA,
                                   icc_profile3.data(), icc_profile3.size()));
    // expected not equal to the previously set preferred profile.
    EXPECT_NE(icc_profile2, icc_profile3);
  }

  JxlDecoderDestroy(dec);
}

// Test decoding ICC from partial files byte for byte.
// This test must pass also if JXL_CRASH_ON_ERROR is enabled, that is, the
// decoding of the ANS histogram and stream of the encoded ICC profile must also
// handle the case of not enough input bytes with StatusCode::kNotEnoughBytes
// rather than fatal error status codes.
TEST(DecodeTest, ICCPartialTest) {
  jxl::IccBytes icc_profile = GetIccTestProfile();
  std::vector<uint8_t> data = GetIccTestHeader(icc_profile, false);

  const uint8_t* next_in = data.data();
  size_t avail_in = 0;

  JxlDecoder* dec = JxlDecoderCreate(nullptr);

  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSubscribeEvents(
                dec, JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING));

  bool seen_basic_info = false;
  bool seen_color_encoding = false;
  size_t total_size = 0;

  for (;;) {
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, next_in, avail_in));
    JxlDecoderStatus status = JxlDecoderProcessInput(dec);
    size_t remaining = JxlDecoderReleaseInput(dec);
    EXPECT_LE(remaining, avail_in);
    next_in += avail_in - remaining;
    avail_in = remaining;
    if (status == JXL_DEC_NEED_MORE_INPUT) {
      if (total_size >= data.size()) {
        // End of partial codestream with codestrema headers and ICC profile
        // reached, it should not require more input since full image is not
        // requested
        FAIL();
        break;
      }
      size_t increment = 1;
      if (total_size + increment > data.size()) {
        increment = data.size() - total_size;
      }
      total_size += increment;
      avail_in += increment;
    } else if (status == JXL_DEC_BASIC_INFO) {
      EXPECT_FALSE(seen_basic_info);
      seen_basic_info = true;
    } else if (status == JXL_DEC_COLOR_ENCODING) {
      EXPECT_TRUE(seen_basic_info);
      EXPECT_FALSE(seen_color_encoding);
      seen_color_encoding = true;

      // Sanity check that the ICC profile was decoded correctly
      size_t dec_profile_size;
      EXPECT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderGetICCProfileSize(
                    dec, JXL_COLOR_PROFILE_TARGET_ORIGINAL, &dec_profile_size));
      EXPECT_EQ(icc_profile.size(), dec_profile_size);

    } else if (status == JXL_DEC_SUCCESS) {
      EXPECT_TRUE(seen_color_encoding);
      break;
    } else {
      // We do not expect any other events or errors
      FAIL();
      break;
    }
  }

  EXPECT_TRUE(seen_basic_info);
  EXPECT_TRUE(seen_color_encoding);

  JxlDecoderDestroy(dec);
}

struct PixelTestConfig {
  // Input image definition.
  bool grayscale;
  bool include_alpha;
  size_t xsize;
  size_t ysize;
  jxl::PreviewMode preview_mode;
  bool add_intrinsic_size;
  // Output format.
  JxlEndianness endianness;
  JxlDataType data_type;
  uint32_t output_channels;
  // Container options.
  CodeStreamBoxFormat add_container;
  // Decoding mode.
  bool use_callback;
  bool set_buffer_early;
  bool use_resizable_runner;
  // Exif orientation, 1-8
  JxlOrientation orientation;
  bool keep_orientation;
  size_t upsampling;
};

class DecodeTestParam : public ::testing::TestWithParam<PixelTestConfig> {};

TEST_P(DecodeTestParam, PixelTest) {
  PixelTestConfig config = GetParam();
  JxlMemoryManager* memory_manager = jxl::test::MemoryManager();
  JxlDecoder* dec = JxlDecoderCreate(nullptr);

  if (config.keep_orientation) {
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetKeepOrientation(dec, JXL_TRUE));
  }

  size_t num_pixels = config.xsize * config.ysize;
  uint32_t orig_channels =
      (config.grayscale ? 1 : 3) + (config.include_alpha ? 1 : 0);
  std::vector<uint8_t> pixels =
      jxl::test::GetSomeTestImage(config.xsize, config.ysize, orig_channels, 0);
  JxlPixelFormat format_orig = {orig_channels, JXL_TYPE_UINT16, JXL_BIG_ENDIAN,
                                0};
  jxl::TestCodestreamParams params;
  // Lossless to verify pixels exactly after roundtrip.
  params.cparams.SetLossless();
  params.cparams.speed_tier = jxl::SpeedTier::kThunder;
  params.cparams.resampling = config.upsampling;
  params.cparams.ec_resampling = config.upsampling;
  params.box_format = config.add_container;
  params.orientation = config.orientation;
  params.preview_mode = config.preview_mode;
  params.add_intrinsic_size = config.add_intrinsic_size;
  std::vector<uint8_t> compressed = jxl::CreateTestJXLCodestream(
      jxl::Bytes(pixels.data(), pixels.size()), config.xsize, config.ysize,
      orig_channels, params);

  JxlPixelFormat format = {config.output_channels, config.data_type,
                           config.endianness, 0};

  bool swap_xy = !config.keep_orientation && (config.orientation > 4);
  size_t xsize = swap_xy ? config.ysize : config.xsize;
  size_t ysize = swap_xy ? config.xsize : config.ysize;

  std::vector<uint8_t> pixels2 =
      jxl::DecodeWithAPI(dec, jxl::Bytes(compressed.data(), compressed.size()),
                         format, config.use_callback, config.set_buffer_early,
                         config.use_resizable_runner, /*require_boxes=*/false,
                         /*expect_success=*/true);
  JxlDecoderReset(dec);
  EXPECT_EQ(num_pixels * config.output_channels *
                jxl::test::GetDataBits(config.data_type) / jxl::kBitsPerByte,
            pixels2.size());

  // If an orientation transformation is expected, to compare the pixels, also
  // apply this transformation to the original pixels. ConvertToExternal is
  // used to achieve this, with a temporary conversion to CodecInOut and back.
  if (config.orientation > 1 && !config.keep_orientation) {
    jxl::Span<const uint8_t> bytes(pixels.data(), pixels.size());
    jxl::ColorEncoding color_encoding =
        jxl::ColorEncoding::SRGB(config.grayscale);

    auto io = jxl::make_unique<jxl::CodecInOut>(memory_manager);
    if (config.include_alpha) io->metadata.m.SetAlphaBits(16);
    io->metadata.m.color_encoding = color_encoding;
    ASSERT_TRUE(io->SetSize(config.xsize, config.ysize));

    EXPECT_TRUE(ConvertFromExternal(bytes, config.xsize, config.ysize,
                                    color_encoding, 16, format_orig, nullptr,
                                    &io->Main(), config.include_alpha));

    for (uint8_t& pixel : pixels) pixel = 0;
    EXPECT_TRUE(ConvertToExternal(
        io->Main(), 16,
        /*float_out=*/false, orig_channels, JXL_BIG_ENDIAN,
        xsize * 2 * orig_channels, nullptr, pixels.data(), pixels.size(),
        /*out_callback=*/{},
        static_cast<jxl::Orientation>(config.orientation)));
  }
  if (config.upsampling == 1) {
    EXPECT_EQ(0u, jxl::test::ComparePixels(pixels.data(), pixels2.data(), xsize,
                                           ysize, format_orig, format));
  } else {
    // resampling is of course not lossless, so as a rough check:
    // count pixels that are more than off-by-25 in the 8-bit value of one of
    // the channels
    EXPECT_LE(
        jxl::test::ComparePixels(
            pixels.data(), pixels2.data(), xsize, ysize, format_orig, format,
            50.0 * (config.data_type == JXL_TYPE_UINT8 ? 1.0 : 256.0)),
        300u);
  }

  JxlDecoderDestroy(dec);
}

std::vector<PixelTestConfig> GeneratePixelTests() {
  std::vector<PixelTestConfig> all_tests;
  struct ChannelInfo {
    bool grayscale;
    bool include_alpha;
    size_t output_channels;
  };
  ChannelInfo ch_info[] = {
      {false, true, 4},   // RGBA -> RGBA
      {true, false, 1},   // G -> G
      {true, true, 1},    // GA -> G
      {true, true, 2},    // GA -> GA
      {false, false, 3},  // RGB -> RGB
      {false, true, 3},   // RGBA -> RGB
      {false, false, 4},  // RGB -> RGBA
  };

  struct OutputFormat {
    JxlEndianness endianness;
    JxlDataType data_type;
  };
  OutputFormat out_formats[] = {
      {JXL_NATIVE_ENDIAN, JXL_TYPE_UINT8},
      {JXL_LITTLE_ENDIAN, JXL_TYPE_UINT16},
      {JXL_BIG_ENDIAN, JXL_TYPE_UINT16},
      {JXL_NATIVE_ENDIAN, JXL_TYPE_FLOAT16},
      {JXL_LITTLE_ENDIAN, JXL_TYPE_FLOAT},
      {JXL_BIG_ENDIAN, JXL_TYPE_FLOAT},
  };

  auto make_test = [&](ChannelInfo ch, size_t xsize, size_t ysize,
                       jxl::PreviewMode preview_mode, bool intrinsic_size,
                       CodeStreamBoxFormat box, JxlOrientation orientation,
                       bool keep_orientation, OutputFormat format,
                       bool use_callback, bool set_buffer_early,
                       bool resizable_runner, size_t upsampling) {
    PixelTestConfig c;
    c.grayscale = ch.grayscale;
    c.include_alpha = ch.include_alpha;
    c.preview_mode = preview_mode;
    c.add_intrinsic_size = intrinsic_size;
    c.xsize = xsize;
    c.ysize = ysize;
    c.add_container = box;
    c.output_channels = ch.output_channels;
    c.data_type = format.data_type;
    c.endianness = format.endianness;
    c.use_callback = use_callback;
    c.set_buffer_early = set_buffer_early;
    c.use_resizable_runner = resizable_runner;
    c.orientation = orientation;
    c.keep_orientation = keep_orientation;
    c.upsampling = upsampling;
    all_tests.push_back(c);
  };

  // Test output formats and methods.
  for (ChannelInfo ch : ch_info) {
    for (bool use_callback : {false, true}) {
      for (size_t upsampling : {1, 2, 4, 8}) {
        for (OutputFormat fmt : out_formats) {
          make_test(ch, 301, 33, jxl::kNoPreview,
                    /*add_intrinsic_size=*/false,
                    CodeStreamBoxFormat::kCSBF_None, JXL_ORIENT_IDENTITY,
                    /*keep_orientation=*/false, fmt, use_callback,
                    /*set_buffer_early=*/false, /*resizable_runner=*/false,
                    upsampling);
        }
      }
    }
  }
  // Test codestream formats.
  for (size_t box = 1; box < kCSBF_NUM_ENTRIES; ++box) {
    make_test(ch_info[0], 77, 33, jxl::kNoPreview,
              /*add_intrinsic_size=*/false,
              static_cast<CodeStreamBoxFormat>(box), JXL_ORIENT_IDENTITY,
              /*keep_orientation=*/false, out_formats[0],
              /*use_callback=*/false,
              /*set_buffer_early=*/false, /*resizable_runner=*/false, 1);
  }
  // Test previews.
  for (int preview_mode = 0; preview_mode < jxl::kNumPreviewModes;
       preview_mode++) {
    make_test(ch_info[0], 77, 33, static_cast<jxl::PreviewMode>(preview_mode),
              /*add_intrinsic_size=*/false, CodeStreamBoxFormat::kCSBF_None,
              JXL_ORIENT_IDENTITY,
              /*keep_orientation=*/false, out_formats[0],
              /*use_callback=*/false, /*set_buffer_early=*/false,
              /*resizable_runner=*/false, 1);
  }
  // Test intrinsic sizes.
  for (bool add_intrinsic_size : {false, true}) {
    make_test(ch_info[0], 55, 34, jxl::kNoPreview, add_intrinsic_size,
              CodeStreamBoxFormat::kCSBF_None, JXL_ORIENT_IDENTITY,
              /*keep_orientation=*/false, out_formats[0],
              /*use_callback=*/false, /*set_buffer_early=*/false,
              /*resizable_runner=*/false, 1);
  }
  // Test setting buffers early.
  make_test(ch_info[0], 300, 33, jxl::kNoPreview,
            /*add_intrinsic_size=*/false, CodeStreamBoxFormat::kCSBF_None,
            JXL_ORIENT_IDENTITY,
            /*keep_orientation=*/false, out_formats[0],
            /*use_callback=*/false, /*set_buffer_early=*/true,
            /*resizable_runner=*/false, 1);

  // Test using the resizable runner
  for (size_t i = 0; i < 4; i++) {
    make_test(ch_info[0], 300 << i, 33 << i, jxl::kNoPreview,
              /*add_intrinsic_size=*/false, CodeStreamBoxFormat::kCSBF_None,
              JXL_ORIENT_IDENTITY,
              /*keep_orientation=*/false, out_formats[0],
              /*use_callback=*/false, /*set_buffer_early=*/false,
              /*resizable_runner=*/true, 1);
  }

  // Test orientations.
  for (int orientation = 2; orientation <= 8; ++orientation) {
    for (bool keep_orientation : {false, true}) {
      for (bool use_callback : {false, true}) {
        for (ChannelInfo ch : ch_info) {
          for (OutputFormat fmt : out_formats) {
            make_test(ch, 280, 12, jxl::kNoPreview,
                      /*add_intrinsic_size=*/false,
                      CodeStreamBoxFormat::kCSBF_None,
                      static_cast<JxlOrientation>(orientation),
                      /*keep_orientation=*/keep_orientation, fmt,
                      /*use_callback=*/use_callback, /*set_buffer_early=*/true,
                      /*resizable_runner=*/false, 1);
          }
        }
      }
    }
  }

  return all_tests;
}

std::ostream& operator<<(std::ostream& os, const PixelTestConfig& c) {
  os << c.xsize << "x" << c.ysize;
  const char* colors[] = {"", "G", "GA", "RGB", "RGBA"};
  os << colors[(c.grayscale ? 1 : 3) + (c.include_alpha ? 1 : 0)];
  os << "to";
  os << colors[c.output_channels];
  switch (c.data_type) {
    case JXL_TYPE_UINT8:
      os << "u8";
      break;
    case JXL_TYPE_UINT16:
      os << "u16";
      break;
    case JXL_TYPE_FLOAT:
      os << "f32";
      break;
    case JXL_TYPE_FLOAT16:
      os << "f16";
      break;
    default:
      ADD_FAILURE();
  };
  if (jxl::test::GetDataBits(c.data_type) > jxl::kBitsPerByte) {
    if (c.endianness == JXL_NATIVE_ENDIAN) {
      // add nothing
    } else if (c.endianness == JXL_BIG_ENDIAN) {
      os << "BE";
    } else if (c.endianness == JXL_LITTLE_ENDIAN) {
      os << "LE";
    }
  }
  if (c.add_container != CodeStreamBoxFormat::kCSBF_None) {
    os << "Box";
    os << static_cast<size_t>(c.add_container);
  }
  if (c.preview_mode == jxl::kSmallPreview) os << "Preview";
  if (c.preview_mode == jxl::kBigPreview) os << "BigPreview";
  if (c.add_intrinsic_size) os << "IntrinsicSize";
  if (c.use_callback) os << "Callback";
  if (c.set_buffer_early) os << "EarlyBuffer";
  if (c.use_resizable_runner) os << "ResizableRunner";
  if (c.orientation != 1) os << "O" << c.orientation;
  if (c.keep_orientation) os << "Keep";
  if (c.upsampling > 1) os << "x" << c.upsampling;
  return os;
}

std::string PixelTestDescription(
    const testing::TestParamInfo<DecodeTestParam::ParamType>& info) {
  std::stringstream name;
  name << info.param;
  return name.str();
}

JXL_GTEST_INSTANTIATE_TEST_SUITE_P(DecodeTest, DecodeTestParam,
                                   testing::ValuesIn(GeneratePixelTests()),
                                   PixelTestDescription);

TEST(DecodeTest, PixelTestWithICCProfileLossless) {
  JxlDecoder* dec = JxlDecoderCreate(nullptr);

  size_t xsize = 123;
  size_t ysize = 77;
  size_t num_pixels = xsize * ysize;
  std::vector<uint8_t> pixels = jxl::test::GetSomeTestImage(xsize, ysize, 4, 0);
  JxlPixelFormat format_orig = {4, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};
  jxl::TestCodestreamParams params;
  // Lossless to verify pixels exactly after roundtrip.
  params.cparams.SetLossless();
  params.cparams.speed_tier = jxl::SpeedTier::kThunder;
  params.add_icc_profile = true;
  // For variation: some have container and no preview, others have preview
  // and no container.
  std::vector<uint8_t> compressed = jxl::CreateTestJXLCodestream(
      jxl::Bytes(pixels.data(), pixels.size()), xsize, ysize, 4, params);

  for (uint32_t channels = 3; channels <= 4; ++channels) {
    {
      JxlPixelFormat format = {channels, JXL_TYPE_UINT8, JXL_LITTLE_ENDIAN, 0};

      std::vector<uint8_t> pixels2 = jxl::DecodeWithAPI(
          dec, jxl::Bytes(compressed.data(), compressed.size()), format,
          /*use_callback=*/false, /*set_buffer_early=*/false,
          /*use_resizable_runner=*/false, /*require_boxes=*/false,
          /*expect_success=*/true);
      JxlDecoderReset(dec);
      EXPECT_EQ(num_pixels * channels, pixels2.size());
      EXPECT_EQ(0u,
                jxl::test::ComparePixels(pixels.data(), pixels2.data(), xsize,
                                         ysize, format_orig, format));
    }
    {
      JxlPixelFormat format = {channels, JXL_TYPE_UINT16, JXL_LITTLE_ENDIAN, 0};

      // Test with the container for one of the pixel formats.
      std::vector<uint8_t> pixels2 = jxl::DecodeWithAPI(
          dec, jxl::Bytes(compressed.data(), compressed.size()), format,
          /*use_callback=*/true, /*set_buffer_early=*/true,
          /*use_resizable_runner=*/false, /*require_boxes=*/false,
          /*expect_success=*/true);
      JxlDecoderReset(dec);
      EXPECT_EQ(num_pixels * channels * 2, pixels2.size());
      EXPECT_EQ(0u,
                jxl::test::ComparePixels(pixels.data(), pixels2.data(), xsize,
                                         ysize, format_orig, format));
    }

    {
      JxlPixelFormat format = {channels, JXL_TYPE_FLOAT, JXL_LITTLE_ENDIAN, 0};

      std::vector<uint8_t> pixels2 = jxl::DecodeWithAPI(
          dec, jxl::Bytes(compressed.data(), compressed.size()), format,
          /*use_callback=*/false, /*set_buffer_early=*/false,
          /*use_resizable_runner=*/false, /*require_boxes=*/false,
          /*expect_success=*/true);
      JxlDecoderReset(dec);
      EXPECT_EQ(num_pixels * channels * 4, pixels2.size());
      EXPECT_EQ(0u,
                jxl::test::ComparePixels(pixels.data(), pixels2.data(), xsize,
                                         ysize, format_orig, format));
    }
  }

  JxlDecoderDestroy(dec);
}

TEST(DecodeTest, PixelTestWithICCProfileLossy) {
  JxlMemoryManager* memory_manager = jxl::test::MemoryManager();
  JxlDecoder* dec = JxlDecoderCreate(nullptr);

  size_t xsize = 123;
  size_t ysize = 77;
  size_t num_pixels = xsize * ysize;
  std::vector<uint8_t> pixels = jxl::test::GetSomeTestImage(xsize, ysize, 3, 0);
  JxlPixelFormat format_orig = {3, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};
  jxl::TestCodestreamParams params;
  params.add_icc_profile = true;
  std::vector<uint8_t> compressed = jxl::CreateTestJXLCodestream(
      jxl::Bytes(pixels.data(), pixels.size()), xsize, ysize, 3, params);
  uint32_t channels = 3;

  JxlPixelFormat format = {channels, JXL_TYPE_FLOAT, JXL_LITTLE_ENDIAN, 0};

  std::vector<uint8_t> icc_data;
  std::vector<uint8_t> pixels2 = jxl::DecodeWithAPI(
      dec, jxl::Bytes(compressed.data(), compressed.size()), format,
      /*use_callback=*/false, /*set_buffer_early=*/true,
      /*use_resizable_runner=*/false, /*require_boxes=*/false,
      /*expect_success=*/true, /*icc=*/&icc_data);
  JxlDecoderReset(dec);
  EXPECT_EQ(num_pixels * channels * 4, pixels2.size());

  // The input pixels use the profile matching GetIccTestProfile, since we set
  // add_icc_profile for CreateTestJXLCodestream to true.
  jxl::ColorEncoding color_encoding0;
  EXPECT_TRUE(color_encoding0.SetICC(GetIccTestProfile(), JxlGetDefaultCms()));
  jxl::Span<const uint8_t> span0(pixels.data(), pixels.size());
  auto io0 = jxl::make_unique<jxl::CodecInOut>(memory_manager);
  ASSERT_TRUE(io0->SetSize(xsize, ysize));
  EXPECT_TRUE(ConvertFromExternal(span0, xsize, ysize, color_encoding0,
                                  /*bits_per_sample=*/16, format_orig,
                                  /*pool=*/nullptr, &io0->Main()));

  jxl::ColorEncoding color_encoding1;
  jxl::IccBytes icc;
  jxl::Bytes(icc_data).AppendTo(icc);
  EXPECT_TRUE(color_encoding1.SetICC(std::move(icc), JxlGetDefaultCms()));
  jxl::Span<const uint8_t> span1(pixels2.data(), pixels2.size());
  auto io1 = jxl::make_unique<jxl::CodecInOut>(memory_manager);
  ASSERT_TRUE(io1->SetSize(xsize, ysize));
  EXPECT_TRUE(ConvertFromExternal(span1, xsize, ysize, color_encoding1,
                                  /*bits_per_sample=*/32, format,
                                  /*pool=*/nullptr, &io1->Main()));

  jxl::ButteraugliParams butteraugli_params;
  EXPECT_SLIGHTLY_BELOW(
      ButteraugliDistance(io0->frames, io1->frames, butteraugli_params,
                          *JxlGetDefaultCms(),
                          /*distmap=*/nullptr, nullptr),
      1.14f);

  JxlDecoderDestroy(dec);
}

std::string ColorDescription(JxlColorEncoding c) {
  jxl::ColorEncoding color_encoding;
  EXPECT_TRUE(color_encoding.FromExternal(c));
  return Description(color_encoding);
}

std::string GetOrigProfile(JxlDecoder* dec) {
  JxlColorEncoding c;
  JxlColorProfileTarget target = JXL_COLOR_PROFILE_TARGET_ORIGINAL;
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderGetColorAsEncodedProfile(dec, target, &c));
  return ColorDescription(c);
}

std::string GetDataProfile(JxlDecoder* dec) {
  JxlColorEncoding c;
  JxlColorProfileTarget target = JXL_COLOR_PROFILE_TARGET_DATA;
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderGetColorAsEncodedProfile(dec, target, &c));
  return ColorDescription(c);
}

double ButteraugliDistance(size_t xsize, size_t ysize,
                           const std::vector<uint8_t>& pixels_in,
                           const jxl::ColorEncoding& color_in,
                           float intensity_in,
                           const std::vector<uint8_t>& pixels_out,
                           const jxl::ColorEncoding& color_out,
                           float intensity_out) {
  JxlMemoryManager* memory_manager = jxl::test::MemoryManager();
  auto in = jxl::make_unique<jxl::CodecInOut>(memory_manager);
  in->metadata.m.color_encoding = color_in;
  in->metadata.m.SetIntensityTarget(intensity_in);
  JxlPixelFormat format_in = {static_cast<uint32_t>(color_in.Channels()),
                              JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};
  EXPECT_TRUE(jxl::ConvertFromExternal(jxl::Bytes(pixels_in), xsize, ysize,
                                       color_in,
                                       /*bits_per_sample=*/16, format_in,
                                       /*pool=*/nullptr, &in->Main()));
  auto out = jxl::make_unique<jxl::CodecInOut>(memory_manager);
  out->metadata.m.color_encoding = color_out;
  out->metadata.m.SetIntensityTarget(intensity_out);
  JxlPixelFormat format_out = {static_cast<uint32_t>(color_out.Channels()),
                               JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};
  EXPECT_TRUE(jxl::ConvertFromExternal(jxl::Bytes(pixels_out), xsize, ysize,
                                       color_out,
                                       /*bits_per_sample=*/16, format_out,
                                       /*pool=*/nullptr, &out->Main()));
  return ButteraugliDistance(in->frames, out->frames, jxl::ButteraugliParams(),
                             *JxlGetDefaultCms(), nullptr, nullptr);
}

class DecodeAllEncodingsTest
    : public ::testing::TestWithParam<jxl::test::ColorEncodingDescriptor> {};
JXL_GTEST_INSTANTIATE_TEST_SUITE_P(
    DecodeAllEncodingsTestInstantiation, DecodeAllEncodingsTest,
    ::testing::ValuesIn(jxl::test::AllEncodings()));
TEST_P(DecodeAllEncodingsTest, PreserveOriginalProfileTest) {
  size_t xsize = 123;
  size_t ysize = 77;
  std::vector<uint8_t> pixels = jxl::test::GetSomeTestImage(xsize, ysize, 3, 0);
  JxlPixelFormat format = {3, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};
  int events = JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE;
  const auto& cdesc = GetParam();
  jxl::ColorEncoding c_in = jxl::test::ColorEncodingFromDescriptor(cdesc);
  if (c_in.GetRenderingIntent() != jxl::RenderingIntent::kRelative) return;
  std::string color_space_in = Description(c_in);
  float intensity_in = c_in.Tf().IsPQ() ? 10000 : 255;
  printf("Testing input color space %s\n", color_space_in.c_str());
  jxl::TestCodestreamParams params;
  params.color_space = color_space_in;
  params.intensity_target = intensity_in;
  std::vector<uint8_t> data = jxl::CreateTestJXLCodestream(
      jxl::Bytes(pixels.data(), pixels.size()), xsize, ysize, 3, params);
  JxlDecoder* dec = JxlDecoderCreate(nullptr);
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSubscribeEvents(dec, events));
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, data.data(), data.size()));
  EXPECT_EQ(JXL_DEC_BASIC_INFO, JxlDecoderProcessInput(dec));
  JxlBasicInfo info;
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBasicInfo(dec, &info));
  EXPECT_EQ(xsize, info.xsize);
  EXPECT_EQ(ysize, info.ysize);
  EXPECT_FALSE(info.uses_original_profile);
  EXPECT_EQ(JXL_DEC_COLOR_ENCODING, JxlDecoderProcessInput(dec));
  EXPECT_EQ(GetOrigProfile(dec), color_space_in);
  EXPECT_EQ(GetDataProfile(dec), color_space_in);
  EXPECT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER, JxlDecoderProcessInput(dec));
  std::vector<uint8_t> out(pixels.size());
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSetImageOutBuffer(dec, &format, out.data(), out.size()));
  EXPECT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec));
  double dist = ButteraugliDistance(xsize, ysize, pixels, c_in, intensity_in,
                                    out, c_in, intensity_in);
  EXPECT_LT(dist, 1.55);
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderProcessInput(dec));
  JxlDecoderDestroy(dec);
}

namespace {
void SetPreferredColorProfileTest(
    const jxl::test::ColorEncodingDescriptor& from, bool icc_dst,
    bool use_cms) {
  size_t xsize = 123;
  size_t ysize = 77;
  int events = JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE;
  jxl::ColorEncoding c_in = jxl::test::ColorEncodingFromDescriptor(from);
  if (c_in.GetRenderingIntent() != jxl::RenderingIntent::kRelative) return;
  if (c_in.GetWhitePointType() != jxl::WhitePoint::kD65) return;
  uint32_t num_channels = c_in.Channels();
  std::vector<uint8_t> pixels =
      jxl::test::GetSomeTestImage(xsize, ysize, num_channels, 0);

  JxlPixelFormat format = {num_channels, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};
  std::string color_space_in = Description(c_in);
  float intensity_in = c_in.Tf().IsPQ() ? 10000 : 255;
  jxl::TestCodestreamParams params;
  params.color_space = color_space_in;
  params.intensity_target = intensity_in;
  std::vector<uint8_t> data =
      jxl::CreateTestJXLCodestream(jxl::Bytes(pixels.data(), pixels.size()),
                                   xsize, ysize, num_channels, params);
  auto all_encodings = jxl::test::AllEncodings();
  // TODO(firsching): understand why XYB does not work together with icc_dst.
  // TODO(jon): fix XYB output space in general
  /*
  if (!icc_dst) {
    all_encodings.push_back(
        {jxl::ColorSpace::kXYB, jxl::WhitePoint::kD65, jxl::Primaries::kCustom,
         jxl::TransferFunction::kUnknown, jxl::RenderingIntent::kPerceptual});
  }
  */
  for (const auto& c1 : all_encodings) {
    jxl::ColorEncoding c_out = jxl::test::ColorEncodingFromDescriptor(c1);
    float intensity_out = intensity_in;
    if (c_out.GetColorSpace() != jxl::ColorSpace::kXYB) {
      if (c_out.GetRenderingIntent() != jxl::RenderingIntent::kRelative) {
        continue;
      }
      if ((c_in.GetPrimariesType() == jxl::Primaries::k2100 &&
           c_out.GetPrimariesType() != jxl::Primaries::k2100) ||
          (c_in.GetPrimariesType() == jxl::Primaries::kP3 &&
           c_out.GetPrimariesType() == jxl::Primaries::kSRGB)) {
        // Converting to a narrower gamut does not work without gamut mapping.
        continue;
      }
    }
    if (c_out.Tf().IsHLG() && intensity_out > 300) {
      // The Linear->HLG OOTF function at this intensity level can push
      // saturated colors out of gamut, so we would need gamut mapping in
      // this case too.
      continue;
    }
    std::string color_space_out = Description(c_out);
    if (color_space_in == color_space_out) continue;
    printf("Testing input color space %s with output color space %s\n",
           color_space_in.c_str(), color_space_out.c_str());
    JxlDecoder* dec = JxlDecoderCreate(nullptr);
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSubscribeEvents(dec, events));
    EXPECT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderSetInput(dec, data.data(), data.size()));
    EXPECT_EQ(JXL_DEC_BASIC_INFO, JxlDecoderProcessInput(dec));
    JxlBasicInfo info;
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBasicInfo(dec, &info));
    EXPECT_EQ(xsize, info.xsize);
    EXPECT_EQ(ysize, info.ysize);
    EXPECT_FALSE(info.uses_original_profile);
    EXPECT_EQ(JXL_DEC_COLOR_ENCODING, JxlDecoderProcessInput(dec));
    EXPECT_EQ(GetOrigProfile(dec), color_space_in);
    JxlColorEncoding encoding_out;
    EXPECT_TRUE(jxl::ParseDescription(color_space_out, &encoding_out));
    if (c_out.GetColorSpace() == jxl::ColorSpace::kXYB &&
        (c_in.GetPrimariesType() != jxl::Primaries::kSRGB ||
         c_in.Tf().IsPQ())) {
      EXPECT_EQ(JXL_DEC_ERROR,
                JxlDecoderSetPreferredColorProfile(dec, &encoding_out));
      JxlDecoderDestroy(dec);
      continue;
    }
    if (use_cms) {
      JxlDecoderSetCms(dec, *JxlGetDefaultCms());
    }
    if (icc_dst) {
      jxl::ColorEncoding internal_encoding_out;
      EXPECT_TRUE(internal_encoding_out.FromExternal(encoding_out));
      EXPECT_TRUE(internal_encoding_out.CreateICC());
      std::vector<uint8_t> rewritten_icc = internal_encoding_out.ICC();

      EXPECT_EQ(use_cms ? JXL_DEC_SUCCESS : JXL_DEC_ERROR,
                JxlDecoderSetOutputColorProfile(
                    dec, nullptr, rewritten_icc.data(), rewritten_icc.size()));
      if (!use_cms) {
        // continue if we don't have a cms here
        JxlDecoderDestroy(dec);
        continue;
      }
    } else {
      EXPECT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderSetPreferredColorProfile(dec, &encoding_out));
    }
    EXPECT_EQ(GetOrigProfile(dec), color_space_in);
    if (icc_dst) {
    } else {
      EXPECT_EQ(GetDataProfile(dec), color_space_out);
    }
    EXPECT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER, JxlDecoderProcessInput(dec));
    size_t buffer_size;
    JxlPixelFormat out_format = format;
    out_format.num_channels = c_out.Channels();
    EXPECT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderImageOutBufferSize(dec, &out_format, &buffer_size));
    std::vector<uint8_t> out(buffer_size);
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetImageOutBuffer(
                                   dec, &out_format, out.data(), out.size()));
    EXPECT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec));
    double dist = ButteraugliDistance(xsize, ysize, pixels, c_in, intensity_in,
                                      out, c_out, intensity_out);

    if (c_in.GetWhitePointType() == c_out.GetWhitePointType()) {
      EXPECT_LT(dist, 1.50);
    } else {
      EXPECT_LT(dist, 4.5);
    }
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderProcessInput(dec));
    JxlDecoderDestroy(dec);
  }
}
}  // namespace

TEST(DecodeTest, SetPreferredColorProfileTestFromGray) {
  jxl::test::ColorEncodingDescriptor gray = {
      jxl::ColorSpace::kGray, jxl::WhitePoint::kD65, jxl::Primaries::kSRGB,
      jxl::TransferFunction::kSRGB, jxl::RenderingIntent::kRelative};
  SetPreferredColorProfileTest(gray, true, true);
  SetPreferredColorProfileTest(gray, false, true);
  SetPreferredColorProfileTest(gray, true, false);
  SetPreferredColorProfileTest(gray, false, false);
}

static std::string DecodeAllEncodingsVariantsTestName(
    const ::testing::TestParamInfo<
        std::tuple<jxl::test::ColorEncodingDescriptor, bool, bool>>& info) {
  const auto& encoding = std::get<0>(info.param);
  bool icc_dst = std::get<1>(info.param);
  bool use_cms = std::get<2>(info.param);

  std::string encoding_name =
      Description(ColorEncodingFromDescriptor(encoding));

  return "From_" + encoding_name +
         (icc_dst ? "_with_icc_dst" : "_without_icc_dst") +
         (use_cms ? "_with_cms" : "_without_cms");
}

class DecodeAllEncodingsVariantsTest
    : public ::testing::TestWithParam<
          std::tuple<jxl::test::ColorEncodingDescriptor, bool, bool>> {};
JXL_GTEST_INSTANTIATE_TEST_SUITE_P(
    DecodeAllEncodingsVariantsTestInstantiation, DecodeAllEncodingsVariantsTest,
    ::testing::Combine(::testing::ValuesIn(jxl::test::AllEncodings()),
                       ::testing::Bool(), ::testing::Bool()),
    DecodeAllEncodingsVariantsTestName);
TEST_P(DecodeAllEncodingsVariantsTest, SetPreferredColorProfileTest) {
  const auto& from = std::get<0>(GetParam());
  bool icc_dst = std::get<1>(GetParam());
  bool use_cms = std::get<2>(GetParam());
  SetPreferredColorProfileTest(from, icc_dst, use_cms);
}

void DecodeImageWithColorEncoding(const std::vector<uint8_t>& compressed,
                                  jxl::ColorEncoding& color_encoding,
                                  bool with_cms, std::vector<uint8_t>& out,
                                  JxlBasicInfo& info) {
  JxlDecoder* dec = JxlDecoderCreate(nullptr);
  int events = JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE;
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSubscribeEvents(dec, events));
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSetInput(dec, compressed.data(), compressed.size()));
  EXPECT_EQ(JXL_DEC_BASIC_INFO, JxlDecoderProcessInput(dec));
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBasicInfo(dec, &info));
  EXPECT_EQ(JXL_DEC_COLOR_ENCODING, JxlDecoderProcessInput(dec));
  if (with_cms) {
    JxlDecoderSetCms(dec, *JxlGetDefaultCms());
    EXPECT_TRUE(color_encoding.CreateICC());
    std::vector<uint8_t> rewritten_icc = color_encoding.ICC();
    EXPECT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderSetOutputColorProfile(
                  dec, nullptr, rewritten_icc.data(), rewritten_icc.size()));
  } else {
    JxlColorEncoding external_color_encoding = color_encoding.ToExternal();
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetOutputColorProfile(
                                   dec, &external_color_encoding, nullptr, 0));
  }
  EXPECT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER, JxlDecoderProcessInput(dec));

  size_t buffer_size;
  JxlPixelFormat format = {3, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};

  JxlPixelFormat out_format = format;
  out_format.num_channels = color_encoding.Channels();
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderImageOutBufferSize(dec, &out_format, &buffer_size));
  out.resize(buffer_size);
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetImageOutBuffer(
                                 dec, &out_format, out.data(), out.size()));
  EXPECT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec));
  JxlDecoderDestroy(dec);
}

class DecodeAllEncodingsWithCMSTest
    : public ::testing::TestWithParam<jxl::test::ColorEncodingDescriptor> {};

JXL_GTEST_INSTANTIATE_TEST_SUITE_P(
    AllEncodings, DecodeAllEncodingsWithCMSTest,
    testing::ValuesIn(jxl::test::AllEncodings()));

TEST_P(DecodeAllEncodingsWithCMSTest, DecodeWithCMS) {
  auto all_encodings = jxl::test::AllEncodings();
  uint32_t num_channels = 3;
  size_t xsize = 177;
  size_t ysize = 123;
  std::vector<uint8_t> pixels =
      jxl::test::GetSomeTestImage(xsize, ysize, num_channels, 0);
  jxl::TestCodestreamParams params;
  std::vector<uint8_t> data =
      jxl::CreateTestJXLCodestream(jxl::Bytes(pixels.data(), pixels.size()),
                                   xsize, ysize, num_channels, params);

  jxl::ColorEncoding color_encoding =
      jxl::test::ColorEncodingFromDescriptor(GetParam());
  fprintf(stderr, "color_description: %s\n",
          Description(color_encoding).c_str());

  std::vector<uint8_t> out_with_cms;
  JxlBasicInfo info_with_cms;
  DecodeImageWithColorEncoding(data, color_encoding, true, out_with_cms,
                               info_with_cms);

  std::vector<uint8_t> out_without_cms;
  JxlBasicInfo info_without_cms;
  DecodeImageWithColorEncoding(data, color_encoding, false, out_without_cms,
                               info_without_cms);

  EXPECT_EQ(info_with_cms.xsize, info_without_cms.xsize);
  EXPECT_EQ(info_with_cms.ysize, info_without_cms.ysize);
  EXPECT_EQ(out_with_cms.size(), out_without_cms.size());
  double dist = ButteraugliDistance(xsize, ysize, out_with_cms, color_encoding,
                                    255, out_without_cms, color_encoding, 255);

  EXPECT_LT(dist, .1);
}

// Tests the case of lossy sRGB image without alpha channel, decoded to RGB8
// and to RGBA8
TEST(DecodeTest, PixelTestOpaqueSrgbLossy) {
  JxlMemoryManager* memory_manager = jxl::test::MemoryManager();
  for (unsigned channels = 3; channels <= 4; channels++) {
    JxlDecoder* dec = JxlDecoderCreate(nullptr);

    size_t xsize = 123;
    size_t ysize = 77;
    size_t num_pixels = xsize * ysize;
    std::vector<uint8_t> pixels =
        jxl::test::GetSomeTestImage(xsize, ysize, 3, 0);
    JxlPixelFormat format_orig = {3, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};
    std::vector<uint8_t> compressed = jxl::CreateTestJXLCodestream(
        jxl::Bytes(pixels.data(), pixels.size()), xsize, ysize, 3,
        jxl::TestCodestreamParams());

    JxlPixelFormat format = {channels, JXL_TYPE_UINT8, JXL_LITTLE_ENDIAN, 0};

    std::vector<uint8_t> pixels2 = jxl::DecodeWithAPI(
        dec, jxl::Bytes(compressed.data(), compressed.size()), format,
        /*use_callback=*/true, /*set_buffer_early=*/false,
        /*use_resizable_runner=*/false, /*require_boxes=*/false,
        /*expect_success*/ true);
    JxlDecoderReset(dec);
    EXPECT_EQ(num_pixels * channels, pixels2.size());

    jxl::ColorEncoding color_encoding0 = jxl::ColorEncoding::SRGB(false);
    jxl::Span<const uint8_t> span0(pixels.data(), pixels.size());
    auto io0 = jxl::make_unique<jxl::CodecInOut>(memory_manager);
    ASSERT_TRUE(io0->SetSize(xsize, ysize));
    EXPECT_TRUE(ConvertFromExternal(span0, xsize, ysize, color_encoding0,
                                    /*bits_per_sample=*/16, format_orig,
                                    /*pool=*/nullptr, &io0->Main()));

    jxl::ColorEncoding color_encoding1 = jxl::ColorEncoding::SRGB(false);
    jxl::Span<const uint8_t> span1(pixels2.data(), pixels2.size());
    auto io1 = jxl::make_unique<jxl::CodecInOut>(memory_manager);
    EXPECT_TRUE(ConvertFromExternal(span1, xsize, ysize, color_encoding1,
                                    /*bits_per_sample=*/8, format,
                                    /*pool=*/nullptr, &io1->Main()));

    jxl::ButteraugliParams butteraugli_params;
    EXPECT_SLIGHTLY_BELOW(
        ButteraugliDistance(io0->frames, io1->frames, butteraugli_params,
                            *JxlGetDefaultCms(),
                            /*distmap=*/nullptr, nullptr),
        1.07f);

    JxlDecoderDestroy(dec);
  }
}

// Opaque image with noise enabled, decoded to RGB8 and RGBA8.
TEST(DecodeTest, PixelTestOpaqueSrgbLossyNoise) {
  JxlMemoryManager* memory_manager = jxl::test::MemoryManager();
  for (unsigned channels = 3; channels <= 4; channels++) {
    JxlDecoder* dec = JxlDecoderCreate(nullptr);

    size_t xsize = 512;
    size_t ysize = 300;
    size_t num_pixels = xsize * ysize;
    std::vector<uint8_t> pixels =
        jxl::test::GetSomeTestImage(xsize, ysize, 3, 0);
    JxlPixelFormat format_orig = {3, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};
    jxl::TestCodestreamParams params;
    params.cparams.noise = jxl::Override::kOn;
    std::vector<uint8_t> compressed = jxl::CreateTestJXLCodestream(
        jxl::Bytes(pixels.data(), pixels.size()), xsize, ysize, 3, params);

    JxlPixelFormat format = {channels, JXL_TYPE_UINT8, JXL_LITTLE_ENDIAN, 0};

    std::vector<uint8_t> pixels2 = jxl::DecodeWithAPI(
        dec, jxl::Bytes(compressed.data(), compressed.size()), format,
        /*use_callback=*/false, /*set_buffer_early=*/true,
        /*use_resizable_runner=*/false, /*require_boxes=*/false,
        /*expect_success=*/true);
    JxlDecoderReset(dec);
    EXPECT_EQ(num_pixels * channels, pixels2.size());

    jxl::ColorEncoding color_encoding0 = jxl::ColorEncoding::SRGB(false);
    jxl::Span<const uint8_t> span0(pixels.data(), pixels.size());
    auto io0 = jxl::make_unique<jxl::CodecInOut>(memory_manager);
    ASSERT_TRUE(io0->SetSize(xsize, ysize));
    EXPECT_TRUE(ConvertFromExternal(span0, xsize, ysize, color_encoding0,
                                    /*bits_per_sample=*/16, format_orig,
                                    /*pool=*/nullptr, &io0->Main()));

    jxl::ColorEncoding color_encoding1 = jxl::ColorEncoding::SRGB(false);
    jxl::Span<const uint8_t> span1(pixels2.data(), pixels2.size());
    auto io1 = jxl::make_unique<jxl::CodecInOut>(memory_manager);
    EXPECT_TRUE(ConvertFromExternal(span1, xsize, ysize, color_encoding1,
                                    /*bits_per_sample=*/8, format,
                                    /*pool=*/nullptr, &io1->Main()));

    jxl::ButteraugliParams butteraugli_params;
    EXPECT_SLIGHTLY_BELOW(
        ButteraugliDistance(io0->frames, io1->frames, butteraugli_params,
                            *JxlGetDefaultCms(),
                            /*distmap=*/nullptr, nullptr),
        1.4f);

    JxlDecoderDestroy(dec);
  }
}

TEST(DecodeTest, ProcessEmptyInputWithBoxes) {
  size_t xsize = 123;
  size_t ysize = 77;
  std::vector<uint8_t> pixels = jxl::test::GetSomeTestImage(xsize, ysize, 3, 0);
  jxl::CompressParams cparams;
  uint32_t channels = 3;
  JxlPixelFormat format = {channels, JXL_TYPE_FLOAT, JXL_LITTLE_ENDIAN, 0};
  for (int i = 0; i < kCSBF_NUM_ENTRIES; ++i) {
    JxlDecoder* dec = JxlDecoderCreate(nullptr);
    jxl::TestCodestreamParams params;
    params.box_format = static_cast<CodeStreamBoxFormat>(i);
    printf("Testing empty input with box format %d\n",
           static_cast<int>(params.box_format));
    std::vector<uint8_t> compressed = jxl::CreateTestJXLCodestream(
        jxl::Bytes(pixels.data(), pixels.size()), xsize, ysize, 3, params);
    const int events =
        JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE | JXL_DEC_COLOR_ENCODING;
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSubscribeEvents(dec, events));
    EXPECT_EQ(JXL_DEC_NEED_MORE_INPUT, JxlDecoderProcessInput(dec));
    EXPECT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderSetInput(dec, compressed.data(), compressed.size()));
    EXPECT_EQ(JXL_DEC_BASIC_INFO, JxlDecoderProcessInput(dec));
    EXPECT_EQ(JXL_DEC_COLOR_ENCODING, JxlDecoderProcessInput(dec));
    size_t buffer_size;
    EXPECT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderImageOutBufferSize(dec, &format, &buffer_size));
    JxlBasicInfo info;
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBasicInfo(dec, &info));
    const size_t remaining = JxlDecoderReleaseInput(dec);
    EXPECT_LE(remaining, compressed.size());
    EXPECT_EQ(JXL_DEC_NEED_MORE_INPUT, JxlDecoderProcessInput(dec));
    JxlDecoderDestroy(dec);
  }
}

TEST(DecodeTest, ExtraBytesAfterCompressedStream) {
  size_t xsize = 123;
  size_t ysize = 77;
  size_t num_pixels = xsize * ysize;
  std::vector<uint8_t> pixels = jxl::test::GetSomeTestImage(xsize, ysize, 3, 0);
  jxl::CompressParams cparams;
  for (int i = 0; i < kCSBF_NUM_ENTRIES; ++i) {
    CodeStreamBoxFormat box_format = static_cast<CodeStreamBoxFormat>(i);
    if (box_format == kCSBF_Multi_Other_Zero_Terminated) continue;
    printf("Testing with box format %d\n", static_cast<int>(box_format));
    size_t last_unknown_box_size = 0;
    if (box_format == kCSBF_Single_Other) {
      last_unknown_box_size = unk1_box_size + 8;
    } else if (box_format == kCSBF_Multi_Other_Terminated) {
      last_unknown_box_size = unk3_box_size + 8;
    } else if (box_format == kCSBF_Multi_Last_Empty_Other) {
      // If boxes are not required, the decoder won't consume the last empty
      // jxlp box.
      last_unknown_box_size = 12 + unk3_box_size + 8;
    }
    jxl::TestCodestreamParams params;
    params.box_format = box_format;
    std::vector<uint8_t> compressed = jxl::CreateTestJXLCodestream(
        jxl::Bytes(pixels.data(), pixels.size()), xsize, ysize, 3, params);
    // Add some more bytes after compressed data.
    compressed.push_back(0);
    compressed.push_back(1);
    compressed.push_back(2);
    JxlDecoder* dec = JxlDecoderCreate(nullptr);
    uint32_t channels = 3;
    JxlPixelFormat format = {channels, JXL_TYPE_FLOAT, JXL_LITTLE_ENDIAN, 0};
    std::vector<uint8_t> pixels2 = jxl::DecodeWithAPI(
        dec, jxl::Bytes(compressed.data(), compressed.size()), format,
        /*use_callback=*/false, /*set_buffer_early=*/true,
        /*use_resizable_runner=*/false, /*require_boxes=*/false,
        /*expect_success=*/true);
    size_t unconsumed_bytes = JxlDecoderReleaseInput(dec);
    EXPECT_EQ(last_unknown_box_size + 3u, unconsumed_bytes);
    EXPECT_EQ(num_pixels * channels * 4, pixels2.size());
    JxlDecoderDestroy(dec);
  }
}

#if JPEGXL_ENABLE_BOXES
TEST(DecodeTest, BrobBoxInputIsRestrictedToBoxSize) {
  const uint8_t box_contents[] = {
      'o', 'r', 'i', 'g',
      0x0f, 0x05, 0x80, 0x68, 0x65, 0x6c, 0x6c, 0x6f,
      0x20, 0x77, 0x6f, 0x72, 0x6c, 0x64, 0x03};
  const uint8_t extra_bytes[] = {0xAA, 0xBB, 0xCC};
  std::vector<uint8_t> input;
  input.insert(input.end(), std::begin(box_contents), std::end(box_contents));
  input.insert(input.end(), std::begin(extra_bytes), std::end(extra_bytes));
  std::vector<uint8_t> out(32);
  jxl::JxlBoxContentDecoder decoder;
  decoder.StartBox(true, false, sizeof(box_contents));
  uint8_t* next_out = out.data();
  size_t avail_out = out.size();
  JxlDecoderStatus status = decoder.Process(
      input.data(), input.size(), 0, &next_out, &avail_out);
  EXPECT_EQ(JXL_DEC_BOX_COMPLETE, status);
  size_t output_size = next_out - out.data();
  EXPECT_EQ(11u, output_size);
  EXPECT_EQ(std::string("hello world"), std::string(reinterpret_cast<char*>(out.data()), output_size));
}

TEST(DecodeTest, BrobBoxTruncatedStreamWithExactBoxSizeReturnsError) {
  const uint8_t box_contents[] = {
      'o', 'r', 'i', 'g',
      0x0f, 0x05, 0x80, 0x68, 0x65, 0x6c, 0x6c, 0x6f,
      0x20, 0x77, 0x6f, 0x72, 0x6c, 0x64, 0x03};
  std::vector<uint8_t> input;
  input.insert(input.end(), std::begin(box_contents), std::end(box_contents) - 1);

  std::vector<uint8_t> out(32);
  jxl::JxlBoxContentDecoder decoder;
  decoder.StartBox(true, false, input.size());
  uint8_t* next_out = out.data();
  size_t avail_out = out.size();
  JxlDecoderStatus status = decoder.Process(
      input.data(), input.size(), 0, &next_out, &avail_out);
  EXPECT_EQ(JXL_DEC_ERROR, status);
}
#endif  // JPEGXL_ENABLE_BOXES

TEST(DecodeTest, ExtraBytesAfterCompressedStreamRequireBoxes) {
  size_t xsize = 123;
  size_t ysize = 77;
  size_t num_pixels = xsize * ysize;
  std::vector<uint8_t> pixels = jxl::test::GetSomeTestImage(xsize, ysize, 3, 0);
  jxl::CompressParams cparams;
  for (int i = 0; i < kCSBF_NUM_ENTRIES; ++i) {
    CodeStreamBoxFormat box_format = static_cast<CodeStreamBoxFormat>(i);
    if (box_format == kCSBF_Multi_Other_Zero_Terminated) continue;
    printf("Testing with box format %d\n", static_cast<int>(box_format));
    bool expect_success = (box_format == kCSBF_None ||
                           box_format == kCSBF_Single_Zero_Terminated ||
                           box_format == kCSBF_Multi_Zero_Terminated);
    jxl::TestCodestreamParams params;
    params.box_format = box_format;
    std::vector<uint8_t> compressed = jxl::CreateTestJXLCodestream(
        jxl::Bytes(pixels.data(), pixels.size()), xsize, ysize, 3, params);
    // Add some more bytes after compressed data.
    compressed.push_back(0);
    compressed.push_back(1);
    compressed.push_back(2);
    JxlDecoder* dec = JxlDecoderCreate(nullptr);
    uint32_t channels = 3;
    JxlPixelFormat format = {channels, JXL_TYPE_FLOAT, JXL_LITTLE_ENDIAN, 0};
    std::vector<uint8_t> pixels2 = jxl::DecodeWithAPI(
        dec, jxl::Bytes(compressed.data(), compressed.size()), format,
        /*use_callback=*/false, /*set_buffer_early=*/true,
        /*use_resizable_runner=*/false, /*require_boxes=*/true, expect_success);
    size_t unconsumed_bytes = JxlDecoderReleaseInput(dec);
    EXPECT_EQ(3u, unconsumed_bytes);
    EXPECT_EQ(num_pixels * channels * 4u, pixels2.size());
    JxlDecoderDestroy(dec);
  }
}

TEST(DecodeTest, ConcatenatedCompressedStreams) {
  size_t xsize = 123;
  size_t ysize = 77;
  size_t num_pixels = xsize * ysize;
  std::vector<uint8_t> pixels = jxl::test::GetSomeTestImage(xsize, ysize, 3, 0);
  jxl::CompressParams cparams;
  for (int i = 0; i < kCSBF_NUM_ENTRIES; ++i) {
    CodeStreamBoxFormat first_box_format = static_cast<CodeStreamBoxFormat>(i);
    if (first_box_format == kCSBF_Multi_Other_Zero_Terminated) continue;
    jxl::TestCodestreamParams params1;
    params1.box_format = first_box_format;
    std::vector<uint8_t> compressed1 = jxl::CreateTestJXLCodestream(
        jxl::Bytes(pixels.data(), pixels.size()), xsize, ysize, 3, params1);
    for (int j = 0; j < kCSBF_NUM_ENTRIES; ++j) {
      CodeStreamBoxFormat second_box_format =
          static_cast<CodeStreamBoxFormat>(j);
      if (second_box_format == kCSBF_Multi_Other_Zero_Terminated) continue;
      printf("Testing with box format pair %d, %d\n",
             static_cast<int>(first_box_format),
             static_cast<int>(second_box_format));
      jxl::TestCodestreamParams params2;
      params2.box_format = second_box_format;
      std::vector<uint8_t> compressed2 = jxl::CreateTestJXLCodestream(
          jxl::Bytes(pixels.data(), pixels.size()), xsize, ysize, 3, params2);
      std::vector<uint8_t> concat;
      jxl::Bytes(compressed1).AppendTo(concat);
      jxl::Bytes(compressed2).AppendTo(concat);
      uint32_t channels = 3;
      JxlPixelFormat format = {channels, JXL_TYPE_FLOAT, JXL_LITTLE_ENDIAN, 0};
      size_t remaining = concat.size();
      for (int part = 0; part < 2; ++part) {
        printf("  Decoding part %d\n", part + 1);
        JxlDecoder* dec = JxlDecoderCreate(nullptr);
        size_t pos = concat.size() - remaining;
        bool expect_success =
            (part == 0 || second_box_format == kCSBF_None ||
             second_box_format == kCSBF_Single_Zero_Terminated ||
             second_box_format == kCSBF_Multi_Zero_Terminated);
        std::vector<uint8_t> pixels2 = jxl::DecodeWithAPI(
            dec, jxl::Bytes(concat.data() + pos, remaining), format,
            /*use_callback=*/false, /*set_buffer_early=*/true,
            /*use_resizable_runner=*/false, /*require_boxes=*/true,
            expect_success);
        EXPECT_EQ(num_pixels * channels * 4u, pixels2.size());
        remaining = JxlDecoderReleaseInput(dec);
        JxlDecoderDestroy(dec);
      }
      EXPECT_EQ(0u, remaining);
    }
  }
}

void TestPartialStream(bool reconstructible_jpeg) {
  size_t xsize = 123;
  size_t ysize = 77;
  uint32_t channels = 4;
  if (reconstructible_jpeg) {
    channels = 3;
  }
  std::vector<uint8_t> pixels =
      jxl::test::GetSomeTestImage(xsize, ysize, channels, 0);
  JxlPixelFormat format_orig = {channels, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};
  jxl::TestCodestreamParams params;
  if (reconstructible_jpeg) {
    params.cparams.color_transform = jxl::ColorTransform::kNone;
  } else {
    // Lossless to verify pixels exactly after roundtrip.
    params.cparams.SetLossless();
  }

  std::vector<uint8_t> pixels2;
  pixels2.resize(pixels.size());

  std::vector<uint8_t> jpeg_output(64);
  size_t used_jpeg_output = 0;

  std::vector<std::vector<uint8_t>> codestreams(kCSBF_NUM_ENTRIES);
  std::vector<std::vector<uint8_t>> jpeg_codestreams(kCSBF_NUM_ENTRIES);
  for (size_t i = 0; i < kCSBF_NUM_ENTRIES; ++i) {
    params.box_format = static_cast<CodeStreamBoxFormat>(i);
    if (reconstructible_jpeg) {
      params.jpeg_codestream = &jpeg_codestreams[i];
    }
    codestreams[i] =
        jxl::CreateTestJXLCodestream(jxl::Bytes(pixels.data(), pixels.size()),
                                     xsize, ysize, channels, params);
  }

  // Test multiple step sizes, to test different combinations of the streaming
  // box parsing.
  std::vector<size_t> increments = {1, 3, 17, 23, 120, 700, 1050};

  for (size_t increment : increments) {
    for (size_t i = 0; i < kCSBF_NUM_ENTRIES; ++i) {
      if (reconstructible_jpeg && static_cast<CodeStreamBoxFormat>(i) ==
                                      CodeStreamBoxFormat::kCSBF_None) {
        continue;
      }
      const std::vector<uint8_t>& data = codestreams[i];
      const uint8_t* next_in = data.data();
      size_t avail_in = 0;

      JxlDecoder* dec = JxlDecoderCreate(nullptr);

      EXPECT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderSubscribeEvents(
                    dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE |
                             JXL_DEC_JPEG_RECONSTRUCTION));

      bool seen_basic_info = false;
      bool seen_full_image = false;
      bool seen_jpeg_recon = false;

      size_t total_size = 0;

      for (;;) {
        EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, next_in, avail_in));
        JxlDecoderStatus status = JxlDecoderProcessInput(dec);
        size_t remaining = JxlDecoderReleaseInput(dec);
        EXPECT_LE(remaining, avail_in);
        next_in += avail_in - remaining;
        avail_in = remaining;
        if (status == JXL_DEC_NEED_MORE_INPUT) {
          if (total_size >= data.size()) {
            // End of test data reached, it should have successfully decoded the
            // image now.
            FAIL();
            break;
          }

          // End of the file reached, should be the final test.
          if (total_size + increment > data.size()) {
            increment = data.size() - total_size;
          }
          total_size += increment;
          avail_in += increment;
        } else if (status == JXL_DEC_BASIC_INFO) {
          // This event should happen exactly once
          EXPECT_FALSE(seen_basic_info);
          if (seen_basic_info) break;
          seen_basic_info = true;
          JxlBasicInfo info;
          EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBasicInfo(dec, &info));
          EXPECT_EQ(info.xsize, xsize);
          EXPECT_EQ(info.ysize, ysize);
        } else if (status == JXL_DEC_JPEG_RECONSTRUCTION) {
          EXPECT_FALSE(seen_basic_info);
          EXPECT_FALSE(seen_full_image);
          EXPECT_EQ(JXL_DEC_SUCCESS,
                    JxlDecoderSetJPEGBuffer(dec, jpeg_output.data(),
                                            jpeg_output.size()));
          seen_jpeg_recon = true;
        } else if (status == JXL_DEC_JPEG_NEED_MORE_OUTPUT) {
          EXPECT_TRUE(seen_jpeg_recon);
          used_jpeg_output =
              jpeg_output.size() - JxlDecoderReleaseJPEGBuffer(dec);
          jpeg_output.resize(jpeg_output.size() * 2);
          EXPECT_EQ(JXL_DEC_SUCCESS,
                    JxlDecoderSetJPEGBuffer(
                        dec, jpeg_output.data() + used_jpeg_output,
                        jpeg_output.size() - used_jpeg_output));
        } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
          EXPECT_EQ(JXL_DEC_SUCCESS,
                    JxlDecoderSetImageOutBuffer(
                        dec, &format_orig, pixels2.data(), pixels2.size()));
        } else if (status == JXL_DEC_FULL_IMAGE) {
          // This event should happen exactly once
          EXPECT_FALSE(seen_full_image);
          if (seen_full_image) break;
          // This event should happen after basic info
          EXPECT_TRUE(seen_basic_info);
          seen_full_image = true;
          if (reconstructible_jpeg) {
            used_jpeg_output =
                jpeg_output.size() - JxlDecoderReleaseJPEGBuffer(dec);
            EXPECT_EQ(used_jpeg_output, jpeg_codestreams[i].size());
            EXPECT_EQ(0, memcmp(jpeg_output.data(), jpeg_codestreams[i].data(),
                                used_jpeg_output));
          } else {
            EXPECT_EQ(pixels, pixels2);
          }
        } else if (status == JXL_DEC_SUCCESS) {
          EXPECT_TRUE(seen_full_image);
          break;
        } else {
          // We do not expect any other events or errors
          FAIL();
          break;
        }
      }

      // Ensure the decoder emitted the basic info and full image events
      EXPECT_TRUE(seen_basic_info);
      EXPECT_TRUE(seen_full_image);

      JxlDecoderDestroy(dec);
    }
  }
}

// Tests the return status when trying to decode pixels on incomplete file: it
// should return JXL_DEC_NEED_MORE_INPUT, not error.
TEST(DecodeTest, PixelPartialTest) { TestPartialStream(false); }

// Tests the return status when trying to decode JPEG bytes on incomplete file.
JXL_TRANSCODE_JPEG_TEST(DecodeTest, JPEGPartialTest) {
  TEST_LIBJPEG_SUPPORT();
  TestPartialStream(true);
}

// The DC event still exists, but is no longer implemented, it is deprecated.
TEST(DecodeTest, DCNotGettableTest) {
  // 1x1 pixel JXL image
  std::string compressed(
      "\377\n\0\20\260\23\0H\200("
      "\0\334\0U\17\0\0\250P\31e\334\340\345\\\317\227\37:,"
      "\246m\\gh\253m\vK\22E\306\261I\252C&pH\22\353 "
      "\363\6\22\bp\0\200\237\34\231W2d\255$\1",
      68);

  JxlDecoder* dec = JxlDecoderCreate(nullptr);

  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO));
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSetInput(
                dec, reinterpret_cast<const uint8_t*>(compressed.data()),
                compressed.size()));

  EXPECT_EQ(JXL_DEC_BASIC_INFO, JxlDecoderProcessInput(dec));

  // Since the image is only 1x1 pixel, there is only 1 group, the decoder is
  // unable to get DC size from this, and will not return the DC at all. Since
  // no full image is requested either, it is expected to return success.
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderProcessInput(dec));

  JxlDecoderDestroy(dec);
}

TEST(DecodeTest, PreviewTest) {
  JxlMemoryManager* memory_manager = jxl::test::MemoryManager();
  size_t xsize = 77;
  size_t ysize = 120;
  std::vector<uint8_t> pixels = jxl::test::GetSomeTestImage(xsize, ysize, 3, 0);
  JxlPixelFormat format_orig = {3, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};
  for (jxl::PreviewMode mode : {jxl::kSmallPreview, jxl::kBigPreview}) {
    jxl::TestCodestreamParams params;
    params.preview_mode = mode;

    std::vector<uint8_t> compressed = jxl::CreateTestJXLCodestream(
        jxl::Bytes(pixels.data(), pixels.size()), xsize, ysize, 3, params);

    JxlPixelFormat format = {3, JXL_TYPE_UINT8, JXL_LITTLE_ENDIAN, 0};

    JxlDecoder* dec = JxlDecoderCreate(nullptr);
    const uint8_t* next_in = compressed.data();
    size_t avail_in = compressed.size();

    EXPECT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderSubscribeEvents(
                  dec, JXL_DEC_BASIC_INFO | JXL_DEC_PREVIEW_IMAGE));
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, next_in, avail_in));

    EXPECT_EQ(JXL_DEC_BASIC_INFO, JxlDecoderProcessInput(dec));
    JxlBasicInfo info;
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBasicInfo(dec, &info));
    size_t buffer_size;
    EXPECT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderPreviewOutBufferSize(dec, &format, &buffer_size));

    jxl::ColorEncoding c_srgb = jxl::ColorEncoding::SRGB(false);
    auto io0 = jxl::make_unique<jxl::CodecInOut>(memory_manager);
    EXPECT_TRUE(jxl::ConvertFromExternal(
        jxl::Bytes(pixels.data(), pixels.size()), xsize, ysize, c_srgb,
        /*bits_per_sample=*/16, format_orig, /*pool=*/nullptr, &io0->Main()));
    GeneratePreview(params.preview_mode, &io0->Main());

    size_t xsize_preview = io0->Main().xsize();
    size_t ysize_preview = io0->Main().ysize();
    EXPECT_EQ(xsize_preview, info.preview.xsize);
    EXPECT_EQ(ysize_preview, info.preview.ysize);
    EXPECT_EQ(xsize_preview * ysize_preview * 3u, buffer_size);

    EXPECT_EQ(JXL_DEC_NEED_PREVIEW_OUT_BUFFER, JxlDecoderProcessInput(dec));

    std::vector<uint8_t> preview(buffer_size);
    EXPECT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderSetPreviewOutBuffer(dec, &format, preview.data(),
                                            preview.size()));

    EXPECT_EQ(JXL_DEC_PREVIEW_IMAGE, JxlDecoderProcessInput(dec));

    auto io1 = jxl::make_unique<jxl::CodecInOut>(memory_manager);
    EXPECT_TRUE(
        jxl::ConvertFromExternal(jxl::Bytes(preview.data(), preview.size()),
                                 xsize_preview, ysize_preview, c_srgb,
                                 /*bits_per_sample=*/8, format,
                                 /*pool=*/nullptr, &io1->Main()));

    jxl::ButteraugliParams butteraugli_params;
    // TODO(lode): this ButteraugliDistance silently returns 0 (dangerous for
    // tests) if xsize or ysize is < 8, no matter how different the images, a
    // tiny size that could happen for a preview. ButteraugliDiffmap does
    // support smaller than 8x8, but jxl's ButteraugliDistance does not. Perhaps
    // move butteraugli's <8x8 handling from ButteraugliDiffmap to
    // ButteraugliComparator::Diffmap in butteraugli.cc.
    EXPECT_LE(ButteraugliDistance(io0->frames, io1->frames, butteraugli_params,
                                  *JxlGetDefaultCms(),
                                  /*distmap=*/nullptr, nullptr),
              mode == jxl::kSmallPreview ? 0.7f : 1.2f);

    JxlDecoderDestroy(dec);
  }
}

TEST(DecodeTest, AlignTest) {
  size_t xsize = 123;
  size_t ysize = 77;
  std::vector<uint8_t> pixels = jxl::test::GetSomeTestImage(xsize, ysize, 4, 0);
  JxlPixelFormat format_orig = {4, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};

  jxl::TestCodestreamParams params;
  // Lossless to verify pixels exactly after roundtrip.
  params.cparams.SetLossless();
  params.cparams.speed_tier = jxl::SpeedTier::kThunder;
  std::vector<uint8_t> compressed = jxl::CreateTestJXLCodestream(
      jxl::Bytes(pixels.data(), pixels.size()), xsize, ysize, 4, params);

  size_t align = 17;
  JxlPixelFormat format = {3, JXL_TYPE_UINT8, JXL_LITTLE_ENDIAN, align};
  // On purpose not using jxl::RoundUpTo to test it independently.
  size_t expected_line_size_last = 1 * 3 * xsize;
  size_t expected_line_size =
      ((expected_line_size_last + align - 1) / align) * align;
  size_t expected_pixels_size =
      expected_line_size * (ysize - 1) + expected_line_size_last;

  for (bool use_callback : {false, true}) {
    std::vector<uint8_t> pixels2 = jxl::DecodeWithAPI(
        jxl::Bytes(compressed.data(), compressed.size()), format, use_callback,
        /*set_buffer_early=*/false,
        /*use_resizable_runner=*/false, /*require_boxes=*/false,
        /*expect_success=*/true);
    EXPECT_EQ(expected_pixels_size, pixels2.size());
    EXPECT_EQ(0u, jxl::test::ComparePixels(pixels.data(), pixels2.data(), xsize,
                                           ysize, format_orig, format));
  }
}

TEST(DecodeTest, AnimationTest) {
  JxlMemoryManager* memory_manager = jxl::test::MemoryManager();
  size_t xsize = 123;
  size_t ysize = 77;
  static const size_t num_frames = 2;
  std::vector<uint8_t> frames[2];
  frames[0] = jxl::test::GetSomeTestImage(xsize, ysize, 3, 0);
  frames[1] = jxl::test::GetSomeTestImage(xsize, ysize, 3, 1);
  JxlPixelFormat format = {3, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};

  auto io = jxl::make_unique<jxl::CodecInOut>(memory_manager);
  ASSERT_TRUE(io->SetSize(xsize, ysize));
  io->metadata.m.SetUintSamples(16);
  io->metadata.m.color_encoding = jxl::ColorEncoding::SRGB(false);
  io->metadata.m.have_animation = true;
  io->frames.clear();
  io->frames.reserve(num_frames);
  ASSERT_TRUE(io->SetSize(xsize, ysize));

  std::vector<uint32_t> frame_durations(num_frames);
  for (size_t i = 0; i < num_frames; ++i) {
    frame_durations[i] = 5 + i;
  }

  for (size_t i = 0; i < num_frames; ++i) {
    jxl::ImageBundle bundle(memory_manager, &io->metadata.m);

    EXPECT_TRUE(ConvertFromExternal(
        jxl::Bytes(frames[i].data(), frames[i].size()), xsize, ysize,
        jxl::ColorEncoding::SRGB(/*is_gray=*/false),
        /*bits_per_sample=*/16, format,
        /*pool=*/nullptr, &bundle));
    bundle.duration = frame_durations[i];
    io->frames.push_back(std::move(bundle));
  }

  jxl::CompressParams cparams;
  cparams.SetLossless();  // Lossless to verify pixels exactly after roundtrip.
  cparams.speed_tier = jxl::SpeedTier::kThunder;
  std::vector<uint8_t> compressed;
  EXPECT_TRUE(jxl::test::EncodeFile(cparams, io.get(), &compressed));

  // Decode and test the animation frames

  JxlDecoder* dec = JxlDecoderCreate(nullptr);
  const uint8_t* next_in = compressed.data();
  size_t avail_in = compressed.size();

  void* runner = JxlThreadParallelRunnerCreate(
      nullptr, JxlThreadParallelRunnerDefaultNumWorkerThreads());
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSetParallelRunner(dec, JxlThreadParallelRunner, runner));

  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSubscribeEvents(
                dec, JXL_DEC_BASIC_INFO | JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE));
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, next_in, avail_in));

  EXPECT_EQ(JXL_DEC_BASIC_INFO, JxlDecoderProcessInput(dec));
  size_t buffer_size;
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderImageOutBufferSize(dec, &format, &buffer_size));
  JxlBasicInfo info;
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBasicInfo(dec, &info));

  for (size_t i = 0; i < num_frames; ++i) {
    std::vector<uint8_t> pixels(buffer_size);

    EXPECT_EQ(JXL_DEC_FRAME, JxlDecoderProcessInput(dec));

    JxlFrameHeader frame_header;
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetFrameHeader(dec, &frame_header));
    EXPECT_EQ(frame_durations[i], frame_header.duration);
    EXPECT_EQ(0u, frame_header.name_length);
    // For now, test with empty name, there's currently no easy way to encode
    // a jxl file with a frame name because ImageBundle doesn't have a
    // jxl::FrameHeader to set the name in. We can test the null termination
    // character though.
    char name;
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetFrameName(dec, &name, 1));
    EXPECT_EQ(0, name);

    EXPECT_EQ(i + 1u == num_frames, frame_header.is_last);

    EXPECT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER, JxlDecoderProcessInput(dec));

    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetImageOutBuffer(
                                   dec, &format, pixels.data(), pixels.size()));

    EXPECT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec));
    EXPECT_EQ(0u, jxl::test::ComparePixels(frames[i].data(), pixels.data(),
                                           xsize, ysize, format, format));
  }

  // After all frames were decoded, JxlDecoderProcessInput should return
  // success to indicate all is done.
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderProcessInput(dec));

  JxlThreadParallelRunnerDestroy(runner);
  JxlDecoderDestroy(dec);
}

TEST(DecodeTest, AnimationTestStreaming) {
  JxlMemoryManager* memory_manager = jxl::test::MemoryManager();
  size_t xsize = 123;
  size_t ysize = 77;
  static const size_t num_frames = 2;
  std::vector<uint8_t> frames[2];
  frames[0] = jxl::test::GetSomeTestImage(xsize, ysize, 3, 0);
  frames[1] = jxl::test::GetSomeTestImage(xsize, ysize, 3, 1);
  JxlPixelFormat format = {3, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};

  auto io = jxl::make_unique<jxl::CodecInOut>(memory_manager);
  ASSERT_TRUE(io->SetSize(xsize, ysize));
  io->metadata.m.SetUintSamples(16);
  io->metadata.m.color_encoding = jxl::ColorEncoding::SRGB(false);
  io->metadata.m.have_animation = true;
  io->frames.clear();
  io->frames.reserve(num_frames);
  ASSERT_TRUE(io->SetSize(xsize, ysize));

  std::vector<uint32_t> frame_durations(num_frames);
  for (size_t i = 0; i < num_frames; ++i) {
    frame_durations[i] = 5 + i;
  }

  for (size_t i = 0; i < num_frames; ++i) {
    jxl::ImageBundle bundle(memory_manager, &io->metadata.m);

    EXPECT_TRUE(ConvertFromExternal(
        jxl::Bytes(frames[i].data(), frames[i].size()), xsize, ysize,
        jxl::ColorEncoding::SRGB(/*is_gray=*/false),
        /*bits_per_sample=*/16, format,
        /*pool=*/nullptr, &bundle));
    bundle.duration = frame_durations[i];
    io->frames.push_back(std::move(bundle));
  }

  jxl::CompressParams cparams;
  cparams.SetLossless();  // Lossless to verify pixels exactly after roundtrip.
  cparams.speed_tier = jxl::SpeedTier::kThunder;
  std::vector<uint8_t> compressed;
  EXPECT_TRUE(jxl::test::EncodeFile(cparams, io.get(), &compressed));

  // Decode and test the animation frames

  const size_t step_size = 16;

  JxlDecoder* dec = JxlDecoderCreate(nullptr);
  const uint8_t* next_in = compressed.data();
  size_t avail_in = 0;
  size_t frame_headers_seen = 0;
  size_t frames_seen = 0;
  bool seen_basic_info = false;

  void* runner = JxlThreadParallelRunnerCreate(
      nullptr, JxlThreadParallelRunnerDefaultNumWorkerThreads());
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSetParallelRunner(dec, JxlThreadParallelRunner, runner));

  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSubscribeEvents(
                dec, JXL_DEC_BASIC_INFO | JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE));

  std::vector<uint8_t> frames2[2];
  for (size_t i = 0; i < num_frames; ++i) {
    frames2[i].resize(frames[i].size());
  }

  size_t total_in = 0;
  size_t loop_count = 0;

  for (;;) {
    if (loop_count++ > compressed.size()) {
      fprintf(stderr, "Too many loops\n");
      FAIL();
      break;
    }

    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, next_in, avail_in));
    auto status = JxlDecoderProcessInput(dec);
    size_t remaining = JxlDecoderReleaseInput(dec);
    EXPECT_LE(remaining, avail_in);
    next_in += avail_in - remaining;
    avail_in = remaining;

    if (status == JXL_DEC_SUCCESS) {
      break;
    } else if (status == JXL_DEC_ERROR) {
      FAIL();
    } else if (status == JXL_DEC_NEED_MORE_INPUT) {
      if (total_in >= compressed.size()) {
        fprintf(stderr, "Already gave all input data\n");
        FAIL();
        break;
      }
      size_t amount = step_size;
      if (total_in + amount > compressed.size()) {
        amount = compressed.size() - total_in;
      }
      avail_in += amount;
      total_in += amount;
    } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
      EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetImageOutBuffer(
                                     dec, &format, frames2[frames_seen].data(),
                                     frames2[frames_seen].size()));
    } else if (status == JXL_DEC_BASIC_INFO) {
      EXPECT_EQ(false, seen_basic_info);
      seen_basic_info = true;
      JxlBasicInfo info;
      EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBasicInfo(dec, &info));
      EXPECT_EQ(xsize, info.xsize);
      EXPECT_EQ(ysize, info.ysize);
    } else if (status == JXL_DEC_FRAME) {
      EXPECT_EQ(true, seen_basic_info);
      frame_headers_seen++;
    } else if (status == JXL_DEC_FULL_IMAGE) {
      frames_seen++;
      EXPECT_EQ(frame_headers_seen, frames_seen);
    } else {
      fprintf(stderr, "Unexpected status: %d\n", static_cast<int>(status));
      FAIL();
    }
  }

  EXPECT_EQ(true, seen_basic_info);
  EXPECT_EQ(num_frames, frames_seen);
  EXPECT_EQ(num_frames, frame_headers_seen);
  for (size_t i = 0; i < num_frames; ++i) {
    EXPECT_EQ(frames[i], frames2[i]);
  }

  JxlThreadParallelRunnerDestroy(runner);
  JxlDecoderDestroy(dec);
}

TEST(DecodeTest, ExtraChannelTest) {
  size_t xsize = 55;
  size_t ysize = 257;
  std::vector<uint8_t> pixels = jxl::test::GetSomeTestImage(xsize, ysize, 4, 0);
  JxlPixelFormat format_orig = {4, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};

  jxl::TestCodestreamParams params;
  // Lossless to verify pixels exactly after roundtrip.
  params.cparams.SetLossless();
  params.cparams.speed_tier = jxl::SpeedTier::kThunder;
  std::vector<uint8_t> compressed = jxl::CreateTestJXLCodestream(
      jxl::Bytes(pixels.data(), pixels.size()), xsize, ysize, 4, params);

  size_t align = 17;
  JxlPixelFormat format = {3, JXL_TYPE_UINT8, JXL_LITTLE_ENDIAN, align};

  JxlDecoder* dec = JxlDecoderCreate(nullptr);

  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSubscribeEvents(
                                 dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE));

  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSetInput(dec, compressed.data(), compressed.size()));
  EXPECT_EQ(JXL_DEC_BASIC_INFO, JxlDecoderProcessInput(dec));
  JxlBasicInfo info;
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBasicInfo(dec, &info));
  EXPECT_EQ(1u, info.num_extra_channels);
  EXPECT_EQ(JXL_FALSE, info.alpha_premultiplied);

  JxlExtraChannelInfo extra_info;
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderGetExtraChannelInfo(dec, 0, &extra_info));
  EXPECT_EQ(0, extra_info.type);

  EXPECT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER, JxlDecoderProcessInput(dec));
  size_t buffer_size;
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderImageOutBufferSize(dec, &format, &buffer_size));
  size_t extra_size;
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderExtraChannelBufferSize(dec, &format, &extra_size, 0));

  std::vector<uint8_t> image(buffer_size);
  std::vector<uint8_t> extra(extra_size);

  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetImageOutBuffer(
                                 dec, &format, image.data(), image.size()));
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetExtraChannelBuffer(
                                 dec, &format, extra.data(), extra.size(), 0));

  EXPECT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec));

  // After the full image was output, JxlDecoderProcessInput should return
  // success to indicate all is done.
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderProcessInput(dec));
  JxlDecoderDestroy(dec);

  EXPECT_EQ(0u, jxl::test::ComparePixels(pixels.data(), image.data(), xsize,
                                         ysize, format_orig, format));

  // Compare the extracted extra channel with the original alpha channel

  std::vector<uint8_t> alpha(pixels.size() / 4);
  for (size_t i = 0; i < pixels.size(); i += 8) {
    size_t index_alpha = i / 4;
    alpha[index_alpha + 0] = pixels[i + 6];
    alpha[index_alpha + 1] = pixels[i + 7];
  }
  JxlPixelFormat format_alpha = format;
  format_alpha.num_channels = 1;
  JxlPixelFormat format_orig_alpha = format_orig;
  format_orig_alpha.num_channels = 1;

  EXPECT_EQ(0u,
            jxl::test::ComparePixels(alpha.data(), extra.data(), xsize, ysize,
                                     format_orig_alpha, format_alpha));
}

TEST(DecodeTest, SkipCurrentFrameTest) {
  JxlMemoryManager* memory_manager = jxl::test::MemoryManager();
  size_t xsize = 90;
  size_t ysize = 120;
  constexpr size_t num_frames = 7;
  std::vector<uint8_t> frames[num_frames];
  for (size_t i = 0; i < num_frames; i++) {
    frames[i] = jxl::test::GetSomeTestImage(xsize, ysize, 3, i);
  }
  JxlPixelFormat format = {3, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};

  auto io = jxl::make_unique<jxl::CodecInOut>(memory_manager);
  ASSERT_TRUE(io->SetSize(xsize, ysize));
  io->metadata.m.SetUintSamples(16);
  io->metadata.m.color_encoding = jxl::ColorEncoding::SRGB(false);
  io->metadata.m.have_animation = true;
  io->frames.clear();
  io->frames.reserve(num_frames);
  ASSERT_TRUE(io->SetSize(xsize, ysize));

  std::vector<uint32_t> frame_durations(num_frames);
  for (size_t i = 0; i < num_frames; ++i) {
    frame_durations[i] = 5 + i;
  }

  for (size_t i = 0; i < num_frames; ++i) {
    jxl::ImageBundle bundle(memory_manager, &io->metadata.m);
    if (i & 1) {
      // Mark some frames as referenceable, others not.
      bundle.use_for_next_frame = true;
    }

    EXPECT_TRUE(ConvertFromExternal(
        jxl::Bytes(frames[i].data(), frames[i].size()), xsize, ysize,
        jxl::ColorEncoding::SRGB(/*is_gray=*/false),
        /*bits_per_sample=*/16, format,
        /*pool=*/nullptr, &bundle));
    bundle.duration = frame_durations[i];
    io->frames.push_back(std::move(bundle));
  }

  jxl::CompressParams cparams;
  cparams.speed_tier = jxl::SpeedTier::kThunder;
  std::vector<uint8_t> compressed;
  jxl::PassDefinition passes[] = {{2, 0, 4}, {4, 0, 4}, {8, 2, 2}, {8, 0, 1}};
  jxl::ProgressiveMode progressive_mode{passes};
  cparams.custom_progressive_mode = &progressive_mode;
  EXPECT_TRUE(jxl::test::EncodeFile(cparams, io.get(), &compressed));

  JxlDecoder* dec = JxlDecoderCreate(nullptr);
  const uint8_t* next_in = compressed.data();
  size_t avail_in = compressed.size();

  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_FRAME |
                                               JXL_DEC_FRAME_PROGRESSION |
                                               JXL_DEC_FULL_IMAGE));
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetProgressiveDetail(dec, kLastPasses));
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, next_in, avail_in));

  EXPECT_EQ(JXL_DEC_BASIC_INFO, JxlDecoderProcessInput(dec));
  size_t buffer_size;
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderImageOutBufferSize(dec, &format, &buffer_size));
  JxlBasicInfo info;
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBasicInfo(dec, &info));

  for (size_t i = 0; i < num_frames; ++i) {
    printf("Decoding frame %d\n", static_cast<int>(i));
    EXPECT_EQ(JXL_DEC_ERROR, JxlDecoderSkipCurrentFrame(dec));
    std::vector<uint8_t> pixels(buffer_size);
    EXPECT_EQ(JXL_DEC_FRAME, JxlDecoderProcessInput(dec));
    EXPECT_EQ(JXL_DEC_ERROR, JxlDecoderSkipCurrentFrame(dec));
    JxlFrameHeader frame_header;
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetFrameHeader(dec, &frame_header));
    EXPECT_EQ(frame_durations[i], frame_header.duration);
    EXPECT_EQ(i + 1u == num_frames, frame_header.is_last);
    EXPECT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER, JxlDecoderProcessInput(dec));
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetImageOutBuffer(
                                   dec, &format, pixels.data(), pixels.size()));
    if (i == 2u) {
      EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSkipCurrentFrame(dec));
      continue;
    }
    EXPECT_EQ(JXL_DEC_FRAME_PROGRESSION, JxlDecoderProcessInput(dec));
    EXPECT_EQ(8u, JxlDecoderGetIntendedDownsamplingRatio(dec));
    if (i == 3u) {
      EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSkipCurrentFrame(dec));
      continue;
    }
    EXPECT_EQ(JXL_DEC_FRAME_PROGRESSION, JxlDecoderProcessInput(dec));
    EXPECT_EQ(4u, JxlDecoderGetIntendedDownsamplingRatio(dec));
    if (i == 4u) {
      EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSkipCurrentFrame(dec));
      continue;
    }
    EXPECT_EQ(JXL_DEC_FRAME_PROGRESSION, JxlDecoderProcessInput(dec));
    EXPECT_EQ(2u, JxlDecoderGetIntendedDownsamplingRatio(dec));
    if (i == 5u) {
      EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSkipCurrentFrame(dec));
      continue;
    }
    EXPECT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec));
    EXPECT_EQ(JXL_DEC_ERROR, JxlDecoderSkipCurrentFrame(dec));
  }

  // After all frames were decoded, JxlDecoderProcessInput should return
  // success to indicate all is done.
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderProcessInput(dec));

  JxlDecoderDestroy(dec);
}

TEST(DecodeTest, SkipFrameTest) {
  JxlMemoryManager* memory_manager = jxl::test::MemoryManager();
  size_t xsize = 90;
  size_t ysize = 120;
  constexpr size_t num_frames = 16;
  std::vector<uint8_t> frames[num_frames];
  for (size_t i = 0; i < num_frames; i++) {
    frames[i] = jxl::test::GetSomeTestImage(xsize, ysize, 3, i);
  }
  JxlPixelFormat format = {3, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};

  auto io = jxl::make_unique<jxl::CodecInOut>(memory_manager);
  ASSERT_TRUE(io->SetSize(xsize, ysize));
  io->metadata.m.SetUintSamples(16);
  io->metadata.m.color_encoding = jxl::ColorEncoding::SRGB(false);
  io->metadata.m.have_animation = true;
  io->frames.clear();
  io->frames.reserve(num_frames);
  ASSERT_TRUE(io->SetSize(xsize, ysize));

  std::vector<uint32_t> frame_durations(num_frames);
  for (size_t i = 0; i < num_frames; ++i) {
    frame_durations[i] = 5 + i;
  }

  for (size_t i = 0; i < num_frames; ++i) {
    jxl::ImageBundle bundle(memory_manager, &io->metadata.m);
    if (i & 1) {
      // Mark some frames as referenceable, others not.
      bundle.use_for_next_frame = true;
    }

    EXPECT_TRUE(ConvertFromExternal(
        jxl::Bytes(frames[i].data(), frames[i].size()), xsize, ysize,
        jxl::ColorEncoding::SRGB(/*is_gray=*/false),
        /*bits_per_sample=*/16, format,
        /*pool=*/nullptr, &bundle));
    bundle.duration = frame_durations[i];
    io->frames.push_back(std::move(bundle));
  }

  jxl::CompressParams cparams;
  cparams.SetLossless();  // Lossless to verify pixels exactly after roundtrip.
  cparams.speed_tier = jxl::SpeedTier::kThunder;
  std::vector<uint8_t> compressed;
  EXPECT_TRUE(jxl::test::EncodeFile(cparams, io.get(), &compressed));

  // Decode and test the animation frames

  JxlDecoder* dec = JxlDecoderCreate(nullptr);
  const uint8_t* next_in = compressed.data();
  size_t avail_in = compressed.size();

  void* runner = JxlThreadParallelRunnerCreate(
      nullptr, JxlThreadParallelRunnerDefaultNumWorkerThreads());
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSetParallelRunner(dec, JxlThreadParallelRunner, runner));

  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSubscribeEvents(
                dec, JXL_DEC_BASIC_INFO | JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE));
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, next_in, avail_in));

  EXPECT_EQ(JXL_DEC_BASIC_INFO, JxlDecoderProcessInput(dec));
  size_t buffer_size;
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderImageOutBufferSize(dec, &format, &buffer_size));
  JxlBasicInfo info;
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBasicInfo(dec, &info));

  for (size_t i = 0; i < num_frames; ++i) {
    if (i == 3) {
      JxlDecoderSkipFrames(dec, 5);
      i += 5;
    }
    std::vector<uint8_t> pixels(buffer_size);

    EXPECT_EQ(JXL_DEC_FRAME, JxlDecoderProcessInput(dec));

    JxlFrameHeader frame_header;
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetFrameHeader(dec, &frame_header));
    EXPECT_EQ(frame_durations[i], frame_header.duration);

    EXPECT_EQ(i + 1u == num_frames, frame_header.is_last);

    EXPECT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER, JxlDecoderProcessInput(dec));

    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetImageOutBuffer(
                                   dec, &format, pixels.data(), pixels.size()));

    EXPECT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec));
    EXPECT_EQ(0u, jxl::test::ComparePixels(frames[i].data(), pixels.data(),
                                           xsize, ysize, format, format));
  }

  // After all frames were decoded, JxlDecoderProcessInput should return
  // success to indicate all is done.
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderProcessInput(dec));

  // Test rewinding the decoder and skipping different frames

  JxlDecoderRewind(dec);
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSubscribeEvents(dec, JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE));
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, next_in, avail_in));

  for (size_t i = 0; i < num_frames; ++i) {
    int test_skipping = (i == 9) ? 3 : 0;
    std::vector<uint8_t> pixels(buffer_size);

    EXPECT_EQ(JXL_DEC_FRAME, JxlDecoderProcessInput(dec));

    // Since this is after JXL_DEC_FRAME but before JXL_DEC_FULL_IMAGE, this
    // should only skip the next frame, not the currently processed one.
    if (test_skipping) JxlDecoderSkipFrames(dec, test_skipping);

    JxlFrameHeader frame_header;
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetFrameHeader(dec, &frame_header));
    EXPECT_EQ(frame_durations[i], frame_header.duration);

    EXPECT_EQ(i + 1u == num_frames, frame_header.is_last);

    EXPECT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER, JxlDecoderProcessInput(dec));

    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetImageOutBuffer(
                                   dec, &format, pixels.data(), pixels.size()));

    EXPECT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec));
    EXPECT_EQ(0u, jxl::test::ComparePixels(frames[i].data(), pixels.data(),
                                           xsize, ysize, format, format));

    if (test_skipping) i += test_skipping;
  }

  JxlThreadParallelRunnerDestroy(runner);
  JxlDecoderDestroy(dec);
}

TEST(DecodeTest, SkipFrameWithBlendingTest) {
  JxlMemoryManager* memory_manager = jxl::test::MemoryManager();
  size_t xsize = 90;
  size_t ysize = 120;
  constexpr size_t num_frames = 16;
  std::vector<uint8_t> frames[num_frames];
  JxlPixelFormat format = {3, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};

  auto io = jxl::make_unique<jxl::CodecInOut>(memory_manager);
  ASSERT_TRUE(io->SetSize(xsize, ysize));
  io->metadata.m.SetUintSamples(16);
  io->metadata.m.color_encoding = jxl::ColorEncoding::SRGB(false);
  io->metadata.m.have_animation = true;
  io->frames.clear();
  io->frames.reserve(num_frames);
  ASSERT_TRUE(io->SetSize(xsize, ysize));

  std::vector<uint32_t> frame_durations(num_frames);

  for (size_t i = 0; i < num_frames; ++i) {
    if (i < 5) {
      std::vector<uint8_t> frame_internal =
          jxl::test::GetSomeTestImage(xsize, ysize, 3, i * 2 + 1);
      // An internal frame with 0 duration, and use_for_next_frame, this is a
      // frame that is not rendered and not output by the API, but on which the
      // rendered frames depend
      jxl::ImageBundle bundle_internal(memory_manager, &io->metadata.m);
      EXPECT_TRUE(ConvertFromExternal(
          jxl::Bytes(frame_internal.data(), frame_internal.size()), xsize,
          ysize, jxl::ColorEncoding::SRGB(/*is_gray=*/false),
          /*bits_per_sample=*/16, format,
          /*pool=*/nullptr, &bundle_internal));
      bundle_internal.duration = 0;
      bundle_internal.use_for_next_frame = true;
      io->frames.push_back(std::move(bundle_internal));
    }

    std::vector<uint8_t> frame =
        jxl::test::GetSomeTestImage(xsize, ysize, 3, i * 2);
    // Actual rendered frame
    frame_durations[i] = 5 + i;
    jxl::ImageBundle bundle(memory_manager, &io->metadata.m);
    EXPECT_TRUE(ConvertFromExternal(jxl::Bytes(frame.data(), frame.size()),
                                    xsize, ysize,
                                    jxl::ColorEncoding::SRGB(/*is_gray=*/false),
                                    /*bits_per_sample=*/16, format,
                                    /*pool=*/nullptr, &bundle));
    bundle.duration = frame_durations[i];
    // Create some variation in which frames depend on which.
    if (i != 3 && i != 9 && i != 10) {
      bundle.use_for_next_frame = true;
    }
    if (i != 12) {
      bundle.blend = true;
      // Choose a blend mode that depends on the pixels of the saved frame and
      // doesn't use alpha
      bundle.blendmode = jxl::BlendMode::kMul;
    }
    io->frames.push_back(std::move(bundle));
  }

  jxl::CompressParams cparams;
  cparams.SetLossless();  // Lossless to verify pixels exactly after roundtrip.
  cparams.speed_tier = jxl::SpeedTier::kThunder;
  std::vector<uint8_t> compressed;
  EXPECT_TRUE(jxl::test::EncodeFile(cparams, io.get(), &compressed));

  // Independently decode all frames without any skipping, to create the
  // expected blended frames, for the actual tests below to compare with.
  {
    JxlDecoder* dec = JxlDecoderCreate(nullptr);
    const uint8_t* next_in = compressed.data();
    size_t avail_in = compressed.size();

    void* runner = JxlThreadParallelRunnerCreate(
        nullptr, JxlThreadParallelRunnerDefaultNumWorkerThreads());
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetParallelRunner(
                                   dec, JxlThreadParallelRunner, runner));
    EXPECT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderSubscribeEvents(dec, JXL_DEC_FULL_IMAGE));
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, next_in, avail_in));
    for (auto& frame : frames) {
      EXPECT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER, JxlDecoderProcessInput(dec));
      frame.resize(xsize * ysize * 6);
      EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetImageOutBuffer(
                                     dec, &format, frame.data(), frame.size()));
      EXPECT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec));
    }

    // After all frames were decoded, JxlDecoderProcessInput should return
    // success to indicate all is done.
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderProcessInput(dec));
    JxlThreadParallelRunnerDestroy(runner);
    JxlDecoderDestroy(dec);
  }

  JxlDecoder* dec = JxlDecoderCreate(nullptr);
  const uint8_t* next_in = compressed.data();
  size_t avail_in = compressed.size();

  void* runner = JxlThreadParallelRunnerCreate(
      nullptr, JxlThreadParallelRunnerDefaultNumWorkerThreads());
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSetParallelRunner(dec, JxlThreadParallelRunner, runner));

  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSubscribeEvents(
                dec, JXL_DEC_BASIC_INFO | JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE));
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, next_in, avail_in));
  EXPECT_EQ(JXL_DEC_BASIC_INFO, JxlDecoderProcessInput(dec));
  size_t buffer_size;
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderImageOutBufferSize(dec, &format, &buffer_size));
  JxlBasicInfo info;
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBasicInfo(dec, &info));

  for (size_t i = 0; i < num_frames; ++i) {
    std::vector<uint8_t> pixels(buffer_size);

    EXPECT_EQ(JXL_DEC_FRAME, JxlDecoderProcessInput(dec));

    JxlFrameHeader frame_header;
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetFrameHeader(dec, &frame_header));
    EXPECT_EQ(frame_durations[i], frame_header.duration);

    EXPECT_EQ(i + 1u == num_frames, frame_header.is_last);

    EXPECT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER, JxlDecoderProcessInput(dec));

    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetImageOutBuffer(
                                   dec, &format, pixels.data(), pixels.size()));

    EXPECT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec));
    EXPECT_EQ(0u, jxl::test::ComparePixels(frames[i].data(), pixels.data(),
                                           xsize, ysize, format, format));

    // Test rewinding mid-way, not decoding all frames.
    if (i == 8) {
      break;
    }
  }

  JxlDecoderRewind(dec);
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSubscribeEvents(dec, JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE));
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, next_in, avail_in));

  for (size_t i = 0; i < num_frames; ++i) {
    if (i == 3) {
      JxlDecoderSkipFrames(dec, 5);
      i += 5;
    }
    std::vector<uint8_t> pixels(buffer_size);

    EXPECT_EQ(JXL_DEC_FRAME, JxlDecoderProcessInput(dec));

    JxlFrameHeader frame_header;
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetFrameHeader(dec, &frame_header));
    EXPECT_EQ(frame_durations[i], frame_header.duration);

    EXPECT_EQ(i + 1u == num_frames, frame_header.is_last);

    EXPECT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER, JxlDecoderProcessInput(dec));

    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetImageOutBuffer(
                                   dec, &format, pixels.data(), pixels.size()));

    EXPECT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec));
    EXPECT_EQ(0u, jxl::test::ComparePixels(frames[i].data(), pixels.data(),
                                           xsize, ysize, format, format));
  }

  // After all frames were decoded, JxlDecoderProcessInput should return
  // success to indicate all is done.
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderProcessInput(dec));

  // Test rewinding the decoder and skipping different frames

  JxlDecoderRewind(dec);
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSubscribeEvents(dec, JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE));
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, next_in, avail_in));

  for (size_t i = 0; i < num_frames; ++i) {
    int test_skipping = (i == 9) ? 3 : 0;
    std::vector<uint8_t> pixels(buffer_size);

    EXPECT_EQ(JXL_DEC_FRAME, JxlDecoderProcessInput(dec));

    // Since this is after JXL_DEC_FRAME but before JXL_DEC_FULL_IMAGE, this
    // should only skip the next frame, not the currently processed one.
    if (test_skipping) JxlDecoderSkipFrames(dec, test_skipping);

    JxlFrameHeader frame_header;
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetFrameHeader(dec, &frame_header));
    EXPECT_EQ(frame_durations[i], frame_header.duration);

    EXPECT_EQ(i + 1u == num_frames, frame_header.is_last);

    EXPECT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER, JxlDecoderProcessInput(dec));

    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetImageOutBuffer(
                                   dec, &format, pixels.data(), pixels.size()));

    EXPECT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec));
    EXPECT_EQ(0u, jxl::test::ComparePixels(frames[i].data(), pixels.data(),
                                           xsize, ysize, format, format));

    if (test_skipping) i += test_skipping;
  }

  JxlThreadParallelRunnerDestroy(runner);
  JxlDecoderDestroy(dec);
}

TEST(DecodeTest, SkipFrameWithAlphaBlendingTest) {
  JxlMemoryManager* memory_manager = jxl::test::MemoryManager();
  size_t xsize = 90;
  size_t ysize = 120;
  constexpr size_t num_frames = 16;
  std::vector<uint8_t> frames[num_frames + 5];
  JxlPixelFormat format = {4, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};

  auto io = jxl::make_unique<jxl::CodecInOut>(memory_manager);
  ASSERT_TRUE(io->SetSize(xsize, ysize));
  io->metadata.m.SetUintSamples(16);
  io->metadata.m.color_encoding = jxl::ColorEncoding::SRGB(false);
  io->metadata.m.have_animation = true;
  io->frames.clear();
  io->frames.reserve(num_frames + 5);
  ASSERT_TRUE(io->SetSize(xsize, ysize));

  std::vector<uint32_t> frame_durations_c;
  std::vector<uint32_t> frame_durations_nc;
  std::vector<uint32_t> frame_xsize;
  std::vector<uint32_t> frame_ysize;
  std::vector<uint32_t> frame_x0;
  std::vector<uint32_t> frame_y0;

  for (size_t i = 0; i < num_frames; ++i) {
    size_t cropxsize = 1 + xsize * 2 / (i + 1);
    size_t cropysize = 1 + ysize * 3 / (i + 2);
    int cropx0 = i * 3 - 8;
    int cropy0 = i * 4 - 7;
    if (i < 5) {
      std::vector<uint8_t> frame_internal =
          jxl::test::GetSomeTestImage(xsize / 2, ysize / 2, 4, i * 2 + 1);
      // An internal frame with 0 duration, and use_for_next_frame, this is a
      // frame that is not rendered and not output by default by the API, but on
      // which the rendered frames depend
      jxl::ImageBundle bundle_internal(memory_manager, &io->metadata.m);
      EXPECT_TRUE(ConvertFromExternal(
          jxl::Bytes(frame_internal.data(), frame_internal.size()), xsize / 2,
          ysize / 2, jxl::ColorEncoding::SRGB(/*is_gray=*/false),
          /*bits_per_sample=*/16, format,
          /*pool=*/nullptr, &bundle_internal));
      bundle_internal.duration = 0;
      bundle_internal.use_for_next_frame = true;
      bundle_internal.origin = {13, 17};
      io->frames.push_back(std::move(bundle_internal));
      frame_durations_nc.push_back(0);
      frame_xsize.push_back(xsize / 2);
      frame_ysize.push_back(ysize / 2);
      frame_x0.push_back(13);
      frame_y0.push_back(17);
    }

    std::vector<uint8_t> frame =
        jxl::test::GetSomeTestImage(cropxsize, cropysize, 4, i * 2);
    // Actual rendered frame
    jxl::ImageBundle bundle(memory_manager, &io->metadata.m);
    EXPECT_TRUE(ConvertFromExternal(jxl::Bytes(frame.data(), frame.size()),
                                    cropxsize, cropysize,
                                    jxl::ColorEncoding::SRGB(/*is_gray=*/false),
                                    /*bits_per_sample=*/16, format,
                                    /*pool=*/nullptr, &bundle));
    bundle.duration = 5 + i;
    frame_durations_nc.push_back(5 + i);
    frame_durations_c.push_back(5 + i);
    frame_xsize.push_back(cropxsize);
    frame_ysize.push_back(cropysize);
    frame_x0.push_back(cropx0);
    frame_y0.push_back(cropy0);
    bundle.origin = {cropx0, cropy0};
    // Create some variation in which frames depend on which.
    if (i != 3 && i != 9 && i != 10) {
      bundle.use_for_next_frame = true;
    }
    if (i != 12) {
      bundle.blend = true;
      bundle.blendmode = jxl::BlendMode::kBlend;
    }
    io->frames.push_back(std::move(bundle));
  }

  jxl::CompressParams cparams;
  cparams.SetLossless();  // Lossless to verify pixels exactly after roundtrip.
  cparams.speed_tier = jxl::SpeedTier::kThunder;
  std::vector<uint8_t> compressed;
  EXPECT_TRUE(jxl::test::EncodeFile(cparams, io.get(), &compressed));
  // try both with and without coalescing
  for (auto coalescing : {JXL_TRUE, JXL_FALSE}) {
    // Independently decode all frames without any skipping, to create the
    // expected blended frames, for the actual tests below to compare with.
    {
      JxlDecoder* dec = JxlDecoderCreate(nullptr);
      const uint8_t* next_in = compressed.data();
      size_t avail_in = compressed.size();
      EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetCoalescing(dec, coalescing));
      void* runner = JxlThreadParallelRunnerCreate(
          nullptr, JxlThreadParallelRunnerDefaultNumWorkerThreads());
      EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetParallelRunner(
                                     dec, JxlThreadParallelRunner, runner));
      EXPECT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderSubscribeEvents(dec, JXL_DEC_FULL_IMAGE));
      EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, next_in, avail_in));
      for (size_t i = 0; i < num_frames + (coalescing ? 0 : 5); ++i) {
        EXPECT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER, JxlDecoderProcessInput(dec));
        size_t buffer_size;
        EXPECT_EQ(JXL_DEC_SUCCESS,
                  JxlDecoderImageOutBufferSize(dec, &format, &buffer_size));
        if (coalescing) {
          EXPECT_EQ(xsize * ysize * 8u, buffer_size);
        } else {
          EXPECT_EQ(frame_xsize[i] * frame_ysize[i] * 8u, buffer_size);
        }
        frames[i].resize(buffer_size);
        EXPECT_EQ(JXL_DEC_SUCCESS,
                  JxlDecoderSetImageOutBuffer(dec, &format, frames[i].data(),
                                              frames[i].size()));
        EXPECT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec));
      }

      // After all frames were decoded, JxlDecoderProcessInput should return
      // success to indicate all is done.
      EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderProcessInput(dec));
      JxlThreadParallelRunnerDestroy(runner);
      JxlDecoderDestroy(dec);
    }

    JxlDecoder* dec = JxlDecoderCreate(nullptr);
    const uint8_t* next_in = compressed.data();
    size_t avail_in = compressed.size();

    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetCoalescing(dec, coalescing));
    void* runner = JxlThreadParallelRunnerCreate(
        nullptr, JxlThreadParallelRunnerDefaultNumWorkerThreads());
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetParallelRunner(
                                   dec, JxlThreadParallelRunner, runner));

    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSubscribeEvents(
                                   dec, JXL_DEC_BASIC_INFO | JXL_DEC_FRAME |
                                            JXL_DEC_FULL_IMAGE));
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, next_in, avail_in));
    EXPECT_EQ(JXL_DEC_BASIC_INFO, JxlDecoderProcessInput(dec));
    JxlBasicInfo info;
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBasicInfo(dec, &info));

    for (size_t i = 0; i < num_frames; ++i) {
      EXPECT_EQ(JXL_DEC_FRAME, JxlDecoderProcessInput(dec));

      size_t buffer_size;
      EXPECT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderImageOutBufferSize(dec, &format, &buffer_size));
      std::vector<uint8_t> pixels(buffer_size);

      JxlFrameHeader frame_header;
      EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetFrameHeader(dec, &frame_header));
      EXPECT_EQ((coalescing ? frame_durations_c[i] : frame_durations_nc[i]),
                frame_header.duration);

      EXPECT_EQ(i + 1u == num_frames, frame_header.is_last);

      EXPECT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER, JxlDecoderProcessInput(dec));

      EXPECT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderSetImageOutBuffer(dec, &format, pixels.data(),
                                            pixels.size()));

      EXPECT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec));
      if (coalescing) {
        EXPECT_EQ(frame_header.layer_info.xsize, xsize);
      } else {
        EXPECT_EQ(frame_header.layer_info.xsize, frame_xsize[i]);
      }
      if (coalescing) {
        EXPECT_EQ(frame_header.layer_info.ysize, ysize);
      } else {
        EXPECT_EQ(frame_header.layer_info.ysize, frame_ysize[i]);
      }
      EXPECT_EQ(0u, jxl::test::ComparePixels(frames[i].data(), pixels.data(),
                                             frame_header.layer_info.xsize,
                                             frame_header.layer_info.ysize,
                                             format, format));

      // Test rewinding mid-way, not decoding all frames.
      if (i == 8) {
        break;
      }
    }

    JxlDecoderRewind(dec);
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSubscribeEvents(
                                   dec, JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE));
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, next_in, avail_in));

    for (size_t i = 0; i < num_frames + (coalescing ? 0 : 5); ++i) {
      if (i == 3) {
        JxlDecoderSkipFrames(dec, 5);
        i += 5;
      }

      EXPECT_EQ(JXL_DEC_FRAME, JxlDecoderProcessInput(dec));
      size_t buffer_size;
      EXPECT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderImageOutBufferSize(dec, &format, &buffer_size));
      std::vector<uint8_t> pixels(buffer_size);

      JxlFrameHeader frame_header;
      EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetFrameHeader(dec, &frame_header));
      EXPECT_EQ((coalescing ? frame_durations_c[i] : frame_durations_nc[i]),
                frame_header.duration);

      EXPECT_EQ(i + 1u == num_frames + (coalescing ? 0u : 5u),
                frame_header.is_last);

      EXPECT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER, JxlDecoderProcessInput(dec));

      EXPECT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderSetImageOutBuffer(dec, &format, pixels.data(),
                                            pixels.size()));

      EXPECT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec));
      if (coalescing) {
        EXPECT_EQ(frame_header.layer_info.xsize, xsize);
        EXPECT_EQ(frame_header.layer_info.ysize, ysize);
        EXPECT_EQ(frame_header.layer_info.crop_x0, 0);
        EXPECT_EQ(frame_header.layer_info.crop_y0, 0);
      } else {
        EXPECT_EQ(frame_header.layer_info.xsize, frame_xsize[i]);
        EXPECT_EQ(frame_header.layer_info.ysize, frame_ysize[i]);
        EXPECT_EQ(frame_header.layer_info.crop_x0,
                  static_cast<int32_t>(frame_x0[i]));
        EXPECT_EQ(frame_header.layer_info.crop_y0,
                  static_cast<int32_t>(frame_y0[i]));
        EXPECT_EQ(frame_header.layer_info.blend_info.blendmode,
                  ((i != 17u) && (frame_header.duration != 0u))
                      ? 2
                      : 0);  // kBlend or the default kReplace
      }
      EXPECT_EQ(0u, jxl::test::ComparePixels(frames[i].data(), pixels.data(),
                                             frame_header.layer_info.xsize,
                                             frame_header.layer_info.ysize,
                                             format, format));
    }

    // After all frames were decoded, JxlDecoderProcessInput should return
    // success to indicate all is done.
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderProcessInput(dec));

    // Test rewinding the decoder and skipping different frames

    JxlDecoderRewind(dec);
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSubscribeEvents(
                                   dec, JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE));
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, next_in, avail_in));

    for (size_t i = 0; i < num_frames + (coalescing ? 0 : 5); ++i) {
      int test_skipping = (i == 9) ? 3 : 0;

      EXPECT_EQ(JXL_DEC_FRAME, JxlDecoderProcessInput(dec));
      size_t buffer_size;
      EXPECT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderImageOutBufferSize(dec, &format, &buffer_size));
      std::vector<uint8_t> pixels(buffer_size);

      // Since this is after JXL_DEC_FRAME but before JXL_DEC_FULL_IMAGE, this
      // should only skip the next frame, not the currently processed one.
      if (test_skipping) JxlDecoderSkipFrames(dec, test_skipping);

      JxlFrameHeader frame_header;
      EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetFrameHeader(dec, &frame_header));
      EXPECT_EQ((coalescing ? frame_durations_c[i] : frame_durations_nc[i]),
                frame_header.duration);

      EXPECT_EQ(i + 1u == num_frames + (coalescing ? 0u : 5u),
                frame_header.is_last);

      EXPECT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER, JxlDecoderProcessInput(dec));

      EXPECT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderSetImageOutBuffer(dec, &format, pixels.data(),
                                            pixels.size()));

      EXPECT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec));
      EXPECT_EQ(0u, jxl::test::ComparePixels(frames[i].data(), pixels.data(),
                                             frame_header.layer_info.xsize,
                                             frame_header.layer_info.ysize,
                                             format, format));

      if (test_skipping) i += test_skipping;
    }

    JxlThreadParallelRunnerDestroy(runner);
    JxlDecoderDestroy(dec);
  }
}

TEST(DecodeTest, OrientedCroppedFrameTest) {
  JxlMemoryManager* memory_manager = jxl::test::MemoryManager();
  const auto test = [&](bool keep_orientation, uint32_t orientation,
                        uint32_t resampling) {
    size_t xsize = 90;
    size_t ysize = 120;
    JxlPixelFormat format = {4, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};
    size_t oxsize = (!keep_orientation && orientation > 4 ? ysize : xsize);
    size_t oysize = (!keep_orientation && orientation > 4 ? xsize : ysize);
    auto io = jxl::make_unique<jxl::CodecInOut>(memory_manager);
    EXPECT_TRUE(io->SetSize(xsize, ysize));
    io->metadata.m.SetUintSamples(16);
    io->metadata.m.color_encoding = jxl::ColorEncoding::SRGB(false);
    io->metadata.m.orientation = orientation;
    io->frames.clear();
    EXPECT_TRUE(io->SetSize(xsize, ysize));

    for (size_t i = 0; i < 3; ++i) {
      size_t cropxsize = 1 + xsize * 2 / (i + 1);
      size_t cropysize = 1 + ysize * 3 / (i + 2);
      int cropx0 = i * 3 - 8;
      int cropy0 = i * 4 - 7;

      std::vector<uint8_t> frame =
          jxl::test::GetSomeTestImage(cropxsize, cropysize, 4, i * 2);
      jxl::ImageBundle bundle(memory_manager, &io->metadata.m);
      EXPECT_TRUE(ConvertFromExternal(
          jxl::Bytes(frame.data(), frame.size()), cropxsize, cropysize,
          jxl::ColorEncoding::SRGB(/*is_gray=*/false),
          /*bits_per_sample=*/16, format,
          /*pool=*/nullptr, &bundle));
      bundle.origin = {cropx0, cropy0};
      bundle.use_for_next_frame = true;
      io->frames.push_back(std::move(bundle));
    }

    jxl::CompressParams cparams;
    cparams
        .SetLossless();  // Lossless to verify pixels exactly after roundtrip.
    cparams.speed_tier = jxl::SpeedTier::kThunder;
    cparams.resampling = resampling;
    std::vector<uint8_t> compressed;
    EXPECT_TRUE(jxl::test::EncodeFile(cparams, io.get(), &compressed));

    // 0 is merged frame as decoded with coalescing enabled (default)
    // 1-3 are non-coalesced frames as decoded with coalescing disabled
    // 4 is the manually merged frame
    std::vector<uint8_t> frames[5];
    frames[4].resize(xsize * ysize * 8, 0);

    // try both with and without coalescing
    for (auto coalescing : {JXL_TRUE, JXL_FALSE}) {
      // Independently decode all frames without any skipping, to create the
      // expected blended frames, for the actual tests below to compare with.
      {
        JxlDecoder* dec = JxlDecoderCreate(nullptr);
        const uint8_t* next_in = compressed.data();
        size_t avail_in = compressed.size();
        EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetCoalescing(dec, coalescing));
        EXPECT_EQ(JXL_DEC_SUCCESS,
                  JxlDecoderSetKeepOrientation(dec, keep_orientation));
        void* runner = JxlThreadParallelRunnerCreate(
            nullptr, JxlThreadParallelRunnerDefaultNumWorkerThreads());
        EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetParallelRunner(
                                       dec, JxlThreadParallelRunner, runner));
        EXPECT_EQ(JXL_DEC_SUCCESS,
                  JxlDecoderSubscribeEvents(dec, JXL_DEC_FULL_IMAGE));
        EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, next_in, avail_in));
        for (size_t i = (coalescing ? 0 : 1); i < (coalescing ? 1 : 4); ++i) {
          EXPECT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER, JxlDecoderProcessInput(dec));
          JxlFrameHeader frame_header;
          EXPECT_EQ(JXL_DEC_SUCCESS,
                    JxlDecoderGetFrameHeader(dec, &frame_header));
          size_t buffer_size;
          EXPECT_EQ(JXL_DEC_SUCCESS,
                    JxlDecoderImageOutBufferSize(dec, &format, &buffer_size));
          if (coalescing) {
            EXPECT_EQ(xsize * ysize * 8u, buffer_size);
          } else {
            EXPECT_EQ(frame_header.layer_info.xsize *
                          frame_header.layer_info.ysize * 8u,
                      buffer_size);
          }
          frames[i].resize(buffer_size);
          EXPECT_EQ(JXL_DEC_SUCCESS,
                    JxlDecoderSetImageOutBuffer(dec, &format, frames[i].data(),
                                                frames[i].size()));
          EXPECT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec));
          EXPECT_EQ(frame_header.layer_info.blend_info.blendmode,
                    JXL_BLEND_REPLACE);
          if (coalescing) {
            EXPECT_EQ(frame_header.layer_info.xsize, oxsize);
            EXPECT_EQ(frame_header.layer_info.ysize, oysize);
            EXPECT_EQ(frame_header.layer_info.crop_x0, 0);
            EXPECT_EQ(frame_header.layer_info.crop_y0, 0);
          } else {
            // manually merge this layer
            int x0 = frame_header.layer_info.crop_x0;
            int y0 = frame_header.layer_info.crop_y0;
            int w = frame_header.layer_info.xsize;
            int h = frame_header.layer_info.ysize;
            for (int y = 0; y < static_cast<int>(oysize); y++) {
              if (y < y0 || y >= y0 + h) continue;
              // pointers do whole 16-bit RGBA pixels at a time
              uint64_t* row_merged = reinterpret_cast<uint64_t*>(
                  frames[4].data() + y * oxsize * 8);
              uint64_t* row_layer = reinterpret_cast<uint64_t*>(
                  frames[i].data() + (y - y0) * w * 8);
              for (int x = 0; x < static_cast<int>(oxsize); x++) {
                if (x < x0 || x >= x0 + w) continue;
                row_merged[x] = row_layer[x - x0];
              }
            }
          }
        }

        // After all frames were decoded, JxlDecoderProcessInput should return
        // success to indicate all is done.
        EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderProcessInput(dec));
        JxlThreadParallelRunnerDestroy(runner);
        JxlDecoderDestroy(dec);
      }
    }

    EXPECT_EQ(0u, jxl::test::ComparePixels(frames[0].data(), frames[4].data(),
                                           oxsize, oysize, format, format));
  };

  for (bool keep_orientation : {true, false}) {
    for (uint32_t orientation = 1; orientation <= 8; orientation++) {
      for (uint32_t resampling : {1, 2, 4, 8}) {
        SCOPED_TRACE(testing::Message()
                     << "keep_orientation: " << keep_orientation << ", "
                     << "orientation: " << orientation << ", "
                     << "resampling: " << resampling);
        test(keep_orientation, orientation, resampling);
      }
    }
  }
}

struct FramePositions {
  size_t frame_start;
  size_t header_end;
  size_t toc_end;
  std::vector<size_t> section_end;
};

struct StreamPositions {
  size_t codestream_start;
  size_t codestream_end;
  size_t basic_info;
  size_t jbrd_end = 0;
  std::vector<size_t> box_start;
  std::vector<FramePositions> frames;
};

void AnalyzeCodestream(const std::vector<uint8_t>& data,
                       StreamPositions* streampos) {
  JxlMemoryManager* memory_manager = jxl::test::MemoryManager();
  // Unbox data to codestream and mark where it is broken up by boxes.
  std::vector<uint8_t> codestream;
  std::vector<std::pair<size_t, size_t>> breakpoints;
  bool codestream_end = false;
  ASSERT_LE(2u, data.size());
  if (data[0] == 0xff && data[1] == 0x0a) {
    codestream = std::vector<uint8_t>(data.begin(), data.end());
    streampos->codestream_start = 0;
  } else {
    const uint8_t* in = data.data();
    size_t pos = 0;
    while (pos < data.size()) {
      ASSERT_LE(pos + 8u, data.size());
      streampos->box_start.push_back(pos);
      size_t box_size = LoadBE32(in + pos);
      if (box_size == 0) box_size = data.size() - pos;
      ASSERT_LE(pos + box_size, data.size());
      if (memcmp(in + pos + 4, "jxlc", 4) == 0) {
        EXPECT_TRUE(codestream.empty());
        streampos->codestream_start = pos + 8;
        codestream.insert(codestream.end(), in + pos + 8, in + pos + box_size);
        codestream_end = true;
      } else if (memcmp(in + pos + 4, "jxlp", 4) == 0) {
        codestream_end = ((LoadBE32(in + pos + 8) & 0x80000000) != 0);
        if (codestream.empty()) {
          streampos->codestream_start = pos + 12;
        } else if (box_size > 12 || !codestream_end) {
          breakpoints.emplace_back(codestream.size(), 12);
        }
        codestream.insert(codestream.end(), in + pos + 12, in + pos + box_size);
      } else if (memcmp(in + pos + 4, "jbrd", 4) == 0) {
        EXPECT_TRUE(codestream.empty());
        streampos->jbrd_end = pos + box_size;
      } else if (!codestream.empty() && !codestream_end) {
        breakpoints.emplace_back(codestream.size(), box_size);
      }
      pos += box_size;
    }
    ASSERT_EQ(pos, data.size());
  }
  // Translate codestream positions to boxed stream positions.
  size_t offset = streampos->codestream_start;
  size_t bp = 0;
  auto add_offset = [&](size_t pos) {
    while (bp < breakpoints.size() && pos >= breakpoints[bp].first) {
      offset += breakpoints[bp++].second;
    }
    return pos + offset;
  };
  // Analyze the unboxed codestream.
  jxl::BitReader br(jxl::Bytes(codestream.data(), codestream.size()));
  ASSERT_EQ(br.ReadFixedBits<16>(), 0x0AFFu);
  auto metadata = jxl::make_unique<jxl::CodecMetadata>();
  ASSERT_TRUE(ReadSizeHeader(&br, &metadata->size));
  ASSERT_TRUE(ReadImageMetadata(&br, &metadata->m));
  streampos->basic_info =
      add_offset(br.TotalBitsConsumed() / jxl::kBitsPerByte);
  metadata->transform_data.nonserialized_xyb_encoded = metadata->m.xyb_encoded;
  ASSERT_TRUE(jxl::Bundle::Read(&br, &metadata->transform_data));
  if (metadata->m.color_encoding.WantICC()) {
    std::vector<uint8_t> icc;
    ASSERT_TRUE(jxl::test::ReadICC(&br, &icc));
    ASSERT_TRUE(!icc.empty());
    metadata->m.color_encoding.SetICCRaw(std::move(icc));
  }
  ASSERT_TRUE(br.JumpToByteBoundary());
  bool has_preview = metadata->m.have_preview;
  while (br.TotalBitsConsumed() < br.TotalBytes() * jxl::kBitsPerByte) {
    FramePositions p;
    p.frame_start = add_offset(br.TotalBitsConsumed() / jxl::kBitsPerByte);
    jxl::FrameHeader frame_header(metadata.get());
    if (has_preview) {
      frame_header.nonserialized_is_preview = true;
      has_preview = false;
    }
    ASSERT_TRUE(ReadFrameHeader(&br, &frame_header));
    p.header_end =
        add_offset(jxl::DivCeil(br.TotalBitsConsumed(), jxl::kBitsPerByte));
    jxl::FrameDimensions frame_dim = frame_header.ToFrameDimensions();
    uint64_t groups_total_size;
    const size_t toc_entries =
        jxl::NumTocEntries(frame_dim.num_groups, frame_dim.num_dc_groups,
                           frame_header.passes.num_passes);
    std::vector<uint64_t> section_offsets;
    std::vector<uint32_t> section_sizes;
    ASSERT_TRUE(ReadGroupOffsets(memory_manager, toc_entries, &br,
                                 &section_offsets, &section_sizes,
                                 &groups_total_size));
    EXPECT_EQ(br.TotalBitsConsumed() % jxl::kBitsPerByte, 0u);
    size_t sections_start = br.TotalBitsConsumed() / jxl::kBitsPerByte;
    p.toc_end = add_offset(sections_start);
    for (size_t i = 0; i < toc_entries; ++i) {
      size_t end = sections_start + section_offsets[i] + section_sizes[i];
      p.section_end.push_back(add_offset(end));
    }
    br.SkipBits(groups_total_size * jxl::kBitsPerByte);
    streampos->frames.push_back(p);
  }
  streampos->codestream_end = add_offset(codestream.size());
  EXPECT_EQ(br.TotalBitsConsumed(), br.TotalBytes() * jxl::kBitsPerByte);
  EXPECT_TRUE(br.Close());
}

enum ExpectedFlushState { NO_FLUSH, SAME_FLUSH, NEW_FLUSH };
struct Breakpoint {
  size_t file_pos;
  ExpectedFlushState expect_flush;
};

void VerifyProgression(size_t xsize, size_t ysize, uint32_t num_channels,
                       const std::vector<uint8_t>& pixels,
                       const std::vector<uint8_t>& data,
                       std::vector<Breakpoint> breakpoints) {
  // Size large enough for multiple groups, required to have progressive stages.
  ASSERT_LT(256u, xsize);
  ASSERT_LT(256u, ysize);
  std::vector<uint8_t> pixels2;
  pixels2.resize(pixels.size());
  JxlPixelFormat format = {num_channels, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};
  JxlDecoder* dec = JxlDecoderCreate(nullptr);
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSubscribeEvents(
                dec, JXL_DEC_BASIC_INFO | JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE));
  size_t bp = 0;
  const uint8_t* next_in = data.data();
  size_t avail_in = breakpoints[bp].file_pos;
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, next_in, avail_in));
  double prev_dist = 1.0;
  for (;;) {
    JxlDecoderStatus status = JxlDecoderProcessInput(dec);
    printf("bp: %d  status: 0x%x\n", static_cast<int>(bp),
           static_cast<int>(status));
    if (status == JXL_DEC_BASIC_INFO) {
      JxlBasicInfo info;
      EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBasicInfo(dec, &info));
      EXPECT_EQ(info.xsize, xsize);
      EXPECT_EQ(info.ysize, ysize);
      // Output buffer/callback not yet set
      EXPECT_EQ(JXL_DEC_ERROR, JxlDecoderFlushImage(dec));
      size_t buffer_size;
      EXPECT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderImageOutBufferSize(dec, &format, &buffer_size));
      EXPECT_EQ(pixels2.size(), buffer_size);
      EXPECT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderSetImageOutBuffer(dec, &format, pixels2.data(),
                                            pixels2.size()));
    } else if (status == JXL_DEC_FRAME) {
      // Nothing to do.
    } else if (status == JXL_DEC_SUCCESS) {
      EXPECT_EQ(bp + 1u, breakpoints.size());
      break;
    } else if (status == JXL_DEC_NEED_MORE_INPUT ||
               status == JXL_DEC_FULL_IMAGE) {
      if (breakpoints[bp].expect_flush == NO_FLUSH) {
        EXPECT_EQ(JXL_DEC_ERROR, JxlDecoderFlushImage(dec));
      } else {
        if (status != JXL_DEC_FULL_IMAGE) {
          EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderFlushImage(dec));
        }
        double dist = jxl::test::DistanceRMS(pixels2.data(), pixels.data(),
                                             xsize, ysize, format);
        if (breakpoints[bp].expect_flush == NEW_FLUSH) {
          EXPECT_LT(dist, prev_dist);
          prev_dist = dist;
        } else {
          EXPECT_EQ(dist, prev_dist);
        }
      }
      if (status == JXL_DEC_FULL_IMAGE) {
        EXPECT_EQ(bp + 1u, breakpoints.size());
        continue;
      }
      bp++;
      ASSERT_LT(bp, breakpoints.size());
      next_in += avail_in - JxlDecoderReleaseInput(dec);
      avail_in = breakpoints[bp].file_pos - (next_in - data.data());
      EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, next_in, avail_in));
    } else {
      printf("Unexpected status: 0x%x\n", static_cast<int>(status));
      FAIL();  // unexpected returned status
    }
  }
  JxlDecoderDestroy(dec);
}

TEST(DecodeTest, ProgressionTest) {
  size_t xsize = 508;
  size_t ysize = 470;
  uint32_t num_channels = 3;
  std::vector<uint8_t> pixels =
      jxl::test::GetSomeTestImage(xsize, ysize, num_channels, 0);
  jxl::TestCodestreamParams params;
  params.cparams.progressive_dc = 1;
  params.preview_mode = jxl::kSmallPreview;
  std::vector<uint8_t> data =
      jxl::CreateTestJXLCodestream(jxl::Bytes(pixels.data(), pixels.size()),
                                   xsize, ysize, num_channels, params);
  StreamPositions streampos;
  AnalyzeCodestream(data, &streampos);
  const std::vector<FramePositions>& fp = streampos.frames;
  // We have preview, dc frame and regular frame.
  EXPECT_EQ(3u, fp.size());
  EXPECT_EQ(7u, fp[2].section_end.size());
  EXPECT_EQ(data.size(), fp[2].section_end[6]);
  std::vector<Breakpoint> breakpoints{
      {fp[0].frame_start, NO_FLUSH},           // headers
      {fp[1].frame_start, NO_FLUSH},           // preview
      {fp[2].frame_start, NO_FLUSH},           // dc frame
      {fp[2].section_end[0], NO_FLUSH},        // DC global
      {fp[2].section_end[1] - 1, NO_FLUSH},    // partial DC group
      {fp[2].section_end[1], NEW_FLUSH},       // DC group
      {fp[2].section_end[2], SAME_FLUSH},      // AC global
      {fp[2].section_end[3], NEW_FLUSH},       // AC group 0
      {fp[2].section_end[4] - 1, SAME_FLUSH},  // partial AC group 1
      {fp[2].section_end[4], NEW_FLUSH},       // AC group 1
      {fp[2].section_end[5], NEW_FLUSH},       // AC group 2
      {data.size() - 1, SAME_FLUSH},           // partial AC group 3
      {data.size(), NEW_FLUSH}};               // full image
  VerifyProgression(xsize, ysize, num_channels, pixels, data, breakpoints);
}

TEST(DecodeTest, ProgressionTestLosslessAlpha) {
  size_t xsize = 508;
  size_t ysize = 470;
  uint32_t num_channels = 4;
  std::vector<uint8_t> pixels =
      jxl::test::GetSomeTestImage(xsize, ysize, num_channels, 0);
  jxl::TestCodestreamParams params;
  params.cparams.SetLossless();
  params.cparams.speed_tier = jxl::SpeedTier::kThunder;
  params.cparams.responsive = 1;
  std::vector<uint8_t> data =
      jxl::CreateTestJXLCodestream(jxl::Bytes(pixels.data(), pixels.size()),
                                   xsize, ysize, num_channels, params);
  StreamPositions streampos;
  AnalyzeCodestream(data, &streampos);
  const std::vector<FramePositions>& fp = streampos.frames;
  // We have preview, dc frame and regular frame.
  EXPECT_EQ(1u, fp.size());
  EXPECT_EQ(7u, fp[0].section_end.size());
  EXPECT_EQ(data.size(), fp[0].section_end[6]);
  std::vector<Breakpoint> breakpoints{
      {fp[0].frame_start, NO_FLUSH},           // headers
      {fp[0].section_end[0] - 1, NO_FLUSH},    // partial DC global
      {fp[0].section_end[0], NEW_FLUSH},       // DC global
      {fp[0].section_end[1], SAME_FLUSH},      // DC group
      {fp[0].section_end[2], SAME_FLUSH},      // AC global
      {fp[0].section_end[3], NEW_FLUSH},       // AC group 0
      {fp[0].section_end[4] - 1, SAME_FLUSH},  // partial AC group 1
      {fp[0].section_end[4], NEW_FLUSH},       // AC group 1
      {fp[0].section_end[5], NEW_FLUSH},       // AC group 2
      {data.size() - 1, SAME_FLUSH},           // partial AC group 3
      {data.size(), NEW_FLUSH}};               // full image
  VerifyProgression(xsize, ysize, num_channels, pixels, data, breakpoints);
}

void VerifyFilePosition(size_t expected_pos, const std::vector<uint8_t>& data,
                        JxlDecoder* dec) {
  size_t remaining = JxlDecoderReleaseInput(dec);
  size_t pos = data.size() - remaining;
  EXPECT_EQ(expected_pos, pos);
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSetInput(dec, data.data() + pos, remaining));
}

TEST(DecodeTest, InputHandlingTestOneShot) {
  size_t xsize = 508;
  size_t ysize = 470;
  uint32_t num_channels = 3;
  std::vector<uint8_t> pixels =
      jxl::test::GetSomeTestImage(xsize, ysize, num_channels, 0);
  for (int i = 0; i < kCSBF_NUM_ENTRIES; ++i) {
    printf("Testing with box format %d\n", i);
    jxl::TestCodestreamParams params;
    params.cparams.progressive_dc = 1;
    params.preview_mode = jxl::kSmallPreview;
    params.box_format = static_cast<CodeStreamBoxFormat>(i);
    std::vector<uint8_t> data =
        jxl::CreateTestJXLCodestream(jxl::Bytes(pixels.data(), pixels.size()),
                                     xsize, ysize, num_channels, params);
    JxlPixelFormat format = {num_channels, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};
    StreamPositions streampos;
    AnalyzeCodestream(data, &streampos);
    const std::vector<FramePositions>& fp = streampos.frames;
    // We have preview, dc frame and regular frame.
    EXPECT_EQ(3u, fp.size());

    std::vector<uint8_t> pixels2;
    pixels2.resize(pixels.size());

    int kNumEvents = 6;
    int events[] = {
        JXL_DEC_BASIC_INFO, JXL_DEC_COLOR_ENCODING, JXL_DEC_PREVIEW_IMAGE,
        JXL_DEC_FRAME,      JXL_DEC_FULL_IMAGE,     JXL_DEC_FRAME_PROGRESSION,
    };
    size_t end_positions[] = {
        streampos.basic_info,     fp[0].frame_start,
        fp[1].frame_start,        fp[2].toc_end,
        streampos.codestream_end, streampos.codestream_end};
    int events_wanted = 0;
    for (int j = 0; j < kNumEvents; ++j) {
      events_wanted |= events[j];
      size_t end_pos = end_positions[j];
      JxlDecoder* dec = JxlDecoderCreate(nullptr);
      EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSubscribeEvents(dec, events_wanted));
      EXPECT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderSetInput(dec, data.data(), data.size()));
      EXPECT_EQ(JXL_DEC_BASIC_INFO, JxlDecoderProcessInput(dec));
      VerifyFilePosition(streampos.basic_info, data, dec);
      if (j >= 1) {
        EXPECT_EQ(JXL_DEC_COLOR_ENCODING, JxlDecoderProcessInput(dec));
        VerifyFilePosition(fp[0].frame_start, data, dec);
      }
      if (j >= 2) {
        EXPECT_EQ(JXL_DEC_NEED_PREVIEW_OUT_BUFFER, JxlDecoderProcessInput(dec));
        VerifyFilePosition(fp[0].toc_end, data, dec);
        size_t buffer_size;
        EXPECT_EQ(JXL_DEC_SUCCESS,
                  JxlDecoderPreviewOutBufferSize(dec, &format, &buffer_size));
        EXPECT_GE(pixels2.size(), buffer_size);
        EXPECT_EQ(JXL_DEC_SUCCESS,
                  JxlDecoderSetPreviewOutBuffer(dec, &format, pixels2.data(),
                                                buffer_size));
        EXPECT_EQ(JXL_DEC_PREVIEW_IMAGE, JxlDecoderProcessInput(dec));
        VerifyFilePosition(fp[1].frame_start, data, dec);
      }
      if (j >= 3) {
        EXPECT_EQ(JXL_DEC_FRAME, JxlDecoderProcessInput(dec));
        VerifyFilePosition(fp[2].toc_end, data, dec);
        if (j >= 5) {
          EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetProgressiveDetail(dec, kDC));
        }
      }
      if (j >= 4) {
        EXPECT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER, JxlDecoderProcessInput(dec));
        VerifyFilePosition(fp[2].toc_end, data, dec);
        size_t buffer_size;
        EXPECT_EQ(JXL_DEC_SUCCESS,
                  JxlDecoderImageOutBufferSize(dec, &format, &buffer_size));
        EXPECT_EQ(pixels2.size(), buffer_size);
        EXPECT_EQ(JXL_DEC_SUCCESS,
                  JxlDecoderSetImageOutBuffer(dec, &format, pixels2.data(),
                                              pixels2.size()));
        if (j >= 5) {
          EXPECT_EQ(JXL_DEC_FRAME_PROGRESSION, JxlDecoderProcessInput(dec));
          VerifyFilePosition(fp[2].section_end[1], data, dec);
        }
        EXPECT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec));
        VerifyFilePosition(streampos.codestream_end, data, dec);
      }
      EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderProcessInput(dec));
      VerifyFilePosition(end_pos, data, dec);
      JxlDecoderDestroy(dec);
    }
  }
}

JXL_TRANSCODE_JPEG_TEST(DecodeTest, InputHandlingTestJPEGOneshot) {
  TEST_LIBJPEG_SUPPORT();
  size_t xsize = 123;
  size_t ysize = 77;
  size_t channels = 3;
  std::vector<uint8_t> pixels =
      jxl::test::GetSomeTestImage(xsize, ysize, channels, /*seed=*/0);
  for (int i = 1; i < kCSBF_NUM_ENTRIES; ++i) {
    printf("Testing with box format %d\n", i);
    std::vector<uint8_t> jpeg_codestream;
    jxl::TestCodestreamParams params;
    params.cparams.color_transform = jxl::ColorTransform::kNone;
    params.jpeg_codestream = &jpeg_codestream;
    params.preview_mode = jxl::kSmallPreview;
    params.box_format = static_cast<CodeStreamBoxFormat>(i);
    std::vector<uint8_t> data =
        jxl::CreateTestJXLCodestream(jxl::Bytes(pixels.data(), pixels.size()),
                                     xsize, ysize, channels, params);
    JxlPixelFormat format = {3, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};
    StreamPositions streampos;
    AnalyzeCodestream(data, &streampos);
    const std::vector<FramePositions>& fp = streampos.frames;
    // We have preview and regular frame.
    EXPECT_EQ(2u, fp.size());
    EXPECT_LT(0u, streampos.jbrd_end);

    std::vector<uint8_t> pixels2;
    pixels2.resize(pixels.size());

    int kNumEvents = 6;
    int events[] = {JXL_DEC_BASIC_INFO,     JXL_DEC_JPEG_RECONSTRUCTION,
                    JXL_DEC_COLOR_ENCODING, JXL_DEC_PREVIEW_IMAGE,
                    JXL_DEC_FRAME,          JXL_DEC_FULL_IMAGE};
    size_t end_positions[] = {streampos.basic_info, streampos.basic_info,
                              fp[0].frame_start,    fp[1].frame_start,
                              fp[1].toc_end,        streampos.codestream_end};
    int events_wanted = 0;
    for (int j = 0; j < kNumEvents; ++j) {
      printf("j = %d\n", j);
      events_wanted |= events[j];
      size_t end_pos = end_positions[j];
      JxlDecoder* dec = JxlDecoderCreate(nullptr);
      EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSubscribeEvents(dec, events_wanted));
      EXPECT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderSetInput(dec, data.data(), data.size()));
      if (j >= 1) {
        EXPECT_EQ(JXL_DEC_JPEG_RECONSTRUCTION, JxlDecoderProcessInput(dec));
        VerifyFilePosition(streampos.jbrd_end, data, dec);
      }
      EXPECT_EQ(JXL_DEC_BASIC_INFO, JxlDecoderProcessInput(dec));
      VerifyFilePosition(streampos.basic_info, data, dec);
      if (j >= 2) {
        EXPECT_EQ(JXL_DEC_COLOR_ENCODING, JxlDecoderProcessInput(dec));
        VerifyFilePosition(fp[0].frame_start, data, dec);
      }
      if (j >= 3) {
        EXPECT_EQ(JXL_DEC_NEED_PREVIEW_OUT_BUFFER, JxlDecoderProcessInput(dec));
        VerifyFilePosition(fp[0].toc_end, data, dec);
        size_t buffer_size;
        EXPECT_EQ(JXL_DEC_SUCCESS,
                  JxlDecoderPreviewOutBufferSize(dec, &format, &buffer_size));
        EXPECT_GE(pixels2.size(), buffer_size);
        EXPECT_EQ(JXL_DEC_SUCCESS,
                  JxlDecoderSetPreviewOutBuffer(dec, &format, pixels2.data(),
                                                buffer_size));
        EXPECT_EQ(JXL_DEC_PREVIEW_IMAGE, JxlDecoderProcessInput(dec));
        VerifyFilePosition(fp[1].frame_start, data, dec);
      }
      if (j >= 4) {
        EXPECT_EQ(JXL_DEC_FRAME, JxlDecoderProcessInput(dec));
        VerifyFilePosition(fp[1].toc_end, data, dec);
      }
      if (j >= 5) {
        EXPECT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER, JxlDecoderProcessInput(dec));
        VerifyFilePosition(fp[1].toc_end, data, dec);
        size_t buffer_size;
        EXPECT_EQ(JXL_DEC_SUCCESS,
                  JxlDecoderImageOutBufferSize(dec, &format, &buffer_size));
        EXPECT_EQ(pixels2.size(), buffer_size);
        EXPECT_EQ(JXL_DEC_SUCCESS,
                  JxlDecoderSetImageOutBuffer(dec, &format, pixels2.data(),
                                              pixels2.size()));
        EXPECT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec));
        VerifyFilePosition(streampos.codestream_end, data, dec);
      }
      EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderProcessInput(dec));
      VerifyFilePosition(end_pos, data, dec);
      JxlDecoderDestroy(dec);
    }
  }
}

TEST(DecodeTest, InputHandlingTestStreaming) {
  size_t xsize = 508;
  size_t ysize = 470;
  uint32_t num_channels = 3;
  std::vector<uint8_t> pixels =
      jxl::test::GetSomeTestImage(xsize, ysize, num_channels, 0);
  for (int i = 0; i < kCSBF_NUM_ENTRIES; ++i) {
    printf("Testing with box format %d\n", i);
    fflush(stdout);
    jxl::TestCodestreamParams params;
    params.cparams.progressive_dc = 1;
    params.box_format = static_cast<CodeStreamBoxFormat>(i);
    params.preview_mode = jxl::kSmallPreview;
    std::vector<uint8_t> data =
        jxl::CreateTestJXLCodestream(jxl::Bytes(pixels.data(), pixels.size()),
                                     xsize, ysize, num_channels, params);
    JxlPixelFormat format = {num_channels, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};
    StreamPositions streampos;
    AnalyzeCodestream(data, &streampos);
    const std::vector<FramePositions>& fp = streampos.frames;
    // We have preview, dc frame and regular frame.
    EXPECT_EQ(3u, fp.size());
    std::vector<uint8_t> pixels2;
    pixels2.resize(pixels.size());
    int events_wanted =
        (JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_PREVIEW_IMAGE |
         JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE | JXL_DEC_FRAME_PROGRESSION |
         JXL_DEC_BOX);
    for (size_t increment : {1, 7, 27, 1024}) {
      JxlDecoder* dec = JxlDecoderCreate(nullptr);
      EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSubscribeEvents(dec, events_wanted));
      size_t file_pos = 0;
      size_t box_index = 0;
      size_t avail_in = 0;
      for (;;) {
        const uint8_t* next_in = data.data() + file_pos;
        EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, next_in, avail_in));
        JxlDecoderStatus status = JxlDecoderProcessInput(dec);
        size_t remaining = JxlDecoderReleaseInput(dec);
        size_t consumed = avail_in - remaining;
        file_pos += consumed;
        avail_in += increment;
        avail_in = std::min<size_t>(avail_in, data.size() - file_pos);
        if (status == JXL_DEC_BASIC_INFO) {
          EXPECT_EQ(file_pos, streampos.basic_info);
        } else if (status == JXL_DEC_COLOR_ENCODING) {
          EXPECT_EQ(file_pos, streampos.frames[0].frame_start);
        } else if (status == JXL_DEC_NEED_PREVIEW_OUT_BUFFER) {
          EXPECT_EQ(file_pos, streampos.frames[0].toc_end);
          size_t buffer_size;
          EXPECT_EQ(JXL_DEC_SUCCESS,
                    JxlDecoderPreviewOutBufferSize(dec, &format, &buffer_size));
          EXPECT_GE(pixels2.size(), buffer_size);
          EXPECT_EQ(JXL_DEC_SUCCESS,
                    JxlDecoderSetPreviewOutBuffer(dec, &format, pixels2.data(),
                                                  buffer_size));
        } else if (status == JXL_DEC_PREVIEW_IMAGE) {
          EXPECT_EQ(file_pos, streampos.frames[1].frame_start);
        } else if (status == JXL_DEC_FRAME) {
          EXPECT_EQ(file_pos, streampos.frames[2].toc_end);
          EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetProgressiveDetail(dec, kDC));
        } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
          EXPECT_EQ(file_pos, streampos.frames[2].toc_end);
          size_t buffer_size;
          EXPECT_EQ(JXL_DEC_SUCCESS,
                    JxlDecoderImageOutBufferSize(dec, &format, &buffer_size));
          EXPECT_EQ(pixels2.size(), buffer_size);
          EXPECT_EQ(JXL_DEC_SUCCESS,
                    JxlDecoderSetImageOutBuffer(dec, &format, pixels2.data(),
                                                pixels2.size()));
        } else if (status == JXL_DEC_FRAME_PROGRESSION) {
          EXPECT_EQ(file_pos, streampos.frames[2].section_end[1]);
        } else if (status == JXL_DEC_FULL_IMAGE) {
          EXPECT_EQ(file_pos, streampos.codestream_end);
        } else if (status == JXL_DEC_SUCCESS) {
          EXPECT_EQ(file_pos, streampos.codestream_end);
          break;
        } else if (status == JXL_DEC_NEED_MORE_INPUT) {
          EXPECT_LT(remaining, 12u);
          if ((i == kCSBF_None && file_pos >= 2u) ||
              (box_index > 0 && box_index < streampos.box_start.size() &&
               file_pos >= streampos.box_start[box_index - 1] + 12 &&
               file_pos < streampos.box_start[box_index])) {
            EXPECT_EQ(remaining, 0u);
          }
          if (file_pos == data.size()) break;
        } else if (status == JXL_DEC_BOX) {
          ASSERT_LT(box_index, streampos.box_start.size());
          EXPECT_EQ(file_pos, streampos.box_start[box_index++]);
        } else {
          printf("Unexpected status: 0x%x\n", static_cast<int>(status));
          FAIL();
        }
      }
      JxlDecoderDestroy(dec);
    }
  }
}

TEST(DecodeTest, FlushTest) {
  // Size large enough for multiple groups, required to have progressive
  // stages
  size_t xsize = 333;
  size_t ysize = 300;
  uint32_t num_channels = 3;
  std::vector<uint8_t> pixels =
      jxl::test::GetSomeTestImage(xsize, ysize, num_channels, 0);
  jxl::TestCodestreamParams params;
  params.preview_mode = jxl::kSmallPreview;
  std::vector<uint8_t> data =
      jxl::CreateTestJXLCodestream(jxl::Bytes(pixels.data(), pixels.size()),
                                   xsize, ysize, num_channels, params);
  JxlPixelFormat format = {num_channels, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};

  std::vector<uint8_t> pixels2;
  pixels2.resize(pixels.size());

  JxlDecoder* dec = JxlDecoderCreate(nullptr);

  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSubscribeEvents(
                dec, JXL_DEC_BASIC_INFO | JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE));

  // Ensure that the first part contains at least the full DC of the image,
  // otherwise flush does not work.
  size_t first_part = data.size() - 1;

  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, data.data(), first_part));

  EXPECT_EQ(JXL_DEC_BASIC_INFO, JxlDecoderProcessInput(dec));
  JxlBasicInfo info;
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBasicInfo(dec, &info));
  EXPECT_EQ(info.xsize, xsize);
  EXPECT_EQ(info.ysize, ysize);

  EXPECT_EQ(JXL_DEC_FRAME, JxlDecoderProcessInput(dec));

  // Output buffer not yet set
  EXPECT_EQ(JXL_DEC_ERROR, JxlDecoderFlushImage(dec));

  size_t buffer_size;
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderImageOutBufferSize(dec, &format, &buffer_size));
  EXPECT_EQ(pixels2.size(), buffer_size);
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetImageOutBuffer(
                                 dec, &format, pixels2.data(), pixels2.size()));

  // Must process input further until we get JXL_DEC_NEED_MORE_INPUT, even if
  // data was already input before, since the processing of the frame only
  // happens at the JxlDecoderProcessInput call after JXL_DEC_FRAME.
  EXPECT_EQ(JXL_DEC_NEED_MORE_INPUT, JxlDecoderProcessInput(dec));

  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderFlushImage(dec));

  // Crude test of actual pixel data: pixel threshold of about 4% (2560/65535).
  // 29000 pixels can be above the threshold
  EXPECT_LE(jxl::test::ComparePixels(pixels2.data(), pixels.data(), xsize,
                                     ysize, format, format, 2560.0),
            29000u);

  EXPECT_EQ(JXL_DEC_NEED_MORE_INPUT, JxlDecoderProcessInput(dec));

  size_t consumed = first_part - JxlDecoderReleaseInput(dec);

  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, data.data() + consumed,
                                                data.size() - consumed));
  EXPECT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec));
  // Lower threshold for the final (still lossy) image
  EXPECT_LE(jxl::test::ComparePixels(pixels2.data(), pixels.data(), xsize,
                                     ysize, format, format, 2560.0),
            24000u);

  JxlDecoderDestroy(dec);
}

TEST(DecodeTest, FlushTestImageOutCallback) {
  // Size large enough for multiple groups, required to have progressive
  // stages
  size_t xsize = 333;
  size_t ysize = 300;
  uint32_t num_channels = 3;
  std::vector<uint8_t> pixels =
      jxl::test::GetSomeTestImage(xsize, ysize, num_channels, 0);
  jxl::TestCodestreamParams params;
  params.preview_mode = jxl::kSmallPreview;
  std::vector<uint8_t> data =
      jxl::CreateTestJXLCodestream(jxl::Bytes(pixels.data(), pixels.size()),
                                   xsize, ysize, num_channels, params);
  JxlPixelFormat format = {num_channels, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};

  std::vector<uint8_t> pixels2;
  pixels2.resize(pixels.size());

  size_t bytes_per_pixel = format.num_channels * 2;
  size_t stride = bytes_per_pixel * xsize;
  auto callback = [&](size_t x, size_t y, size_t num_pixels,
                      const void* pixels_row) {
    memcpy(pixels2.data() + stride * y + bytes_per_pixel * x, pixels_row,
           num_pixels * bytes_per_pixel);
  };

  JxlDecoder* dec = JxlDecoderCreate(nullptr);

  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSubscribeEvents(
                dec, JXL_DEC_BASIC_INFO | JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE));

  // Ensure that the first part contains at least the full DC of the image,
  // otherwise flush does not work.
  size_t first_part = data.size() - 1;

  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, data.data(), first_part));

  EXPECT_EQ(JXL_DEC_BASIC_INFO, JxlDecoderProcessInput(dec));
  JxlBasicInfo info;
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBasicInfo(dec, &info));
  EXPECT_EQ(info.xsize, xsize);
  EXPECT_EQ(info.ysize, ysize);

  EXPECT_EQ(JXL_DEC_FRAME, JxlDecoderProcessInput(dec));

  // Output callback not yet set
  EXPECT_EQ(JXL_DEC_ERROR, JxlDecoderFlushImage(dec));

  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetImageOutCallback(
                                 dec, &format,
                                 [](void* opaque, size_t x, size_t y,
                                    size_t xsize, const void* pixels_row) {
                                   auto cb =
                                       static_cast<decltype(&callback)>(opaque);
                                   (*cb)(x, y, xsize, pixels_row);
                                 },
                                 /*opaque=*/&callback));

  // Must process input further until we get JXL_DEC_NEED_MORE_INPUT, even if
  // data was already input before, since the processing of the frame only
  // happens at the JxlDecoderProcessInput call after JXL_DEC_FRAME.
  EXPECT_EQ(JXL_DEC_NEED_MORE_INPUT, JxlDecoderProcessInput(dec));

  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderFlushImage(dec));

  // Crude test of actual pixel data: pixel threshold of about 4% (2560/65535).
  // 29000 pixels can be above the threshold
  EXPECT_LE(jxl::test::ComparePixels(pixels2.data(), pixels.data(), xsize,
                                     ysize, format, format, 2560.0),
            29000u);

  EXPECT_EQ(JXL_DEC_NEED_MORE_INPUT, JxlDecoderProcessInput(dec));

  size_t consumed = first_part - JxlDecoderReleaseInput(dec);

  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, data.data() + consumed,
                                                data.size() - consumed));
  EXPECT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec));
  // Lower threshold for the final (still lossy) image
  EXPECT_LE(jxl::test::ComparePixels(pixels2.data(), pixels.data(), xsize,
                                     ysize, format, format, 2560.0),
            24000u);

  JxlDecoderDestroy(dec);
}

TEST(DecodeTest, FlushTestLossyProgressiveAlpha) {
  // Size large enough for multiple groups, required to have progressive
  // stages
  size_t xsize = 333;
  size_t ysize = 300;
  uint32_t num_channels = 4;
  std::vector<uint8_t> pixels =
      jxl::test::GetSomeTestImage(xsize, ysize, num_channels, 0);
  jxl::TestCodestreamParams params;
  params.preview_mode = jxl::kSmallPreview;
  std::vector<uint8_t> data =
      jxl::CreateTestJXLCodestream(jxl::Bytes(pixels.data(), pixels.size()),
                                   xsize, ysize, num_channels, params);
  JxlPixelFormat format = {num_channels, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};

  std::vector<uint8_t> pixels2;
  pixels2.resize(pixels.size());

  JxlDecoder* dec = JxlDecoderCreate(nullptr);

  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSubscribeEvents(
                dec, JXL_DEC_BASIC_INFO | JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE));

  // Ensure that the first part contains at least the full DC of the image,
  // otherwise flush does not work.
  size_t first_part = data.size() - 1;

  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, data.data(), first_part));

  EXPECT_EQ(JXL_DEC_BASIC_INFO, JxlDecoderProcessInput(dec));
  JxlBasicInfo info;
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBasicInfo(dec, &info));
  EXPECT_EQ(info.xsize, xsize);
  EXPECT_EQ(info.ysize, ysize);

  EXPECT_EQ(JXL_DEC_FRAME, JxlDecoderProcessInput(dec));

  // Output buffer not yet set
  EXPECT_EQ(JXL_DEC_ERROR, JxlDecoderFlushImage(dec));

  size_t buffer_size;
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderImageOutBufferSize(dec, &format, &buffer_size));
  EXPECT_EQ(pixels2.size(), buffer_size);
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetImageOutBuffer(
                                 dec, &format, pixels2.data(), pixels2.size()));

  // Must process input further until we get JXL_DEC_NEED_MORE_INPUT, even if
  // data was already input before, since the processing of the frame only
  // happens at the JxlDecoderProcessInput call after JXL_DEC_FRAME.
  EXPECT_EQ(JXL_DEC_NEED_MORE_INPUT, JxlDecoderProcessInput(dec));

  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderFlushImage(dec));

  EXPECT_LE(jxl::test::ComparePixels(pixels2.data(), pixels.data(), xsize,
                                     ysize, format, format, 2560.0),
            30000u);

  EXPECT_EQ(JXL_DEC_NEED_MORE_INPUT, JxlDecoderProcessInput(dec));

  size_t consumed = first_part - JxlDecoderReleaseInput(dec);

  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, data.data() + consumed,
                                                data.size() - consumed));

  EXPECT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec));
  EXPECT_LE(jxl::test::ComparePixels(pixels2.data(), pixels.data(), xsize,
                                     ysize, format, format, 2560.0),
            24000u);

  JxlDecoderDestroy(dec);
}
TEST(DecodeTest, FlushTestLossyProgressiveAlphaUpsampling) {
  size_t xsize = 533;
  size_t ysize = 401;
  uint32_t num_channels = 4;
  std::vector<uint8_t> pixels =
      jxl::test::GetSomeTestImage(xsize, ysize, num_channels, 0);
  jxl::TestCodestreamParams params;
  params.cparams.resampling = 2;
  params.cparams.ec_resampling = 4;
  params.preview_mode = jxl::kSmallPreview;
  std::vector<uint8_t> data =
      jxl::CreateTestJXLCodestream(jxl::Bytes(pixels.data(), pixels.size()),
                                   xsize, ysize, num_channels, params);
  JxlPixelFormat format = {num_channels, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};

  std::vector<uint8_t> pixels2;
  pixels2.resize(pixels.size());

  JxlDecoder* dec = JxlDecoderCreate(nullptr);

  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSubscribeEvents(
                dec, JXL_DEC_BASIC_INFO | JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE));

  // Ensure that the first part contains at least the full DC of the image,
  // otherwise flush does not work.
  size_t first_part = data.size() * 2 / 3;

  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, data.data(), first_part));

  EXPECT_EQ(JXL_DEC_BASIC_INFO, JxlDecoderProcessInput(dec));
  JxlBasicInfo info;
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBasicInfo(dec, &info));
  EXPECT_EQ(info.xsize, xsize);
  EXPECT_EQ(info.ysize, ysize);

  EXPECT_EQ(JXL_DEC_FRAME, JxlDecoderProcessInput(dec));

  // Output buffer not yet set
  EXPECT_EQ(JXL_DEC_ERROR, JxlDecoderFlushImage(dec));

  size_t buffer_size;
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderImageOutBufferSize(dec, &format, &buffer_size));
  EXPECT_EQ(pixels2.size(), buffer_size);
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetImageOutBuffer(
                                 dec, &format, pixels2.data(), pixels2.size()));

  // Must process input further until we get JXL_DEC_NEED_MORE_INPUT, even if
  // data was already input before, since the processing of the frame only
  // happens at the JxlDecoderProcessInput call after JXL_DEC_FRAME.
  EXPECT_EQ(JXL_DEC_NEED_MORE_INPUT, JxlDecoderProcessInput(dec));

  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderFlushImage(dec));

  EXPECT_LE(jxl::test::ComparePixels(pixels2.data(), pixels.data(), xsize,
                                     ysize, format, format, 2560.0),
            125000u);

  EXPECT_EQ(JXL_DEC_NEED_MORE_INPUT, JxlDecoderProcessInput(dec));

  size_t consumed = first_part - JxlDecoderReleaseInput(dec);

  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, data.data() + consumed,
                                                data.size() - consumed));

  EXPECT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec));
  EXPECT_LE(jxl::test::ComparePixels(pixels2.data(), pixels.data(), xsize,
                                     ysize, format, format, 2560.0),
            70000u);

  JxlDecoderDestroy(dec);
}
TEST(DecodeTest, FlushTestLosslessProgressiveAlpha) {
  // Size large enough for multiple groups, required to have progressive
  // stages
  size_t xsize = 333;
  size_t ysize = 300;
  uint32_t num_channels = 4;
  std::vector<uint8_t> pixels =
      jxl::test::GetSomeTestImage(xsize, ysize, num_channels, 0);
  jxl::TestCodestreamParams params;
  params.cparams.SetLossless();
  params.cparams.speed_tier = jxl::SpeedTier::kThunder;
  params.cparams.responsive = 1;
  params.cparams.modular_group_size_shift = 1;
  params.preview_mode = jxl::kSmallPreview;
  std::vector<uint8_t> data =
      jxl::CreateTestJXLCodestream(jxl::Bytes(pixels.data(), pixels.size()),
                                   xsize, ysize, num_channels, params);
  JxlPixelFormat format = {num_channels, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};

  std::vector<uint8_t> pixels2;
  pixels2.resize(pixels.size());

  JxlDecoder* dec = JxlDecoderCreate(nullptr);

  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSubscribeEvents(
                dec, JXL_DEC_BASIC_INFO | JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE));

  // Ensure that the first part contains at least the full DC of the image,
  // otherwise flush does not work.
  size_t first_part = data.size() / 2;

  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, data.data(), first_part));

  EXPECT_EQ(JXL_DEC_BASIC_INFO, JxlDecoderProcessInput(dec));
  JxlBasicInfo info;
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBasicInfo(dec, &info));
  EXPECT_EQ(info.xsize, xsize);
  EXPECT_EQ(info.ysize, ysize);

  EXPECT_EQ(JXL_DEC_FRAME, JxlDecoderProcessInput(dec));

  // Output buffer not yet set
  EXPECT_EQ(JXL_DEC_ERROR, JxlDecoderFlushImage(dec));

  size_t buffer_size;
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderImageOutBufferSize(dec, &format, &buffer_size));
  EXPECT_EQ(pixels2.size(), buffer_size);
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetImageOutBuffer(
                                 dec, &format, pixels2.data(), pixels2.size()));

  // Must process input further until we get JXL_DEC_NEED_MORE_INPUT, even if
  // data was already input before, since the processing of the frame only
  // happens at the JxlDecoderProcessInput call after JXL_DEC_FRAME.
  EXPECT_EQ(JXL_DEC_NEED_MORE_INPUT, JxlDecoderProcessInput(dec));

  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderFlushImage(dec));

  EXPECT_LE(jxl::test::ComparePixels(pixels2.data(), pixels.data(), xsize,
                                     ysize, format, format, 2560.0),
            24000u);

  EXPECT_EQ(JXL_DEC_NEED_MORE_INPUT, JxlDecoderProcessInput(dec));

  size_t consumed = first_part - JxlDecoderReleaseInput(dec);

  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, data.data() + consumed,
                                                data.size() - consumed));

  EXPECT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec));
  EXPECT_LE(jxl::test::ComparePixels(pixels2.data(), pixels.data(), xsize,
                                     ysize, format, format),
            0u);

  JxlDecoderDestroy(dec);
}

class DecodeProgressiveTest : public ::testing::TestWithParam<int> {};
JXL_GTEST_INSTANTIATE_TEST_SUITE_P(DecodeProgressiveTestInstantiation,
                                   DecodeProgressiveTest,
                                   ::testing::Range(0, 8));
TEST_P(DecodeProgressiveTest, ProgressiveEventTest) {
  JxlMemoryManager* memory_manager = jxl::test::MemoryManager();
  const int params = GetParam();
  bool single_group = ((params & 1) != 0);
  bool lossless = (((params >> 1) & 1) != 0);
  uint32_t num_channels = 3 + ((params >> 2) & 1);
  bool has_alpha = ((num_channels & 1) == 0);
  std::set<JxlProgressiveDetail> progressive_details = {kDC, kLastPasses,
                                                        kPasses};
  for (auto prog_detail : progressive_details) {
    // Only few combinations are expected to support outputting
    // intermediate flushes for complete DC and complete passes.
    // The test can be updated if more cases are expected to support it.
    bool expect_flush = !has_alpha && !lossless;
    size_t xsize;
    size_t ysize;
    if (single_group) {
      // An image smaller than 256x256 ensures it contains only 1 group.
      xsize = 99;
      ysize = 100;
    } else {
      xsize = 277;
      ysize = 280;
    }
    std::vector<uint8_t> pixels =
        jxl::test::GetSomeTestImage(xsize, ysize, num_channels, 0);
    JxlPixelFormat format = {num_channels, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};
    jxl::ColorEncoding color_encoding = jxl::ColorEncoding::SRGB(false);
    auto io = jxl::make_unique<jxl::CodecInOut>(memory_manager);
    EXPECT_TRUE(jxl::ConvertFromExternal(
        jxl::Bytes(pixels.data(), pixels.size()), xsize, ysize, color_encoding,
        /*bits_per_sample=*/16, format,
        /*pool=*/nullptr, &io->Main()));
    jxl::TestCodestreamParams tc_params;
    if (lossless) {
      tc_params.cparams.SetLossless();
    } else {
      tc_params.cparams.butteraugli_distance = 0.5f;
    }
    const jxl::PassDefinition kPasses[] = {
        {2, 0, 4}, {4, 0, 4}, {8, 2, 2}, {8, 1, 2}, {8, 0, 1}};
    const int kNumPasses = 5;
    jxl::ProgressiveMode progressive_mode{kPasses};
    tc_params.cparams.custom_progressive_mode = &progressive_mode;
    std::vector<uint8_t> data =
        jxl::CreateTestJXLCodestream(jxl::Bytes(pixels.data(), pixels.size()),
                                     xsize, ysize, num_channels, tc_params);

    for (size_t increment : {static_cast<size_t>(1), data.size()}) {
      printf(
          "Testing with single_group=%s, lossless=%s, "
          "num_channels=%d, prog_detail=%d, increment=%d\n",
          BoolToCStr(single_group), BoolToCStr(lossless),
          static_cast<int>(num_channels), static_cast<int>(prog_detail),
          static_cast<int>(increment));
      std::vector<std::vector<uint8_t>> passes(kNumPasses + 1);
      for (int i = 0; i <= kNumPasses; ++i) {
        passes[i].resize(pixels.size());
      }

      JxlDecoder* dec = JxlDecoderCreate(nullptr);

      EXPECT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderSubscribeEvents(
                    dec, JXL_DEC_BASIC_INFO | JXL_DEC_FRAME |
                             JXL_DEC_FULL_IMAGE | JXL_DEC_FRAME_PROGRESSION));
      EXPECT_EQ(JXL_DEC_ERROR, JxlDecoderSetProgressiveDetail(dec, kFrames));
      EXPECT_EQ(JXL_DEC_ERROR,
                JxlDecoderSetProgressiveDetail(dec, kDCProgressive));
      EXPECT_EQ(JXL_DEC_ERROR, JxlDecoderSetProgressiveDetail(dec, kDCGroups));
      EXPECT_EQ(JXL_DEC_ERROR, JxlDecoderSetProgressiveDetail(dec, kGroups));
      EXPECT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderSetProgressiveDetail(dec, prog_detail));

      uint8_t* next_in = data.data();
      size_t avail_in = 0;
      size_t pos = 0;

      auto process_input = [&]() {
        for (;;) {
          EXPECT_EQ(JXL_DEC_SUCCESS,
                    JxlDecoderSetInput(dec, next_in, avail_in));
          JxlDecoderStatus status = JxlDecoderProcessInput(dec);
          size_t remaining = JxlDecoderReleaseInput(dec);
          EXPECT_LE(remaining, avail_in);
          next_in += avail_in - remaining;
          avail_in = remaining;
          if (status == JXL_DEC_NEED_MORE_INPUT && pos < data.size()) {
            size_t chunk = std::min<size_t>(increment, data.size() - pos);
            pos += chunk;
            avail_in += chunk;
            continue;
          }
          return status;
        }
      };

      EXPECT_EQ(JXL_DEC_BASIC_INFO, process_input());
      JxlBasicInfo info;
      EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBasicInfo(dec, &info));
      EXPECT_EQ(info.xsize, xsize);
      EXPECT_EQ(info.ysize, ysize);

      EXPECT_EQ(JXL_DEC_FRAME, process_input());

      size_t buffer_size;
      EXPECT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderImageOutBufferSize(dec, &format, &buffer_size));
      EXPECT_EQ(pixels.size(), buffer_size);
      EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetImageOutBuffer(
                                     dec, &format, passes[kNumPasses].data(),
                                     passes[kNumPasses].size()));

      auto next_pass = [&](int pass) {
        if (prog_detail <= kDC) return kNumPasses;
        if (prog_detail <= kLastPasses) {
          return std::min(pass + 2, kNumPasses);
        }
        return pass + 1;
      };

      if (expect_flush) {
        // Return a particular downsampling ratio only after the last
        // pass for that downsampling was processed.
        size_t expected_downsampling_ratios[] = {8u, 8u, 4u, 4u, 2u};
        for (int p = 0; p < kNumPasses; p = next_pass(p)) {
          EXPECT_EQ(JXL_DEC_FRAME_PROGRESSION, process_input());
          EXPECT_EQ(expected_downsampling_ratios[p],
                    JxlDecoderGetIntendedDownsamplingRatio(dec));
          EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderFlushImage(dec));
          passes[p] = passes[kNumPasses];
        }
      }

      EXPECT_EQ(JXL_DEC_FULL_IMAGE, process_input());
      EXPECT_EQ(JXL_DEC_SUCCESS, process_input());

      JxlDecoderDestroy(dec);

      if (!expect_flush) {
        continue;
      }
      jxl::ButteraugliParams butteraugli_params;
      std::vector<float> distances(kNumPasses + 1);
      for (int p = 0;; p = next_pass(p)) {
        auto io1 = jxl::make_unique<jxl::CodecInOut>(memory_manager);
        EXPECT_TRUE(jxl::ConvertFromExternal(
            jxl::Bytes(passes[p].data(), passes[p].size()), xsize, ysize,
            color_encoding,
            /*bits_per_sample=*/16, format,
            /*pool=*/nullptr, &io1->Main()));
        distances[p] =
            ButteraugliDistance(io->frames, io1->frames, butteraugli_params,
                                *JxlGetDefaultCms(), nullptr, nullptr);
        if (p == kNumPasses) break;
      }
      const float kMaxDistance[kNumPasses + 1] = {30.0f, 20.0f, 10.0f,
                                                  5.0f,  3.0f,  2.0f};
      EXPECT_LT(distances[kNumPasses], kMaxDistance[kNumPasses]);
      for (int p = 0; p < kNumPasses;) {
        int next_p = next_pass(p);
        EXPECT_LT(distances[p], kMaxDistance[p]);
        // Verify that the returned pass image is actually not the
        // same as the next pass image, by checking that it has a bit
        // worse butteraugli score.
        EXPECT_LT(distances[next_p] * 1.1f, distances[p]);
        p = next_p;
      }
    }
  }
}

void VerifyJPEGReconstruction(jxl::Span<const uint8_t> container,
                              jxl::Span<const uint8_t> jpeg_bytes) {
  JxlDecoderPtr dec = JxlDecoderMake(nullptr);
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSubscribeEvents(
                dec.get(), JXL_DEC_JPEG_RECONSTRUCTION | JXL_DEC_FULL_IMAGE));
  JxlDecoderSetInput(dec.get(), container.data(), container.size());
  EXPECT_EQ(JXL_DEC_JPEG_RECONSTRUCTION, JxlDecoderProcessInput(dec.get()));
  std::vector<uint8_t> reconstructed_buffer(128);
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSetJPEGBuffer(dec.get(), reconstructed_buffer.data(),
                                    reconstructed_buffer.size()));
  size_t used = 0u;
  JxlDecoderStatus process_result = JXL_DEC_JPEG_NEED_MORE_OUTPUT;
  while (process_result == JXL_DEC_JPEG_NEED_MORE_OUTPUT) {
    used = reconstructed_buffer.size() - JxlDecoderReleaseJPEGBuffer(dec.get());
    reconstructed_buffer.resize(reconstructed_buffer.size() * 2);
    EXPECT_EQ(
        JXL_DEC_SUCCESS,
        JxlDecoderSetJPEGBuffer(dec.get(), reconstructed_buffer.data() + used,
                                reconstructed_buffer.size() - used));
    process_result = JxlDecoderProcessInput(dec.get());
  }
  ASSERT_EQ(JXL_DEC_FULL_IMAGE, process_result);
  used = reconstructed_buffer.size() - JxlDecoderReleaseJPEGBuffer(dec.get());
  ASSERT_EQ(used, jpeg_bytes.size());
  EXPECT_EQ(0, memcmp(reconstructed_buffer.data(), jpeg_bytes.data(), used));
}

JXL_TRANSCODE_JPEG_TEST(DecodeTest, JPEGReconstructTestCodestream) {
  TEST_LIBJPEG_SUPPORT();
  size_t xsize = 123;
  size_t ysize = 77;
  size_t channels = 3;
  std::vector<uint8_t> pixels =
      jxl::test::GetSomeTestImage(xsize, ysize, channels, /*seed=*/0);
  std::vector<uint8_t> jpeg_codestream;
  jxl::TestCodestreamParams params;
  params.cparams.color_transform = jxl::ColorTransform::kNone;
  params.box_format = kCSBF_Single;
  params.jpeg_codestream = &jpeg_codestream;
  params.preview_mode = jxl::kSmallPreview;
  std::vector<uint8_t> compressed = jxl::CreateTestJXLCodestream(
      jxl::Bytes(pixels.data(), pixels.size()), xsize, ysize, channels, params);
  VerifyJPEGReconstruction(jxl::Bytes(compressed), jxl::Bytes(jpeg_codestream));
}

JXL_TRANSCODE_JPEG_TEST(DecodeTest, JPEGReconstructionTest) {
  JxlMemoryManager* memory_manager = jxl::test::MemoryManager();
  const std::string jpeg_path = "jxl/flower/flower.png.im_q85_420.jpg";
  const std::vector<uint8_t> orig = jxl::test::ReadTestData(jpeg_path);
  auto orig_io = jxl::make_unique<jxl::CodecInOut>(memory_manager);
  JXL_TEST_ASSIGN_OR_DIE(std::unique_ptr<jxl::jpeg::JPEGData> jpeg_data,
                         jxl::jpeg::ParseJPG(memory_manager, jxl::Bytes(orig)));
  ASSERT_TRUE(
      jxl::test::JpegDataToCodecInOut(std::move(jpeg_data), orig_io.get()));
  jxl::jpeg::JPEGData jpeg_data_copy = *orig_io->Main().jpeg_data;
  orig_io->metadata.m.xyb_encoded = false;
  jxl::BitWriter writer{memory_manager};
  ASSERT_TRUE(WriteCodestreamHeaders(&orig_io->metadata, &writer, nullptr));
  writer.ZeroPadToByte();
  jxl::CompressParams cparams;
  cparams.color_transform = jxl::ColorTransform::kNone;
  ASSERT_TRUE(jxl::EncodeFrame(memory_manager, cparams, jxl::FrameInfo{},
                               &orig_io->metadata, orig_io->Main(),
                               *JxlGetDefaultCms(),
                               /*pool=*/nullptr, &writer,
                               /*aux_out=*/nullptr));

  std::vector<uint8_t> encoded_jpeg_data;
  ASSERT_TRUE(EncodeJPEGData(memory_manager, jpeg_data_copy, &encoded_jpeg_data,
                             cparams));
  std::vector<uint8_t> container = jxl::MakeContainerHeader(0);
  jxl::AppendBoxHeader(jxl::MakeBoxType("jbrd"), encoded_jpeg_data.size(),
                       false, &container);
  jxl::Bytes(encoded_jpeg_data).AppendTo(container);
  jxl::AppendBoxHeader(jxl::MakeBoxType("jxlc"), 0, true, &container);
  jxl::PaddedBytes codestream = std::move(writer).TakeBytes();
  jxl::Bytes(codestream).AppendTo(container);
  VerifyJPEGReconstruction(jxl::Bytes(container), jxl::Bytes(orig));
}

JXL_TRANSCODE_JPEG_TEST(DecodeTest, JPEGReconstructionMetadataTest) {
  const std::string jpeg_path = "jxl/jpeg_reconstruction/1x1_exif_xmp.jpg";
  const std::string jxl_path = "jxl/jpeg_reconstruction/1x1_exif_xmp.jxl";
  const std::vector<uint8_t> jpeg = jxl::test::ReadTestData(jpeg_path);
  const std::vector<uint8_t> jxl = jxl::test::ReadTestData(jxl_path);
  VerifyJPEGReconstruction(jxl::Bytes(jxl), jxl::Bytes(jpeg));
}

TEST(DecodeTest, ContinueFinalNonEssentialBoxTest) {
  size_t xsize = 80;
  size_t ysize = 90;
  std::vector<uint8_t> pixels = jxl::test::GetSomeTestImage(xsize, ysize, 4, 0);
  jxl::TestCodestreamParams params;
  params.box_format = kCSBF_Multi_Other_Terminated;
  params.add_icc_profile = true;
  std::vector<uint8_t> compressed = jxl::CreateTestJXLCodestream(
      jxl::Bytes(pixels.data(), pixels.size()), xsize, ysize, 4, params);
  StreamPositions streampos;
  AnalyzeCodestream(compressed, &streampos);

  // The non-essential final box size including 8-byte header
  size_t final_box_size = unk3_box_size + 8;
  size_t last_box_begin = compressed.size() - final_box_size;
  // Verify that the test is indeed setup correctly to be at the beginning of
  // the 'unkn' box header.
  ASSERT_EQ(compressed[last_box_begin + 3], final_box_size);
  ASSERT_EQ(compressed[last_box_begin + 4], 'u');
  ASSERT_EQ(compressed[last_box_begin + 5], 'n');
  ASSERT_EQ(compressed[last_box_begin + 6], 'k');
  ASSERT_EQ(compressed[last_box_begin + 7], '3');

  JxlDecoder* dec = JxlDecoderCreate(nullptr);

  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_FRAME));

  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSetInput(dec, compressed.data(), last_box_begin));

  EXPECT_EQ(JXL_DEC_BASIC_INFO, JxlDecoderProcessInput(dec));
  EXPECT_EQ(JXL_DEC_FRAME, JxlDecoderProcessInput(dec));
  // The decoder returns success despite not having seen the final unknown box
  // yet. This is because calling JxlDecoderCloseInput is not mandatory for
  // backwards compatibility, so it doesn't know more bytes follow, the current
  // bytes ended at a perfectly valid place.
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderProcessInput(dec));

  size_t remaining = JxlDecoderReleaseInput(dec);
  // Since the test was set up to end exactly at the boundary of the final
  // codestream box, and the decoder returned success, all bytes are expected to
  // be consumed until the end of the  frame header.
  EXPECT_EQ(remaining, last_box_begin - streampos.frames[0].toc_end);

  // Now set the remaining non-codestream box as input.
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSetInput(dec, compressed.data() + last_box_begin,
                               compressed.size() - last_box_begin));
  // Even though JxlDecoderProcessInput already returned JXL_DEC_SUCCESS before,
  // when calling it again now after setting more input, success is expected, no
  // event occurs but the box has been successfully skipped.
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderProcessInput(dec));

  JxlDecoderDestroy(dec);
}

namespace {
bool BoxTypeEquals(const std::string& type_string, const JxlBoxType type) {
  return type_string.size() == 4 && type_string[0] == type[0] &&
         type_string[1] == type[1] && type_string[2] == type[2] &&
         type_string[3] == type[3];
}
}  // namespace

TEST(DecodeTest, ExtendedBoxSizeTest) {
  const std::string jxl_path = "jxl/boxes/square-extended-size-container.jxl";
  const std::vector<uint8_t> orig = jxl::test::ReadTestData(jxl_path);
  JxlDecoder* dec = JxlDecoderCreate(nullptr);

  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSubscribeEvents(dec, JXL_DEC_BOX));

  JxlBoxType type;
  uint64_t box_size;
  uint64_t contents_size;
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, orig.data(), orig.size()));
  EXPECT_EQ(JXL_DEC_BOX, JxlDecoderProcessInput(dec));
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBoxType(dec, type, JXL_FALSE));
  EXPECT_TRUE(BoxTypeEquals("JXL ", type));
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBoxSizeRaw(dec, &box_size));
  EXPECT_EQ(12u, box_size);
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBoxSizeContents(dec, &contents_size));
  EXPECT_EQ(contents_size + 8, box_size);
  EXPECT_EQ(JXL_DEC_BOX, JxlDecoderProcessInput(dec));
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBoxType(dec, type, JXL_FALSE));
  EXPECT_TRUE(BoxTypeEquals("ftyp", type));
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBoxSizeRaw(dec, &box_size));
  EXPECT_EQ(20u, box_size);
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBoxSizeContents(dec, &contents_size));
  EXPECT_EQ(contents_size + 8u, box_size);
  EXPECT_EQ(JXL_DEC_BOX, JxlDecoderProcessInput(dec));
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBoxType(dec, type, JXL_FALSE));
  EXPECT_TRUE(BoxTypeEquals("jxlc", type));
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBoxSizeRaw(dec, &box_size));
  EXPECT_EQ(72u, box_size);
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBoxSizeContents(dec, &contents_size));
  // This is an extended box, hence the difference between `box_size` and
  // `contents_size` is 16.
  EXPECT_EQ(contents_size + 8u + 8u, box_size);

  JxlDecoderDestroy(dec);
}

JXL_BOXES_TEST(DecodeTest, BoxTest) {
  size_t xsize = 1;
  size_t ysize = 1;
  std::vector<uint8_t> pixels = jxl::test::GetSomeTestImage(xsize, ysize, 4, 0);
  jxl::TestCodestreamParams params;
  params.box_format = kCSBF_Multi_Other_Terminated;
  params.add_icc_profile = true;
  std::vector<uint8_t> compressed = jxl::CreateTestJXLCodestream(
      jxl::Bytes(pixels.data(), pixels.size()), xsize, ysize, 4, params);

  JxlDecoder* dec = JxlDecoderCreate(nullptr);

  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSubscribeEvents(dec, JXL_DEC_BOX));

  std::vector<std::string> expected_box_types = {
      "JXL ", "ftyp", "jxlp", "unk1", "unk2", "jxlp", "jxlp", "jxlp", "unk3"};

  // Value 0 means to not test the size: codestream is not required to be a
  // particular exact size.
  std::vector<size_t> expected_box_sizes = {12, 20, 0, 34, 18, 0, 0, 0, 20};

  JxlBoxType type;
  uint64_t box_size;
  uint64_t contents_size;
  std::vector<uint8_t> contents(50);
  size_t expected_release_size = 0;

  // Cannot get these when decoding didn't start yet
  EXPECT_EQ(JXL_DEC_ERROR, JxlDecoderGetBoxType(dec, type, JXL_FALSE));
  EXPECT_EQ(JXL_DEC_ERROR, JxlDecoderGetBoxSizeRaw(dec, &box_size));

  uint8_t* next_in = compressed.data();
  size_t avail_in = compressed.size();
  for (size_t i = 0; i < expected_box_types.size(); i++) {
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, next_in, avail_in));
    EXPECT_EQ(JXL_DEC_BOX, JxlDecoderProcessInput(dec));
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBoxType(dec, type, JXL_FALSE));
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBoxSizeRaw(dec, &box_size));
    EXPECT_TRUE(BoxTypeEquals(expected_box_types[i], type));
    if (expected_box_sizes[i]) {
      EXPECT_EQ(expected_box_sizes[i], box_size);
      EXPECT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderGetBoxSizeContents(dec, &contents_size));
      EXPECT_EQ(contents_size + 8u, box_size);
    }

    if (expected_release_size > 0) {
      EXPECT_EQ(expected_release_size, JxlDecoderReleaseBoxBuffer(dec));
      expected_release_size = 0u;
    }

    if (type[0] == 'u' && type[1] == 'n' && type[2] == 'k') {
      JxlDecoderSetBoxBuffer(dec, contents.data(), contents.size());
      size_t expected_box_contents_size =
          type[3] == '1' ? unk1_box_size
                         : (type[3] == '2' ? unk2_box_size : unk3_box_size);
      expected_release_size = contents.size() - expected_box_contents_size;
    }
    size_t consumed = avail_in - JxlDecoderReleaseInput(dec);
    next_in += consumed;
    avail_in -= consumed;
  }

  // After the last DEC_BOX event, check that the input position is exactly at
  // the stat of the box header.
  EXPECT_EQ(avail_in, expected_box_sizes.back());

  // Even though all input is given, the decoder cannot assume there aren't
  // more boxes if the input was not closed.
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, next_in, avail_in));
  EXPECT_EQ(JXL_DEC_NEED_MORE_INPUT, JxlDecoderProcessInput(dec));
  JxlDecoderCloseInput(dec);
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderProcessInput(dec));

  JxlDecoderDestroy(dec);
}

JXL_BOXES_TEST(DecodeTest, ExifBrobBoxTest) {
  size_t xsize = 1;
  size_t ysize = 1;
  std::vector<uint8_t> pixels = jxl::test::GetSomeTestImage(xsize, ysize, 4, 0);
  jxl::TestCodestreamParams params;
  // Lossless to verify pixels exactly after roundtrip.
  params.cparams.SetLossless();
  params.box_format = kCSBF_Brob_Exif;
  params.add_icc_profile = true;
  std::vector<uint8_t> compressed = jxl::CreateTestJXLCodestream(
      jxl::Bytes(pixels.data(), pixels.size()), xsize, ysize, 4, params);

  // Test raw brob box, not brotli-decompressing
  for (int streaming = 0; streaming < 2; ++streaming) {
    JxlDecoder* dec = JxlDecoderCreate(nullptr);

    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSubscribeEvents(dec, JXL_DEC_BOX));
    if (!streaming) {
      EXPECT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderSetInput(dec, compressed.data(), compressed.size()));
      JxlDecoderCloseInput(dec);
    }
    // for streaming input case
    const uint8_t* next_in = compressed.data();
    size_t avail_in = 0;
    size_t total_in = 0;
    size_t step_size = 64;

    std::vector<uint8_t> box_buffer;
    size_t box_num_output;
    bool seen_brob_begin = false;
    bool seen_brob_end = false;

    for (;;) {
      JxlDecoderStatus status = JxlDecoderProcessInput(dec);
      if (status == JXL_DEC_NEED_MORE_INPUT) {
        if (streaming) {
          size_t remaining = JxlDecoderReleaseInput(dec);
          EXPECT_LE(remaining, avail_in);
          next_in += avail_in - remaining;
          avail_in = remaining;
          size_t amount = step_size;
          if (total_in + amount > compressed.size()) {
            amount = compressed.size() - total_in;
          }
          avail_in += amount;
          total_in += amount;
          EXPECT_EQ(JXL_DEC_SUCCESS,
                    JxlDecoderSetInput(dec, next_in, avail_in));
          if (total_in == compressed.size()) JxlDecoderCloseInput(dec);
        } else {
          FAIL();
          break;
        }
      } else if (status == JXL_DEC_BOX || status == JXL_DEC_SUCCESS) {
        if (!box_buffer.empty()) {
          EXPECT_EQ(false, seen_brob_end);
          seen_brob_end = true;
          size_t remaining = JxlDecoderReleaseBoxBuffer(dec);
          box_num_output = box_buffer.size() - remaining;
          EXPECT_EQ(box_num_output, box_brob_exif_size - 8u);
          EXPECT_EQ(
              0, memcmp(box_buffer.data(), box_brob_exif + 8, box_num_output));
          box_buffer.clear();
        }
        if (status == JXL_DEC_SUCCESS) break;
        JxlBoxType type;
        EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBoxType(dec, type, JXL_FALSE));
        if (BoxTypeEquals("brob", type)) {
          EXPECT_EQ(false, seen_brob_begin);
          seen_brob_begin = true;
          box_buffer.resize(8);
          JxlDecoderSetBoxBuffer(dec, box_buffer.data(), box_buffer.size());
        }
      } else if (status == JXL_DEC_BOX_NEED_MORE_OUTPUT) {
        size_t remaining = JxlDecoderReleaseBoxBuffer(dec);
        box_num_output = box_buffer.size() - remaining;
        box_buffer.resize(box_buffer.size() * 2);
        JxlDecoderSetBoxBuffer(dec, box_buffer.data() + box_num_output,
                               box_buffer.size() - box_num_output);
      } else {
        // We do not expect any other events or errors
        FAIL();
        break;
      }
    }

    EXPECT_EQ(true, seen_brob_begin);
    EXPECT_EQ(true, seen_brob_end);

    JxlDecoderDestroy(dec);
  }

  // Test decompressed brob box
  for (int streaming = 0; streaming < 2; ++streaming) {
    JxlDecoder* dec = JxlDecoderCreate(nullptr);

    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSubscribeEvents(dec, JXL_DEC_BOX));
    if (!streaming) {
      EXPECT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderSetInput(dec, compressed.data(), compressed.size()));
      JxlDecoderCloseInput(dec);
    }
    // for streaming input case
    const uint8_t* next_in = compressed.data();
    size_t avail_in = 0;
    size_t total_in = 0;
    size_t step_size = 64;

    std::vector<uint8_t> box_buffer;
    size_t box_num_output;
    bool seen_exif_begin = false;
    bool seen_exif_end = false;

    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetDecompressBoxes(dec, JXL_TRUE));

    for (;;) {
      JxlDecoderStatus status = JxlDecoderProcessInput(dec);
      if (status == JXL_DEC_NEED_MORE_INPUT) {
        if (streaming) {
          size_t remaining = JxlDecoderReleaseInput(dec);
          EXPECT_LE(remaining, avail_in);
          next_in += avail_in - remaining;
          avail_in = remaining;
          size_t amount = step_size;
          if (total_in + amount > compressed.size()) {
            amount = compressed.size() - total_in;
          }
          avail_in += amount;
          total_in += amount;
          EXPECT_EQ(JXL_DEC_SUCCESS,
                    JxlDecoderSetInput(dec, next_in, avail_in));
          if (total_in == compressed.size()) JxlDecoderCloseInput(dec);
        } else {
          FAIL();
          break;
        }
      } else if (status == JXL_DEC_BOX || status == JXL_DEC_SUCCESS) {
        if (!box_buffer.empty()) {
          EXPECT_EQ(false, seen_exif_end);
          seen_exif_end = true;
          size_t remaining = JxlDecoderReleaseBoxBuffer(dec);
          box_num_output = box_buffer.size() - remaining;
          // Expect that the output has the same size and contents as the
          // uncompressed exif data. Only check contents if the sizes match to
          // avoid comparing uninitialized memory in the test.
          EXPECT_EQ(box_num_output, exif_uncompressed_size);
          if (box_num_output == exif_uncompressed_size) {
            EXPECT_EQ(0, memcmp(box_buffer.data(), exif_uncompressed,
                                exif_uncompressed_size));
          }
          box_buffer.clear();
        }
        if (status == JXL_DEC_SUCCESS) break;
        JxlBoxType type;
        EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBoxType(dec, type, JXL_TRUE));
        if (BoxTypeEquals("Exif", type)) {
          EXPECT_EQ(false, seen_exif_begin);
          seen_exif_begin = true;
          box_buffer.resize(8);
          JxlDecoderSetBoxBuffer(dec, box_buffer.data(), box_buffer.size());
        }
      } else if (status == JXL_DEC_BOX_NEED_MORE_OUTPUT) {
        size_t remaining = JxlDecoderReleaseBoxBuffer(dec);
        box_num_output = box_buffer.size() - remaining;
        box_buffer.resize(box_buffer.size() * 2);
        JxlDecoderSetBoxBuffer(dec, box_buffer.data() + box_num_output,
                               box_buffer.size() - box_num_output);
      } else {
        // We do not expect any other events or errors
        FAIL();
        break;
      }
    }

    EXPECT_EQ(true, seen_exif_begin);
    EXPECT_EQ(true, seen_exif_end);

    JxlDecoderDestroy(dec);
  }
}

JXL_BOXES_TEST(DecodeTest, PartialCodestreamBoxTest) {
  size_t xsize = 23;
  size_t ysize = 81;
  std::vector<uint8_t> pixels = jxl::test::GetSomeTestImage(xsize, ysize, 4, 0);
  JxlPixelFormat format_orig = {4, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};
  // Lossless to verify pixels exactly after roundtrip.
  jxl::TestCodestreamParams params;
  params.cparams.SetLossless();
  params.cparams.speed_tier = jxl::SpeedTier::kThunder;
  params.box_format = kCSBF_Multi;
  params.add_icc_profile = true;
  std::vector<uint8_t> compressed = jxl::CreateTestJXLCodestream(
      jxl::Bytes(pixels.data(), pixels.size()), xsize, ysize, 4, params);

  std::vector<uint8_t> extracted_codestream;

  {
    JxlDecoder* dec = JxlDecoderCreate(nullptr);

    EXPECT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderSubscribeEvents(
                  dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE | JXL_DEC_BOX));
    EXPECT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderSetInput(dec, compressed.data(), compressed.size()));
    JxlDecoderCloseInput(dec);

    size_t num_jxlp = 0;

    std::vector<uint8_t> pixels2;
    pixels2.resize(pixels.size());

    std::vector<uint8_t> box_buffer;
    size_t box_num_output;

    for (;;) {
      JxlDecoderStatus status = JxlDecoderProcessInput(dec);
      if (status == JXL_DEC_NEED_MORE_INPUT) {
        FAIL();
        break;
      } else if (status == JXL_DEC_BASIC_INFO) {
        JxlBasicInfo info;
        EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBasicInfo(dec, &info));
        EXPECT_EQ(info.xsize, xsize);
        EXPECT_EQ(info.ysize, ysize);
      } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
        EXPECT_EQ(JXL_DEC_SUCCESS,
                  JxlDecoderSetImageOutBuffer(dec, &format_orig, pixels2.data(),
                                              pixels2.size()));
      } else if (status == JXL_DEC_FULL_IMAGE) {
        continue;
      } else if (status == JXL_DEC_BOX || status == JXL_DEC_SUCCESS) {
        if (!box_buffer.empty()) {
          size_t remaining = JxlDecoderReleaseBoxBuffer(dec);
          box_num_output = box_buffer.size() - remaining;
          EXPECT_GE(box_num_output, 4u);
          // Do not insert the first 4 bytes, which are not part of the
          // codestream, but the partial codestream box index
          extracted_codestream.insert(extracted_codestream.end(),
                                      box_buffer.begin() + 4,
                                      box_buffer.begin() + box_num_output);
          box_buffer.clear();
        }
        if (status == JXL_DEC_SUCCESS) break;
        JxlBoxType type;
        EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBoxType(dec, type, JXL_FALSE));
        if (BoxTypeEquals("jxlp", type)) {
          num_jxlp++;
          box_buffer.resize(8);
          JxlDecoderSetBoxBuffer(dec, box_buffer.data(), box_buffer.size());
        }
      } else if (status == JXL_DEC_BOX_NEED_MORE_OUTPUT) {
        size_t remaining = JxlDecoderReleaseBoxBuffer(dec);
        box_num_output = box_buffer.size() - remaining;
        box_buffer.resize(box_buffer.size() * 2);
        JxlDecoderSetBoxBuffer(dec, box_buffer.data() + box_num_output,
                               box_buffer.size() - box_num_output);
      } else {
        // We do not expect any other events or errors
        FAIL();
        break;
      }
    }

    // The test file created with kCSBF_Multi is expected to have 4 jxlp boxes.
    EXPECT_EQ(4u, num_jxlp);

    EXPECT_EQ(0u, jxl::test::ComparePixels(pixels.data(), pixels2.data(), xsize,
                                           ysize, format_orig, format_orig));

    JxlDecoderDestroy(dec);
  }

  // Now test whether the codestream extracted from the jxlp boxes can itself
  // also be decoded and gives the same pixels
  {
    JxlDecoder* dec = JxlDecoderCreate(nullptr);

    EXPECT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderSubscribeEvents(
                  dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE | JXL_DEC_BOX));
    EXPECT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderSetInput(dec, extracted_codestream.data(),
                                 extracted_codestream.size()));
    JxlDecoderCloseInput(dec);

    size_t num_boxes = 0;

    std::vector<uint8_t> pixels2;
    pixels2.resize(pixels.size());

    std::vector<uint8_t> box_buffer;
    size_t box_num_output;

    for (;;) {
      JxlDecoderStatus status = JxlDecoderProcessInput(dec);
      if (status == JXL_DEC_NEED_MORE_INPUT) {
        FAIL();
        break;
      } else if (status == JXL_DEC_BASIC_INFO) {
        JxlBasicInfo info;
        EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBasicInfo(dec, &info));
        EXPECT_EQ(info.xsize, xsize);
        EXPECT_EQ(info.ysize, ysize);
      } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
        EXPECT_EQ(JXL_DEC_SUCCESS,
                  JxlDecoderSetImageOutBuffer(dec, &format_orig, pixels2.data(),
                                              pixels2.size()));
      } else if (status == JXL_DEC_FULL_IMAGE) {
        continue;
      } else if (status == JXL_DEC_BOX) {
        num_boxes++;
      } else if (status == JXL_DEC_BOX_NEED_MORE_OUTPUT) {
        size_t remaining = JxlDecoderReleaseBoxBuffer(dec);
        box_num_output = box_buffer.size() - remaining;
        box_buffer.resize(box_buffer.size() * 2);
        JxlDecoderSetBoxBuffer(dec, box_buffer.data() + box_num_output,
                               box_buffer.size() - box_num_output);
      } else if (status == JXL_DEC_SUCCESS) {
        break;
      } else {
        // We do not expect any other events or errors
        FAIL();
        break;
      }
    }

    EXPECT_EQ(0u, num_boxes);  // The data does not use the container format.
    EXPECT_EQ(0u, jxl::test::ComparePixels(pixels.data(), pixels2.data(), xsize,
                                           ysize, format_orig, format_orig));

    JxlDecoderDestroy(dec);
  }
}

// Regression test for out-of-order jxlp box handling (PR #4741). A jxlp box
// whose index duplicates an already-buffered out-of-order index must be
// rejected. Before the fix, the second box's payload was silently concatenated
// onto the first buffered one (and its is_last flag dropped) instead of being
// flagged as an error, so the decoder kept asking for more input. We therefore
// keep the input open: without the fix this returns JXL_DEC_NEED_MORE_INPUT,
// with the fix it returns JXL_DEC_ERROR at the duplicate box.
JXL_BOXES_TEST(DecodeTest, OutOfOrderJxlpDuplicateIndexTest) {
  auto append_tag = [](const char* tag, std::vector<uint8_t>* out) {
    out->insert(out->end(), tag, tag + 4);
  };
  std::vector<uint8_t> c;

  // JXL signature box (12 bytes).
  AppendU32BE(12, &c);
  append_tag("JXL ", &c);
  c.insert(c.end(), {0x0D, 0x0A, 0x87, 0x0A});

  // ftyp box declaring file format version 1, which enables out-of-order jxlp.
  AppendU32BE(20, &c);
  append_tag("ftyp", &c);
  append_tag("jxl ", &c);  // major brand
  AppendU32BE(1, &c);      // minor version = 1
  append_tag("jxl ", &c);  // compatible brand

  // Two jxlp boxes that both carry out-of-order index 1 (the expected first
  // index is 0). The first is buffered; the second is a duplicate.
  for (int i = 0; i < 2; ++i) {
    AppendU32BE(16, &c);  // box size: 8-byte header + 8-byte contents
    append_tag("jxlp", &c);
    AppendU32BE(1, &c);  // jxlp index 1, high bit unset (not the last box)
    c.insert(c.end(), {0xAA, 0xBB, 0xCC, 0xDD});  // payload
  }

  JxlDecoder* dec = JxlDecoderCreate(nullptr);
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO));
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec, c.data(), c.size()));
  // Intentionally do not close the input: this distinguishes the rejection
  // (JXL_DEC_ERROR) from the pre-fix "buffer and wait" (JXL_DEC_NEED_MORE_INPUT)
  // behavior.
  EXPECT_EQ(JXL_DEC_ERROR, JxlDecoderProcessInput(dec));
  JxlDecoderDestroy(dec);
}

TEST(DecodeTest, SpotColorTest) {
  JxlMemoryManager* memory_manager = jxl::test::MemoryManager();
  auto io = jxl::make_unique<jxl::CodecInOut>(memory_manager);
  size_t xsize = 55;
  size_t ysize = 257;
  io->metadata.m.color_encoding = jxl::ColorEncoding::LinearSRGB();
  JXL_TEST_ASSIGN_OR_DIE(Image3F main,
                         Image3F::Create(memory_manager, xsize, ysize));
  JXL_TEST_ASSIGN_OR_DIE(ImageF spot,
                         ImageF::Create(memory_manager, xsize, ysize));
  jxl::ZeroFillImage(&main);
  jxl::ZeroFillImage(&spot);

  for (size_t y = 0; y < ysize; y++) {
    float* JXL_RESTRICT rowm = main.PlaneRow(1, y);
    float* JXL_RESTRICT rows = spot.Row(y);
    for (size_t x = 0; x < xsize; x++) {
      rowm[x] = (x + y) * (1.f / 255.f);
      rows[x] = ((x ^ y) & 255) * (1.f / 255.f);
    }
  }
  ASSERT_TRUE(
      io->SetFromImage(std::move(main), jxl::ColorEncoding::LinearSRGB()));
  jxl::ExtraChannelInfo info;
  info.bit_depth.bits_per_sample = 8;
  info.dim_shift = 0;
  info.type = jxl::ExtraChannel::kSpotColor;
  info.spot_color[0] = 0.5f;
  info.spot_color[1] = 0.2f;
  info.spot_color[2] = 1.f;
  info.spot_color[3] = 0.5f;

  io->metadata.m.extra_channel_info.push_back(info);
  std::vector<ImageF> ec;
  ec.push_back(std::move(spot));
  ASSERT_TRUE(io->frames[0].SetExtraChannels(std::move(ec)));

  jxl::CompressParams cparams;
  cparams.speed_tier = jxl::SpeedTier::kLightning;
  cparams.modular_mode = true;
  cparams.color_transform = jxl::ColorTransform::kNone;
  cparams.butteraugli_distance = 0.f;

  std::vector<uint8_t> compressed;
  EXPECT_TRUE(jxl::test::EncodeFile(cparams, io.get(), &compressed));

  for (size_t render_spot = 0; render_spot < 2; render_spot++) {
    JxlPixelFormat format = {3, JXL_TYPE_UINT8, JXL_LITTLE_ENDIAN, 0};

    JxlDecoder* dec = JxlDecoderCreate(nullptr);

    EXPECT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderSubscribeEvents(
                  dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE));
    if (!render_spot) {
      EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetRenderSpotcolors(dec, JXL_FALSE));
    }

    EXPECT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderSetInput(dec, compressed.data(), compressed.size()));
    EXPECT_EQ(JXL_DEC_BASIC_INFO, JxlDecoderProcessInput(dec));
    JxlBasicInfo binfo;
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBasicInfo(dec, &binfo));
    EXPECT_EQ(1u, binfo.num_extra_channels);
    EXPECT_EQ(xsize, binfo.xsize);
    EXPECT_EQ(ysize, binfo.ysize);

    JxlExtraChannelInfo extra_info;
    EXPECT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderGetExtraChannelInfo(dec, 0, &extra_info));
    EXPECT_EQ(static_cast<unsigned int>(jxl::ExtraChannel::kSpotColor),
              extra_info.type);

    EXPECT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER, JxlDecoderProcessInput(dec));
    size_t buffer_size;
    EXPECT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderImageOutBufferSize(dec, &format, &buffer_size));
    size_t extra_size;
    EXPECT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderExtraChannelBufferSize(dec, &format, &extra_size, 0));

    std::vector<uint8_t> image(buffer_size);
    std::vector<uint8_t> extra(extra_size);
    size_t bytes_per_pixel = format.num_channels *
                             jxl::test::GetDataBits(format.data_type) /
                             jxl::kBitsPerByte;
    size_t stride = bytes_per_pixel * binfo.xsize;

    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetImageOutBuffer(
                                   dec, &format, image.data(), image.size()));
    EXPECT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderSetExtraChannelBuffer(dec, &format, extra.data(),
                                              extra.size(), 0));

    EXPECT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec));

    // After the full image was output, JxlDecoderProcessInput should return
    // success to indicate all is done.
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderProcessInput(dec));
    JxlDecoderDestroy(dec);

    for (size_t y = 0; y < ysize; y++) {
      uint8_t* JXL_RESTRICT rowm = image.data() + stride * y;
      uint8_t* JXL_RESTRICT rows = extra.data() + xsize * y;
      for (size_t x = 0; x < xsize; x++) {
        if (!render_spot) {
          // if spot color isn't rendered, main image should be as we made it
          // (red and blue are all zeroes)

          EXPECT_EQ(rowm[x * 3 + 0], 0u);
          EXPECT_EQ(rowm[x * 3 + 1], (x + y > 255u ? 255u : x + y));
          EXPECT_EQ(rowm[x * 3 + 2], 0u);
        }
        if (render_spot) {
          // if spot color is rendered, expect red and blue to look like the
          // spot color channel
          EXPECT_LT(abs(rowm[x * 3 + 0] - (rows[x] * 0.25f)), 1.0f);
          EXPECT_LT(abs(rowm[x * 3 + 2] - (rows[x] * 0.5f)), 1.0f);
        }
        EXPECT_EQ(rows[x], ((x ^ y) & 255u));
      }
    }
  }
}

TEST(DecodeTest, CloseInput) {
  std::vector<uint8_t> partial_file = {0xff};

  JxlDecoderPtr dec = JxlDecoderMake(nullptr);
  EXPECT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSubscribeEvents(dec.get(),
                                      JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE));
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec.get(), partial_file.data(),
                                                partial_file.size()));
  EXPECT_EQ(JXL_DEC_NEED_MORE_INPUT, JxlDecoderProcessInput(dec.get()));
  EXPECT_EQ(JXL_DEC_NEED_MORE_INPUT, JxlDecoderProcessInput(dec.get()));
  JxlDecoderCloseInput(dec.get());
  EXPECT_EQ(JXL_DEC_ERROR, JxlDecoderProcessInput(dec.get()));
}

std::vector<uint8_t> CreateDCOnlyTestCodestream(
    size_t xsize, size_t ysize, uint32_t num_channels,
    const jxl::TestCodestreamParams& params) {
  std::vector<uint8_t> pixels =
      jxl::test::GetSomeTestImage(xsize, ysize, num_channels, 0);
  return jxl::CreateTestJXLCodestream(jxl::Bytes(pixels.data(), pixels.size()),
                                      xsize, ysize, num_channels, params);
}

float BoxAveragePixel(const jxl::extras::PackedImage& image, size_t factor,
                      size_t out_x, size_t out_y, size_t channel) {
  const size_t x0 = out_x * factor;
  const size_t y0 = out_y * factor;
  const size_t x1 = std::min(x0 + factor, image.xsize);
  const size_t y1 = std::min(y0 + factor, image.ysize);
  const size_t count = (x1 - x0) * (y1 - y0);
  float sum = 0.0f;
  for (size_t y = y0; y < y1; ++y) {
    for (size_t x = x0; x < x1; ++x) {
      sum += image.GetPixelValue(y, x, channel);
    }
  }
  return sum / count;
}

// Decodes `compressed` fully and as a preview at `factor`, and checks the
// preview against box averages of the full decode. Non-fallback previews are
// checked by mean absolute error against `max_mae`, about twice the measured
// error, and must match their own boxes better than their neighbors'.
void VerifyPreviewDownsamplingOfCodestream(
    const std::vector<uint8_t>& compressed, size_t xsize, size_t ysize,
    size_t factor, jxl::extras::JXLPreviewBackend expected_backend,
    double max_mae) {
  jxl::extras::JXLDecompressParams full_params;
  jxl::test::DefaultAcceptedFormats(full_params);
  jxl::extras::PackedPixelFile full;
  ASSERT_TRUE(jxl::extras::DecodeImageJXL(compressed.data(), compressed.size(),
                                          full_params,
                                          /*decoded_bytes=*/nullptr, &full));

  jxl::extras::JXLDecompressParams preview_params;
  jxl::test::DefaultAcceptedFormats(preview_params);
  preview_params.preview_downsampling = factor;
  preview_params.preview_hooks = jxl::GetDecoderPreviewHooks();
  jxl::extras::JXLPreviewBackend preview_backend =
      jxl::extras::JXLPreviewBackend::kNone;
  preview_params.preview_backend = &preview_backend;
  jxl::extras::PackedPixelFile preview;
  size_t preview_decoded_bytes = 0;
  ASSERT_TRUE(jxl::extras::DecodeImageJXL(compressed.data(), compressed.size(),
                                          preview_params,
                                          &preview_decoded_bytes, &preview));
  EXPECT_EQ(expected_backend, preview_backend);
  if (expected_backend == jxl::extras::JXLPreviewBackend::kFallbackDownsample) {
    EXPECT_EQ(compressed.size(), preview_decoded_bytes);
  } else if (expected_backend !=
                 jxl::extras::JXLPreviewBackend::kNativeReducedInput &&
             expected_backend !=
                 jxl::extras::JXLPreviewBackend::kNativeFusedUpsampling) {
    // Progressive and DC-only native paths stop reading the stream early.
    // Native reduced/fused paths run the pipeline at preview/intermediate
    // resolution but may consume the full stream (e.g. non-responsive modular
    // has no sub-passes to stop after; only pipeline cost is reduced, not
    // bytes read).
    EXPECT_LT(preview_decoded_bytes, compressed.size());
  }

  ASSERT_EQ(jxl::DivCeil(xsize, factor), preview.info.xsize);
  ASSERT_EQ(jxl::DivCeil(ysize, factor), preview.info.ysize);
  ASSERT_EQ(1u, preview.frames.size());
  ASSERT_EQ(preview.info.xsize, preview.frames[0].color.xsize);
  ASSERT_EQ(preview.info.ysize, preview.frames[0].color.ysize);
  ASSERT_EQ(full.frames[0].color.format.num_channels,
            preview.frames[0].color.format.num_channels);

  if (expected_backend == jxl::extras::JXLPreviewBackend::kFallbackDownsample) {
    constexpr float kPreviewFallbackTolerance = 1.0f / 255.0f + 1e-6f;
    for (size_t y = 0; y < preview.frames[0].color.ysize; ++y) {
      for (size_t x = 0; x < preview.frames[0].color.xsize; ++x) {
        for (size_t c = 0; c < preview.frames[0].color.format.num_channels;
             ++c) {
          EXPECT_NEAR(BoxAveragePixel(full.frames[0].color, factor, x, y, c),
                      preview.frames[0].color.GetPixelValue(y, x, c),
                      kPreviewFallbackTolerance);
        }
      }
    }
  } else {
    // VarDCT DC coefficients are 8x8 block averages, not aligned with the
    // downsampling factor's box grid, so individual pixels can differ
    // substantially from box averages. Use mean absolute error (MAE) which
    // catches systematic errors (all-zeros, wrong colorspace) while tolerating
    // per-pixel deviations inherent to the DC approximation.
    const jxl::extras::PackedImage& image = preview.frames[0].color;
    const size_t num_channels = image.format.num_channels;
    std::vector<float> expected(image.xsize * image.ysize * num_channels);
    for (size_t y = 0; y < image.ysize; ++y) {
      for (size_t x = 0; x < image.xsize; ++x) {
        for (size_t c = 0; c < num_channels; ++c) {
          expected[(y * image.xsize + x) * num_channels + c] =
              BoxAveragePixel(full.frames[0].color, factor, x, y, c);
        }
      }
    }
    // The MAE against the box averages `dx`, `dy` pixels away, over the
    // pixels that have neighbors on every side.
    const auto mae_at_offset = [&](int dx, int dy) -> double {
      double sum_diff = 0.0;
      size_t count = 0;
      for (size_t y = 1; y + 1 < image.ysize; ++y) {
        const size_t ey = static_cast<size_t>(static_cast<ptrdiff_t>(y) + dy);
        for (size_t x = 1; x + 1 < image.xsize; ++x) {
          const size_t ex = static_cast<size_t>(static_cast<ptrdiff_t>(x) + dx);
          for (size_t c = 0; c < num_channels; ++c) {
            sum_diff +=
                std::abs(expected[(ey * image.xsize + ex) * num_channels + c] -
                         image.GetPixelValue(y, x, c));
            ++count;
          }
        }
      }
      return sum_diff / count;
    };
    ASSERT_GE(image.xsize, 3u);
    ASSERT_GE(image.ysize, 3u);
    const double mae = mae_at_offset(0, 0);
    EXPECT_LT(mae, max_mae) << "Non-fallback preview mean absolute error "
                            << mae << " exceeds tolerance " << max_mae;
    // Each preview pixel matches its own box better than its neighbors' (a
    // preview shifted by a pixel can be close on average).
    for (const auto& offset : {std::make_pair(1, 0), std::make_pair(-1, 0),
                               std::make_pair(0, 1), std::make_pair(0, -1)}) {
      EXPECT_LT(mae, mae_at_offset(offset.first, offset.second))
          << "offset " << offset.first << "," << offset.second;
    }
  }
}

void VerifyPreviewDownsamplingRoundtrip(
    const jxl::TestCodestreamParams& params, size_t xsize, size_t ysize,
    uint32_t num_channels, size_t factor,
    jxl::extras::JXLPreviewBackend expected_backend, double max_mae) {
  std::vector<uint8_t> pixels =
      jxl::test::GetSomeTestImage(xsize, ysize, num_channels, 0);
  std::vector<uint8_t> compressed =
      jxl::CreateTestJXLCodestream(jxl::Bytes(pixels.data(), pixels.size()),
                                   xsize, ysize, num_channels, params);
  VerifyPreviewDownsamplingOfCodestream(compressed, xsize, ysize, factor,
                                        expected_backend, max_mae);
}

void VerifyPreviewBackendForCodestream(
    const std::vector<uint8_t>& compressed, size_t xsize, size_t ysize,
    uint32_t expected_num_channels, size_t factor,
    jxl::extras::JXLPreviewBackend expected_backend) {
  jxl::extras::JXLDecompressParams preview_params;
  jxl::test::DefaultAcceptedFormats(preview_params);
  preview_params.preview_downsampling = factor;
  preview_params.preview_hooks = jxl::GetDecoderPreviewHooks();
  jxl::extras::JXLPreviewBackend preview_backend =
      jxl::extras::JXLPreviewBackend::kNone;
  preview_params.preview_backend = &preview_backend;
  jxl::extras::PackedPixelFile preview;
  size_t preview_decoded_bytes = 0;
  ASSERT_TRUE(jxl::extras::DecodeImageJXL(compressed.data(), compressed.size(),
                                          preview_params,
                                          &preview_decoded_bytes, &preview));

  EXPECT_EQ(expected_backend, preview_backend);
  if (expected_backend == jxl::extras::JXLPreviewBackend::kFallbackDownsample) {
    EXPECT_EQ(compressed.size(), preview_decoded_bytes);
  } else if (expected_backend !=
                 jxl::extras::JXLPreviewBackend::kNativeReducedInput &&
             expected_backend !=
                 jxl::extras::JXLPreviewBackend::kNativeFusedUpsampling) {
    EXPECT_LT(preview_decoded_bytes, compressed.size());
  }

  ASSERT_EQ(jxl::DivCeil(xsize, factor), preview.info.xsize);
  ASSERT_EQ(jxl::DivCeil(ysize, factor), preview.info.ysize);
  ASSERT_EQ(1u, preview.frames.size());
  ASSERT_EQ(preview.info.xsize, preview.frames[0].color.xsize);
  ASSERT_EQ(preview.info.ysize, preview.frames[0].color.ysize);
  ASSERT_EQ(expected_num_channels, preview.frames[0].color.format.num_channels);
}

std::vector<uint8_t> CreateSpotColorPreviewCodestream(size_t xsize,
                                                      size_t ysize) {
  JxlMemoryManager* memory_manager = jxl::test::MemoryManager();
  auto io = jxl::make_unique<jxl::CodecInOut>(memory_manager);
  io->metadata.m.color_encoding = jxl::ColorEncoding::LinearSRGB();

  JXL_TEST_ASSIGN_OR_DIE(jxl::Image3F color,
                         jxl::Image3F::Create(memory_manager, xsize, ysize));
  JXL_TEST_ASSIGN_OR_DIE(jxl::ImageF spot,
                         jxl::ImageF::Create(memory_manager, xsize, ysize));
  for (size_t y = 0; y < ysize; ++y) {
    float* JXL_RESTRICT row0 = color.PlaneRow(0, y);
    float* JXL_RESTRICT row1 = color.PlaneRow(1, y);
    float* JXL_RESTRICT row2 = color.PlaneRow(2, y);
    float* JXL_RESTRICT row_spot = spot.Row(y);
    for (size_t x = 0; x < xsize; ++x) {
      row0[x] = (x & 255) * (1.0f / 255.0f);
      row1[x] = (y & 255) * (1.0f / 255.0f);
      row2[x] = ((x + y) & 255) * (1.0f / 255.0f);
      row_spot[x] = ((x ^ y) & 255) * (1.0f / 255.0f);
    }
  }

  EXPECT_TRUE(
      io->SetFromImage(std::move(color), jxl::ColorEncoding::LinearSRGB()));
  jxl::ExtraChannelInfo info;
  info.bit_depth.bits_per_sample = 8;
  info.dim_shift = 0;
  info.type = jxl::ExtraChannel::kSpotColor;
  info.spot_color[0] = 0.5f;
  info.spot_color[1] = 0.2f;
  info.spot_color[2] = 1.0f;
  info.spot_color[3] = 0.5f;
  io->metadata.m.extra_channel_info.push_back(info);
  std::vector<jxl::ImageF> extra_channels;
  extra_channels.push_back(std::move(spot));
  EXPECT_TRUE(io->frames[0].SetExtraChannels(std::move(extra_channels)));

  jxl::CompressParams cparams;
  cparams.SetLossless();
  cparams.speed_tier = jxl::SpeedTier::kThunder;
  cparams.responsive = 0;
  std::vector<uint8_t> compressed;
  EXPECT_TRUE(jxl::test::EncodeFile(cparams, io.get(), &compressed));
  return compressed;
}

std::vector<uint8_t> CreateReferenceableFirstFrameCodestream(
    size_t xsize, size_t ysize, uint32_t num_channels) {
  JxlMemoryManager* memory_manager = jxl::test::MemoryManager();
  auto io = jxl::make_unique<jxl::CodecInOut>(memory_manager);
  EXPECT_TRUE(io->SetSize(xsize, ysize));
  io->metadata.m.SetUintSamples(16);
  if (num_channels == 4) {
    io->metadata.m.SetAlphaBits(16);
  }
  io->metadata.m.color_encoding = jxl::ColorEncoding::SRGB(false);
  io->metadata.m.have_animation = true;
  io->frames.clear();
  EXPECT_TRUE(io->SetSize(xsize, ysize));

  const JxlPixelFormat format = {num_channels, JXL_TYPE_UINT16, JXL_BIG_ENDIAN,
                                 0};
  for (size_t frame = 0; frame < 2; ++frame) {
    std::vector<uint8_t> pixels =
        jxl::test::GetSomeTestImage(xsize, ysize, num_channels, frame);
    jxl::ImageBundle bundle(memory_manager, &io->metadata.m);
    EXPECT_TRUE(ConvertFromExternal(
        jxl::Bytes(pixels.data(), pixels.size()), xsize, ysize,
        jxl::ColorEncoding::SRGB(false), /*bits_per_sample=*/16, format,
        /*pool=*/nullptr, &bundle, /*set_alpha=*/num_channels == 4));
    bundle.duration = 1;
    bundle.use_for_next_frame = frame == 0;
    io->frames.push_back(std::move(bundle));
  }

  jxl::CompressParams cparams;
  cparams.progressive_dc = 0;
  cparams.responsive = 0;
  cparams.speed_tier = jxl::SpeedTier::kThunder;
  std::vector<uint8_t> compressed;
  EXPECT_TRUE(jxl::test::EncodeFile(cparams, io.get(), &compressed));
  return compressed;
}

void VerifyPreviewInplaceFlushResponsiveModular(size_t xsize, size_t ysize,
                                                uint32_t num_channels,
                                                size_t factor) {
  std::vector<uint8_t> pixels =
      jxl::test::GetSomeTestImage(xsize, ysize, num_channels, 0);
  jxl::TestCodestreamParams params;
  params.cparams.SetLossless();
  params.cparams.speed_tier = jxl::SpeedTier::kThunder;
  params.cparams.responsive = 1;
  params.cparams.modular_group_size_shift = 1;
  std::vector<uint8_t> compressed =
      jxl::CreateTestJXLCodestream(jxl::Bytes(pixels.data(), pixels.size()),
                                   xsize, ysize, num_channels, params);

  JxlDecoderPtr dec = JxlDecoderMake(nullptr);
  ASSERT_NE(dec, nullptr);
  ASSERT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSubscribeEvents(
                dec.get(), JXL_DEC_BASIC_INFO | JXL_DEC_FRAME |
                               JXL_DEC_FRAME_PROGRESSION | JXL_DEC_FULL_IMAGE));

  const size_t initial_bytes =
      std::min(compressed.size() - 1,
               std::max(compressed.size() / 8, static_cast<size_t>(1 << 16)));
  ASSERT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSetInput(dec.get(), compressed.data(), initial_bytes));
  size_t supplied_bytes = initial_bytes;

  const JxlPixelFormat format = {4, JXL_TYPE_UINT8, JXL_LITTLE_ENDIAN, 0};
  std::vector<uint8_t> preview;
  bool output_set = false;
  bool flushed = false;

  for (;;) {
    const JxlDecoderStatus status = JxlDecoderProcessInput(dec.get());
    if (status == JXL_DEC_BASIC_INFO) {
      continue;
    }
    if (status == JXL_DEC_FRAME) {
      ASSERT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderSetImageOutDownsampling(dec.get(), factor));
      ASSERT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderSetPreferPreviewInplaceFlush(dec.get(), JXL_TRUE));
      size_t buffer_size = 0;
      ASSERT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderImageOutBufferSize(dec.get(), &format, &buffer_size));
      preview.resize(buffer_size);
      ASSERT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderSetImageOutBuffer(dec.get(), &format, preview.data(),
                                            preview.size()));
      output_set = true;
      continue;
    }
    if (status == JXL_DEC_FRAME_PROGRESSION) {
      ASSERT_TRUE(output_set);
      ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderFlushImage(dec.get()));
      flushed = true;
      break;
    }
    if (status == JXL_DEC_NEED_MORE_INPUT) {
      size_t remaining = JxlDecoderReleaseInput(dec.get());
      const size_t consumed = supplied_bytes - remaining;
      ASSERT_LT(consumed, compressed.size());
      const size_t doubled = supplied_bytes > compressed.size() / 2
                                 ? compressed.size()
                                 : supplied_bytes * 2;
      const size_t next_supplied_bytes = std::min(
          compressed.size(), std::max(doubled, supplied_bytes + (1 << 16)));
      ASSERT_GT(next_supplied_bytes, supplied_bytes);
      ASSERT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderSetInput(dec.get(), compressed.data() + consumed,
                                   next_supplied_bytes - consumed));
      supplied_bytes = next_supplied_bytes;
      continue;
    }
    ASSERT_NE(JXL_DEC_ERROR, status);
    ASSERT_NE(JXL_DEC_SUCCESS, status);
  }

  ASSERT_TRUE(flushed);
  // The flush may have reused the frame's storage: decoding ends here.
  EXPECT_EQ(JXL_DEC_ERROR, JxlDecoderProcessInput(dec.get()));
  ASSERT_FALSE(preview.empty());
  EXPECT_EQ(jxl::DivCeil(xsize, factor) * jxl::DivCeil(ysize, factor) *
                format.num_channels,
            preview.size());
  uint64_t sum = 0;
  for (uint8_t v : preview) {
    sum += v;
  }
  EXPECT_GT(sum, 0u);
}

// progressive_dc adds no AC passes: the only progression step is the DC, too
// coarse for a factor 4 preview, so the AC is decoded at reduced resolution.
TEST(DecodeTest, PreviewDownsamplingVarDCT) {
  jxl::TestCodestreamParams params;
  params.cparams.progressive_dc = 1;
  params.cparams.responsive = 1;
  // Measured: 0.015. Point sampling gives about 0.058.
  VerifyPreviewDownsamplingRoundtrip(
      params, /*xsize=*/333, /*ysize=*/300, /*num_channels=*/3,
      /*factor=*/4, jxl::extras::JXLPreviewBackend::kNativeReducedInput,
      /*max_mae=*/0.03);
}

// With AC passes there are progression steps at 1/4 and 1/2 resolution, and
// a factor 2 preview is flushed at the latter without reading the rest.
TEST(DecodeTest, PreviewDownsamplingVarDCTProgressivePassesFactor2) {
  constexpr size_t xsize = 1346;
  constexpr size_t ysize = 732;
  jxl::TestCodestreamParams params;
  params.cparams.progressive_mode = jxl::Override::kOn;
  const std::vector<uint8_t> compressed =
      CreateDCOnlyTestCodestream(xsize, ysize, 3, params);
  // Measured: 0.0072. Flushed at the 1/4 step instead: 0.0083, too close to
  // tell apart, so the steps are told apart by the bytes read below.
  VerifyPreviewDownsamplingOfCodestream(
      compressed, xsize, ysize, /*factor=*/2,
      jxl::extras::JXLPreviewBackend::kNativeProgressionFlush,
      /*max_mae=*/0.015);

  // The bytes the decoder has read at the 1/4 and 1/2 progression steps.
  size_t quarter_step_bytes = 0;
  size_t half_step_bytes = 0;
  JxlDecoderPtr dec = JxlDecoderMake(nullptr);
  ASSERT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSubscribeEvents(
                dec.get(), JXL_DEC_FRAME_PROGRESSION | JXL_DEC_FULL_IMAGE));
  ASSERT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSetProgressiveDetail(dec.get(), kPasses));
  // The input is released at each step to count the bytes read, so it is not
  // closed (a closed input cannot be set again).
  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec.get(), compressed.data(),
                                                compressed.size()));
  const JxlPixelFormat format = {3, JXL_TYPE_FLOAT, JXL_NATIVE_ENDIAN, 0};
  std::vector<uint8_t> pixels;
  for (;;) {
    const JxlDecoderStatus status = JxlDecoderProcessInput(dec.get());
    if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
      size_t buffer_size;
      ASSERT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderImageOutBufferSize(dec.get(), &format, &buffer_size));
      pixels.resize(buffer_size);
      ASSERT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderSetImageOutBuffer(dec.get(), &format, pixels.data(),
                                            pixels.size()));
    } else if (status == JXL_DEC_FRAME_PROGRESSION) {
      const size_t remaining = JxlDecoderReleaseInput(dec.get());
      const size_t ratio = JxlDecoderGetIntendedDownsamplingRatio(dec.get());
      if (ratio == 4) quarter_step_bytes = compressed.size() - remaining;
      if (ratio == 2) half_step_bytes = compressed.size() - remaining;
      ASSERT_EQ(
          JXL_DEC_SUCCESS,
          JxlDecoderSetInput(dec.get(),
                             compressed.data() + compressed.size() - remaining,
                             remaining));
    } else {
      ASSERT_EQ(JXL_DEC_FULL_IMAGE, status);
      break;
    }
  }
  ASSERT_NE(0u, quarter_step_bytes);
  ASSERT_NE(0u, half_step_bytes);

  jxl::extras::JXLDecompressParams preview_params;
  jxl::test::DefaultAcceptedFormats(preview_params);
  preview_params.preview_downsampling = 2;
  preview_params.preview_hooks = jxl::GetDecoderPreviewHooks();
  jxl::extras::PackedPixelFile preview;
  size_t preview_decoded_bytes = 0;
  ASSERT_TRUE(jxl::extras::DecodeImageJXL(compressed.data(), compressed.size(),
                                          preview_params,
                                          &preview_decoded_bytes, &preview));
  // Flushed at the 1/2 step: past the 1/4 step, and no further.
  EXPECT_LT(quarter_step_bytes, preview_decoded_bytes);
  EXPECT_LE(preview_decoded_bytes, half_step_bytes);
}

// A single-pass VarDCT frame has no progression step finer than the DC, which
// the decoder pauses at even when the whole file is available. Factor 2 and 4
// previews must not be flushed there; they decode the AC through the
// reduced-input path instead.
TEST(DecodeTest, PreviewDownsamplingVarDCTSimpleFactor2) {
  jxl::TestCodestreamParams params;
  params.cparams.progressive_dc = 0;
  params.cparams.responsive = 0;
  params.cparams.speed_tier = jxl::SpeedTier::kThunder;
  // Measured: 0.0045. Point sampling gives about 0.012, a preview from the
  // DC about 0.012.
  VerifyPreviewDownsamplingRoundtrip(
      params, /*xsize=*/320, /*ysize=*/240, /*num_channels=*/3,
      /*factor=*/2, jxl::extras::JXLPreviewBackend::kNativeReducedInput,
      /*max_mae=*/0.01);
}

TEST(DecodeTest, PreviewDownsamplingVarDCTSimpleFactor4) {
  jxl::TestCodestreamParams params;
  params.cparams.progressive_dc = 0;
  params.cparams.responsive = 0;
  params.cparams.speed_tier = jxl::SpeedTier::kThunder;
  // Measured: 0.0036.
  VerifyPreviewDownsamplingRoundtrip(
      params, /*xsize=*/320, /*ysize=*/240, /*num_channels=*/3,
      /*factor=*/4, jxl::extras::JXLPreviewBackend::kNativeReducedInput,
      /*max_mae=*/0.008);
}

// The decoder pauses at the DC step of a multi-group VarDCT frame, which has
// only 1/8 detail; a factor 2 preview must not be flushed there.
TEST(DecodeTest, PreviewDownsamplingVarDCTLargeInputFactor2) {
  constexpr size_t xsize = 2048;
  constexpr size_t ysize = 1024;
  // 16-bit big-endian RGB: random 4x4 tiles plus mild per-pixel noise. The
  // noise keeps the codestream above 1 MiB; the tiles survive 2x2 box
  // averaging but not the 8x8 averaging of the DC. A factor 2 preview with
  // full detail measures a mean absolute error of about 0.027 here, one
  // flushed at the DC step about 0.16.
  constexpr double kMaxMAE = 0.06;
  constexpr size_t tiles_per_row = xsize / 4;
  uint32_t state = 12345;
  const auto next_random = [&state]() -> uint32_t {
    state = state * 1664525u + 1013904223u;
    return state >> 16;
  };
  std::vector<uint32_t> tiles(tiles_per_row * (ysize / 4) * 3);
  for (uint32_t& tile : tiles) tile = next_random();
  std::vector<uint8_t> pixels(xsize * ysize * 3 * 2);
  for (size_t y = 0; y < ysize; ++y) {
    for (size_t x = 0; x < xsize; ++x) {
      for (size_t c = 0; c < 3; ++c) {
        const uint32_t tile = tiles[((y / 4) * tiles_per_row + x / 4) * 3 + c];
        const uint32_t value = tile * 3 / 4 + (next_random() >> 3);
        uint8_t* p = &pixels[((y * xsize + x) * 3 + c) * 2];
        p[0] = static_cast<uint8_t>(value >> 8);
        p[1] = static_cast<uint8_t>(value & 0xFF);
      }
    }
  }
  jxl::TestCodestreamParams params;
  params.cparams.progressive_dc = 0;
  params.cparams.responsive = 0;
  params.cparams.speed_tier = jxl::SpeedTier::kThunder;
  std::vector<uint8_t> compressed = jxl::CreateTestJXLCodestream(
      jxl::Bytes(pixels.data(), pixels.size()), xsize, ysize, 3, params);
  ASSERT_GE(compressed.size(), static_cast<size_t>(1) << 20);
  VerifyPreviewDownsamplingOfCodestream(
      compressed, xsize, ysize, /*factor=*/2,
      jxl::extras::JXLPreviewBackend::kNativeReducedInput, kMaxMAE);
}

TEST(DecodeTest, PreviewDownsamplingVarDCTLargeProgressive) {
  jxl::TestCodestreamParams params;
  params.cparams.progressive_dc = 1;
  params.cparams.responsive = 1;
  // Measured: 0.0047.
  VerifyPreviewDownsamplingRoundtrip(
      params, /*xsize=*/1346, /*ysize=*/732, /*num_channels=*/3,
      /*factor=*/4, jxl::extras::JXLPreviewBackend::kNativeReducedInput,
      /*max_mae=*/0.01);
}

TEST(DecodeTest, PreviewDownsamplingVarDCTNativeDcOnly) {
  jxl::TestCodestreamParams params;
  params.cparams.progressive_dc = 1;
  params.cparams.responsive = 1;
  // Measured: 0.0058. Point sampling gives about 0.03.
  VerifyPreviewDownsamplingRoundtrip(
      params, /*xsize=*/1346, /*ysize=*/732, /*num_channels=*/3,
      /*factor=*/8, jxl::extras::JXLPreviewBackend::kNativeDcOnly,
      /*max_mae=*/0.012);
}

TEST(DecodeTest, PreviewDownsamplingVarDCTAlphaFactor8NativeReducedInput) {
  jxl::TestCodestreamParams params;
  params.cparams.progressive_dc = 0;
  params.cparams.responsive = 0;
  params.cparams.speed_tier = jxl::SpeedTier::kThunder;
  // Measured: 0.0026.
  VerifyPreviewDownsamplingRoundtrip(
      params, /*xsize=*/320, /*ysize=*/240, /*num_channels=*/4,
      /*factor=*/8, jxl::extras::JXLPreviewBackend::kNativeReducedInput,
      /*max_mae=*/0.006);
}

TEST(DecodeTest, PreviewDownsamplingVarDCTAlphaFactor2NativeReducedInput) {
  jxl::TestCodestreamParams params;
  params.cparams.progressive_dc = 0;
  params.cparams.responsive = 0;
  params.cparams.speed_tier = jxl::SpeedTier::kThunder;
  // Measured: 0.0033.
  VerifyPreviewDownsamplingRoundtrip(
      params, /*xsize=*/320, /*ysize=*/240, /*num_channels=*/4,
      /*factor=*/2, jxl::extras::JXLPreviewBackend::kNativeReducedInput,
      /*max_mae=*/0.007);
}

TEST(DecodeTest, PreviewDownsamplingVarDCTAlphaFactor4NativeReducedInput) {
  jxl::TestCodestreamParams params;
  params.cparams.progressive_dc = 0;
  params.cparams.responsive = 0;
  params.cparams.speed_tier = jxl::SpeedTier::kThunder;
  // Measured: 0.0027.
  VerifyPreviewDownsamplingRoundtrip(
      params, /*xsize=*/320, /*ysize=*/240, /*num_channels=*/4,
      /*factor=*/4, jxl::extras::JXLPreviewBackend::kNativeReducedInput,
      /*max_mae=*/0.006);
}

TEST(DecodeTest, PreviewDownsamplingReferenceableFirstFrameReducedInput) {
  std::vector<uint8_t> compressed = CreateReferenceableFirstFrameCodestream(
      /*xsize=*/320, /*ysize=*/240, /*num_channels=*/4);
  VerifyPreviewBackendForCodestream(
      compressed, /*xsize=*/320, /*ysize=*/240, /*expected_num_channels=*/4,
      /*factor=*/4, jxl::extras::JXLPreviewBackend::kNativeReducedInput);
}

TEST(DecodeTest, PreviewDownsamplingResponsiveModularLossless) {
  jxl::TestCodestreamParams params;
  params.cparams.SetLossless();
  params.cparams.speed_tier = jxl::SpeedTier::kThunder;
  params.cparams.responsive = 1;
  params.cparams.modular_group_size_shift = 1;
  // Lossless previews are exact box averages: measured below 1e-5.
  VerifyPreviewDownsamplingRoundtrip(
      params, /*xsize=*/333, /*ysize=*/300, /*num_channels=*/4,
      /*factor=*/4, jxl::extras::JXLPreviewBackend::kNativeReducedInput,
      /*max_mae=*/0.001);
}

// The DC step of a responsive modular frame is exactly 1/8 resolution, so a
// factor 8 preview is flushed there.
TEST(DecodeTest, PreviewDownsamplingResponsiveModularLosslessFactor8) {
  jxl::TestCodestreamParams params;
  params.cparams.SetLossless();
  params.cparams.speed_tier = jxl::SpeedTier::kThunder;
  params.cparams.responsive = 1;
  params.cparams.modular_group_size_shift = 1;
  // Lossless previews are exact box averages: measured below 1e-5.
  VerifyPreviewDownsamplingRoundtrip(
      params, /*xsize=*/333, /*ysize=*/300, /*num_channels=*/4,
      /*factor=*/8, jxl::extras::JXLPreviewBackend::kNativeProgressionFlush,
      /*max_mae=*/0.001);
}

TEST(DecodeTest, PreviewDownsamplingModularLossless) {
  jxl::TestCodestreamParams params;
  params.cparams.SetLossless();
  params.cparams.speed_tier = jxl::SpeedTier::kThunder;
  params.cparams.responsive = 0;
  // Lossless previews are exact box averages: measured below 1e-5.
  VerifyPreviewDownsamplingRoundtrip(
      params, /*xsize=*/65, /*ysize=*/47, /*num_channels=*/4,
      /*factor=*/4, jxl::extras::JXLPreviewBackend::kNativeReducedInput,
      /*max_mae=*/0.001);
}

TEST(DecodeTest, PreviewDownsamplingModularAlphaFactor2NativeReducedInput) {
  jxl::TestCodestreamParams params;
  params.cparams.SetLossless();
  params.cparams.speed_tier = jxl::SpeedTier::kThunder;
  params.cparams.responsive = 0;
  // Lossless previews are exact box averages: measured below 1e-5.
  VerifyPreviewDownsamplingRoundtrip(
      params, /*xsize=*/65, /*ysize=*/47, /*num_channels=*/4,
      /*factor=*/2, jxl::extras::JXLPreviewBackend::kNativeReducedInput,
      /*max_mae=*/0.001);
}

TEST(DecodeTest, PreviewDownsamplingModularAlphaFactor8NativeReducedInput) {
  jxl::TestCodestreamParams params;
  params.cparams.SetLossless();
  params.cparams.speed_tier = jxl::SpeedTier::kThunder;
  params.cparams.responsive = 0;
  // Lossless previews are exact box averages: measured below 1e-5.
  VerifyPreviewDownsamplingRoundtrip(
      params, /*xsize=*/65, /*ysize=*/47, /*num_channels=*/4,
      /*factor=*/8, jxl::extras::JXLPreviewBackend::kNativeReducedInput,
      /*max_mae=*/0.001);
}

TEST(DecodeTest, PreviewDownsamplingNonAlphaExtraChannelFallback) {
  std::vector<uint8_t> compressed = CreateSpotColorPreviewCodestream(
      /*xsize=*/80, /*ysize=*/72);
  VerifyPreviewBackendForCodestream(
      compressed, /*xsize=*/80, /*ysize=*/72, /*expected_num_channels=*/3,
      /*factor=*/4, jxl::extras::JXLPreviewBackend::kFallbackDownsample);
}

uint32_t PreviewBackendBit(jxl::extras::JXLPreviewBackend backend) {
  return 1u << static_cast<uint32_t>(backend);
}

TEST(DecodeTest, PreviewDownsamplingFrameUpsamplingFused) {
  jxl::TestCodestreamParams params;
  params.cparams.SetLossless();
  params.cparams.speed_tier = jxl::SpeedTier::kThunder;
  params.cparams.responsive = 0;
  params.cparams.resampling = 2;
  // Measured: 0.0005.
  VerifyPreviewDownsamplingRoundtrip(
      params, /*xsize=*/128, /*ysize=*/96, /*num_channels=*/3,
      /*factor=*/4, jxl::extras::JXLPreviewBackend::kNativeFusedUpsampling,
      /*max_mae=*/0.002);
}

TEST(DecodeTest, PreviewDownsamplingModularFrameUpsamplingFusedMatrix) {
  for (size_t upsampling : {2u, 4u, 8u}) {
    for (size_t factor : {2u, 4u, 8u}) {
      SCOPED_TRACE(::testing::Message()
                   << "upsampling " << upsampling << ", factor " << factor);
      jxl::TestCodestreamParams params;
      params.cparams.SetLossless();
      params.cparams.speed_tier = jxl::SpeedTier::kThunder;
      params.cparams.responsive = 0;
      params.cparams.resampling = upsampling;
      // Measured: at most 0.0036.
      VerifyPreviewDownsamplingRoundtrip(
          params, /*xsize=*/128, /*ysize=*/96, /*num_channels=*/3, factor,
          jxl::extras::JXLPreviewBackend::kNativeFusedUpsampling,
          /*max_mae=*/0.008);
    }
  }
}

TEST(DecodeTest, PreviewDownsamplingVarDCTAlphaFrameUpsamplingFusedMatrix) {
  for (size_t upsampling : {2u, 4u, 8u}) {
    for (size_t factor : {2u, 4u, 8u}) {
      SCOPED_TRACE(::testing::Message()
                   << "upsampling " << upsampling << ", factor " << factor);
      jxl::TestCodestreamParams params;
      params.cparams.progressive_dc = 0;
      params.cparams.responsive = 0;
      params.cparams.speed_tier = jxl::SpeedTier::kThunder;
      params.cparams.resampling = upsampling;
      params.cparams.ec_resampling = upsampling;
      // Measured: at most 0.009.
      VerifyPreviewDownsamplingRoundtrip(
          params, /*xsize=*/128, /*ysize=*/96, /*num_channels=*/4, factor,
          jxl::extras::JXLPreviewBackend::kNativeFusedUpsampling,
          /*max_mae=*/0.02);
    }
  }
}

TEST(DecodeTest, PreviewDownsamplingFrameUpsamplingAllowedBackends) {
  std::vector<uint8_t> pixels =
      jxl::test::GetSomeTestImage(/*xsize=*/128, /*ysize=*/96,
                                  /*num_channels=*/3, 0);
  jxl::TestCodestreamParams params;
  params.cparams.SetLossless();
  params.cparams.speed_tier = jxl::SpeedTier::kThunder;
  params.cparams.responsive = 0;
  params.cparams.resampling = 2;
  std::vector<uint8_t> compressed = jxl::CreateTestJXLCodestream(
      jxl::Bytes(pixels.data(), pixels.size()), /*xsize=*/128, /*ysize=*/96,
      /*num_channels=*/3, params);

  auto decode_with_mask = [&](uint32_t mask,
                              jxl::extras::JXLPreviewBackend* backend) {
    jxl::extras::JXLDecompressParams preview_params;
    jxl::test::DefaultAcceptedFormats(preview_params);
    preview_params.preview_downsampling = 4;
    preview_params.preview_hooks = jxl::GetDecoderPreviewHooks();
    preview_params.preview_allowed_backends = mask;
    preview_params.preview_backend = backend;
    jxl::extras::PackedPixelFile preview;
    return jxl::extras::DecodeImageJXL(compressed.data(), compressed.size(),
                                       preview_params, nullptr, &preview);
  };

  jxl::extras::JXLPreviewBackend backend =
      jxl::extras::JXLPreviewBackend::kNone;
  EXPECT_TRUE(decode_with_mask(
      PreviewBackendBit(jxl::extras::JXLPreviewBackend::kNativeFusedUpsampling),
      &backend));
  EXPECT_EQ(jxl::extras::JXLPreviewBackend::kNativeFusedUpsampling, backend);

  backend = jxl::extras::JXLPreviewBackend::kNone;
  EXPECT_TRUE(decode_with_mask(
      PreviewBackendBit(jxl::extras::JXLPreviewBackend::kFallbackDownsample),
      &backend));
  EXPECT_EQ(jxl::extras::JXLPreviewBackend::kFallbackDownsample, backend);

  // kDecoderDownsample allows each of the decoder's methods, also when the
  // hooks can tell which one rendered the frame.
  backend = jxl::extras::JXLPreviewBackend::kNone;
  EXPECT_TRUE(decode_with_mask(
      PreviewBackendBit(jxl::extras::JXLPreviewBackend::kDecoderDownsample),
      &backend));
  EXPECT_EQ(jxl::extras::JXLPreviewBackend::kNativeFusedUpsampling, backend);

  jxl::extras::JXLPreviewFailureReason failure_reason =
      jxl::extras::JXLPreviewFailureReason::kNone;
  jxl::extras::JXLDecompressParams preview_params;
  jxl::test::DefaultAcceptedFormats(preview_params);
  preview_params.preview_downsampling = 4;
  preview_params.preview_hooks = jxl::GetDecoderPreviewHooks();
  preview_params.preview_allowed_backends =
      PreviewBackendBit(jxl::extras::JXLPreviewBackend::kNativeReducedInput);
  preview_params.preview_failure_reason = &failure_reason;
  jxl::extras::PackedPixelFile preview;
  EXPECT_FALSE(jxl::extras::DecodeImageJXL(compressed.data(), compressed.size(),
                                           preview_params, nullptr, &preview));
  EXPECT_EQ(jxl::extras::JXLPreviewFailureReason::kNoBackendAvailable,
            failure_reason);
}

TEST(DecodeTest, PreviewDownsamplingLargeModularLossless) {
  jxl::TestCodestreamParams params;
  params.cparams.SetLossless();
  params.cparams.speed_tier = jxl::SpeedTier::kThunder;
  params.cparams.responsive = 0;
  // Lossless previews are exact box averages: measured below 1e-5.
  VerifyPreviewDownsamplingRoundtrip(
      params, /*xsize=*/1346, /*ysize=*/732, /*num_channels=*/4,
      /*factor=*/4, jxl::extras::JXLPreviewBackend::kNativeReducedInput,
      /*max_mae=*/0.001);
}

TEST(DecodeTest, PreviewInplaceFlushResponsiveModular) {
  VerifyPreviewInplaceFlushResponsiveModular(
      /*xsize=*/1346, /*ysize=*/732, /*num_channels=*/4, /*factor=*/4);
}

// Decodes the first frame of `compressed` with output downsampling `factor`
// into `out`, applying `bit_depth` when it is not null. Returns the output
// dimensions and the downsampling method the decoder chose.
void DecodeFirstFrameDownsampled(const std::vector<uint8_t>& compressed,
                                 size_t factor, const JxlPixelFormat& format,
                                 const JxlBitDepth* bit_depth,
                                 std::vector<uint8_t>* out, size_t* out_xsize,
                                 size_t* out_ysize,
                                 JxlImageOutDownsamplingMethod* method) {
  JxlDecoderPtr dec = JxlDecoderMake(nullptr);
  ASSERT_NE(dec, nullptr);
  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSubscribeEvents(
                                 dec.get(), JXL_DEC_BASIC_INFO | JXL_DEC_FRAME |
                                                JXL_DEC_FULL_IMAGE));
  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec.get(), compressed.data(),
                                                compressed.size()));
  JxlDecoderCloseInput(dec.get());
  ASSERT_EQ(JXL_DEC_BASIC_INFO, JxlDecoderProcessInput(dec.get()));
  JxlBasicInfo info;
  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBasicInfo(dec.get(), &info));
  ASSERT_EQ(JXL_DEC_FRAME, JxlDecoderProcessInput(dec.get()));
  ASSERT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSetImageOutDownsampling(dec.get(), factor));
  size_t buffer_size = 0;
  ASSERT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderImageOutBufferSize(dec.get(), &format, &buffer_size));
  out->assign(buffer_size, 0);
  ASSERT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER, JxlDecoderProcessInput(dec.get()));
  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetImageOutBuffer(
                                 dec.get(), &format, out->data(), out->size()));
  if (bit_depth != nullptr) {
    ASSERT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderSetImageOutBitDepth(dec.get(), bit_depth));
  }
  ASSERT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec.get()));
  ASSERT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderGetImageOutDownsamplingMethod(dec.get(), method));
  *out_xsize = jxl::DivCeil(info.xsize, factor);
  *out_ysize = jxl::DivCeil(info.ysize, factor);
}

// Decodes the first frame of `compressed` at 1/8 resolution, which renders a
// VarDCT frame from its DC image.
void DecodeDCOnly(const std::vector<uint8_t>& compressed,
                  const JxlPixelFormat& format, const JxlBitDepth* bit_depth,
                  std::vector<uint8_t>* out, size_t* dc_xsize,
                  size_t* dc_ysize) {
  JxlImageOutDownsamplingMethod method = JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_NONE;
  ASSERT_NO_FATAL_FAILURE(
      DecodeFirstFrameDownsampled(compressed, /*factor=*/8, format, bit_depth,
                                  out, dc_xsize, dc_ysize, &method));
  ASSERT_EQ(JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_DC_ONLY, method);
}

// Mean absolute difference, on a 0-1 scale, between `small`, an 8-bit
// interleaved image downsampled by `factor`, and box averages of `full`, the
// same image at xsize x ysize.
double MeanAbsDiffToBoxAverage(const std::vector<uint8_t>& full, size_t xsize,
                               size_t ysize, size_t num_channels,
                               const std::vector<uint8_t>& small,
                               size_t factor) {
  const size_t small_xsize = jxl::DivCeil(xsize, factor);
  const size_t small_ysize = jxl::DivCeil(ysize, factor);
  double sum = 0.0;
  for (size_t y = 0; y < small_ysize; ++y) {
    for (size_t x = 0; x < small_xsize; ++x) {
      for (size_t c = 0; c < num_channels; ++c) {
        double box = 0.0;
        size_t count = 0;
        for (size_t fy = y * factor; fy < std::min(ysize, (y + 1) * factor);
             ++fy) {
          for (size_t fx = x * factor; fx < std::min(xsize, (x + 1) * factor);
               ++fx) {
            box += full[(fy * xsize + fx) * num_channels + c];
            ++count;
          }
        }
        sum += std::abs(box / count -
                        small[(y * small_xsize + x) * num_channels + c]);
      }
    }
  }
  return sum / (small_xsize * small_ysize * num_channels) / 255.0;
}

// At 1/8 output a VarDCT frame is rendered from its DC image alone.
TEST(DecodeTest, OutputDownsampling8RendersFromDC) {
  constexpr size_t xsize = 256;
  constexpr size_t ysize = 256;
  jxl::TestCodestreamParams params;
  params.cparams.speed_tier = jxl::SpeedTier::kLightning;
  std::vector<uint8_t> compressed =
      CreateDCOnlyTestCodestream(xsize, ysize, 3, params);
  const JxlPixelFormat format = {3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};

  std::vector<uint8_t> full;
  size_t full_xsize = 0;
  size_t full_ysize = 0;
  JxlImageOutDownsamplingMethod method =
      JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_DC_ONLY;
  ASSERT_NO_FATAL_FAILURE(
      DecodeFirstFrameDownsampled(compressed, /*factor=*/1, format, nullptr,
                                  &full, &full_xsize, &full_ysize, &method));
  EXPECT_EQ(JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_NONE, method);

  std::vector<uint8_t> dc;
  size_t dc_xsize = 0;
  size_t dc_ysize = 0;
  ASSERT_NO_FATAL_FAILURE(
      DecodeDCOnly(compressed, format, nullptr, &dc, &dc_xsize, &dc_ysize));
  EXPECT_EQ(xsize / 8, dc_xsize);
  EXPECT_EQ(ysize / 8, dc_ysize);
  ASSERT_EQ(dc_xsize * dc_ysize * 3, dc.size());
  EXPECT_LT(MeanAbsDiffToBoxAverage(full, xsize, ysize, 3, dc, 8), 0.02);
}

TEST(DecodeTest, OutputDownsamplingDoesNotPersistAcrossRewind) {
  constexpr size_t xsize = 256;
  constexpr size_t ysize = 256;
  jxl::TestCodestreamParams params;
  params.cparams.speed_tier = jxl::SpeedTier::kLightning;
  std::vector<uint8_t> compressed =
      CreateDCOnlyTestCodestream(xsize, ysize, 3, params);

  JxlPixelFormat format = {3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
  JxlDecoderPtr dec = JxlDecoderMake(nullptr);
  ASSERT_NE(dec, nullptr);

  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSubscribeEvents(
                                 dec.get(), JXL_DEC_BASIC_INFO | JXL_DEC_FRAME |
                                                JXL_DEC_FULL_IMAGE));
  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec.get(), compressed.data(),
                                                compressed.size()));

  ASSERT_EQ(JXL_DEC_BASIC_INFO, JxlDecoderProcessInput(dec.get()));
  ASSERT_EQ(JXL_DEC_FRAME, JxlDecoderProcessInput(dec.get()));
  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetImageOutDownsampling(dec.get(), 8));
  size_t downsampled_buffer_size = 0;
  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderImageOutBufferSize(
                                 dec.get(), &format, &downsampled_buffer_size));
  EXPECT_EQ((xsize / 8) * (ysize / 8) * format.num_channels,
            downsampled_buffer_size);

  JxlDecoderRewind(dec.get());
  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSubscribeEvents(
                                 dec.get(), JXL_DEC_BASIC_INFO | JXL_DEC_FRAME |
                                                JXL_DEC_FULL_IMAGE));
  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec.get(), compressed.data(),
                                                compressed.size()));

  ASSERT_EQ(JXL_DEC_BASIC_INFO, JxlDecoderProcessInput(dec.get()));
  ASSERT_EQ(JXL_DEC_FRAME, JxlDecoderProcessInput(dec.get()));
  size_t full_buffer_size = 0;
  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderImageOutBufferSize(dec.get(), &format,
                                                          &full_buffer_size));
  EXPECT_EQ(xsize * ysize * format.num_channels, full_buffer_size);
}

TEST(DecodeTest, DCOnlyUint16MatchesUint8) {
  constexpr size_t xsize = 256;
  constexpr size_t ysize = 256;
  jxl::TestCodestreamParams params;
  params.cparams.speed_tier = jxl::SpeedTier::kLightning;
  std::vector<uint8_t> compressed =
      CreateDCOnlyTestCodestream(xsize, ysize, 3, params);

  std::vector<uint8_t> buf8;
  std::vector<uint8_t> buf16;
  size_t dc_xsize = 0;
  size_t dc_ysize = 0;
  ASSERT_NO_FATAL_FAILURE(
      DecodeDCOnly(compressed, {3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0},
                   nullptr, &buf8, &dc_xsize, &dc_ysize));
  ASSERT_NO_FATAL_FAILURE(
      DecodeDCOnly(compressed, {3, JXL_TYPE_UINT16, JXL_NATIVE_ENDIAN, 0},
                   nullptr, &buf16, &dc_xsize, &dc_ysize));

  const size_t num_components = dc_xsize * dc_ysize * 3u;
  ASSERT_EQ(num_components, buf8.size());
  ASSERT_EQ(num_components * 2, buf16.size());
  // uint16 ≈ uint8 * 257  (since 65535/255 = 257).
  for (size_t i = 0; i < num_components; ++i) {
    uint16_t v16;
    memcpy(&v16, &buf16[i * 2], 2);
    uint16_t expected = static_cast<uint16_t>(
        static_cast<float>(buf8[i]) / 255.0f * 65535.0f + 0.5f);
    EXPECT_NEAR(v16, expected, 258u)
        << "UINT16/UINT8 mismatch at component " << i;
  }
}

// A non-XYB VarDCT frame with the YCbCr color transform is converted to RGB
// with the BT.601 matrix of stage_ycbcr.cc.
TEST(DecodeTest, DCOnlyYCbCr) {
  // Encode a uniform-color image with the YCbCr color transform, then verify
  // that DC-only decode produces the expected BT.601 output.  Using a uniform
  // image ensures DC = pixel value (no AC detail, minimal quantization error).
  constexpr size_t xsize = 64;
  constexpr size_t ysize = 64;

  // Known input values (16-bit, big-endian, 3 channels).
  // These are interpreted by the encoder as Cb, Y, Cr respectively.
  // Values chosen so that BT.601 YCbCr→RGB output stays within [0,1].
  const uint16_t ch0_u16 = 3277;  // Cb ≈ 0.05
  const uint16_t ch1_u16 = 6554;  // Y  ≈ 0.10
  const uint16_t ch2_u16 = 3277;  // Cr ≈ 0.05
  const float ch0_f = ch0_u16 / 65535.0f;
  const float ch1_f = ch1_u16 / 65535.0f;
  const float ch2_f = ch2_u16 / 65535.0f;

  // Fill pixel buffer: 3 channels, 16-bit big-endian.
  std::vector<uint8_t> pixels(xsize * ysize * 3 * 2);
  for (size_t i = 0; i < xsize * ysize; ++i) {
    const uint16_t vals[3] = {ch0_u16, ch1_u16, ch2_u16};
    for (size_t c = 0; c < 3; ++c) {
      pixels[i * 6 + c * 2 + 0] = static_cast<uint8_t>(vals[c] >> 8);
      pixels[i * 6 + c * 2 + 1] = static_cast<uint8_t>(vals[c] & 0xFF);
    }
  }

  jxl::TestCodestreamParams params;
  params.cparams.color_transform = jxl::ColorTransform::kYCbCr;
  std::vector<uint8_t> compressed = jxl::CreateTestJXLCodestream(
      jxl::Bytes(pixels.data(), pixels.size()), xsize, ysize, 3, params);

  std::vector<uint8_t> dc_buf;
  size_t dc_xsize = 0;
  size_t dc_ysize = 0;
  ASSERT_NO_FATAL_FAILURE(
      DecodeDCOnly(compressed, {3, JXL_TYPE_FLOAT, JXL_NATIVE_ENDIAN, 0},
                   nullptr, &dc_buf, &dc_xsize, &dc_ysize));
  const size_t dc_count = dc_xsize * dc_ysize;
  ASSERT_EQ(dc_count * 3 * sizeof(float), dc_buf.size());
  std::vector<float> dc_pixels(dc_count * 3);
  memcpy(dc_pixels.data(), dc_buf.data(), dc_buf.size());

  // Compute expected output: BT.601 YCbCr→RGB (matching stage_ycbcr.cc).
  // Planes: 0=Cb(ch0), 1=Y(ch1), 2=Cr(ch2).
  const float c128 = 128.0f / 255.0f;
  const float crcr = 1.402f;
  const float cgcb = -0.114f * 1.772f / 0.587f;
  const float cgcr = -0.299f * 1.402f / 0.587f;
  const float cbcb = 1.772f;
  const float y_val = ch1_f + c128;
  const float expected_r = y_val + crcr * ch2_f;
  const float expected_g = y_val + cgcb * ch0_f + cgcr * ch2_f;
  const float expected_b = y_val + cbcb * ch0_f;

  // Average over all DC pixels.
  double avg[3] = {0, 0, 0};
  for (size_t i = 0; i < dc_count; ++i) {
    avg[0] += dc_pixels[i * 3 + 0];
    avg[1] += dc_pixels[i * 3 + 1];
    avg[2] += dc_pixels[i * 3 + 2];
  }
  for (double& v : avg) v /= dc_count;

  // Tolerance: VarDCT quantization on non-XYB data may introduce moderate
  // error, but for a uniform image the DC coefficient is well-preserved.
  constexpr float kTol = 0.05f;
  EXPECT_NEAR(avg[0], expected_r, kTol)
      << "R channel: expected=" << expected_r << " got=" << avg[0];
  EXPECT_NEAR(avg[1], expected_g, kTol)
      << "G channel: expected=" << expected_g << " got=" << avg[1];
  EXPECT_NEAR(avg[2], expected_b, kTol)
      << "B channel: expected=" << expected_b << " got=" << avg[2];
}

// bits_per_sample can be a codestream or custom bit depth smaller than the
// sample (e.g. 12 bits in a 16-bit sample).
TEST(DecodeTest, DCOnlySampleSizeFollowsDataType) {
  constexpr size_t xsize = 256;
  constexpr size_t ysize = 256;
  jxl::TestCodestreamParams params;
  params.cparams.speed_tier = jxl::SpeedTier::kLightning;
  std::vector<uint8_t> compressed =
      CreateDCOnlyTestCodestream(xsize, ysize, 3, params);
  size_t dc_xsize = 0;
  size_t dc_ysize = 0;

  const JxlPixelFormat u16 = {3, JXL_TYPE_UINT16, JXL_NATIVE_ENDIAN, 0};
  std::vector<uint8_t> full_range;
  ASSERT_NO_FATAL_FAILURE(DecodeDCOnly(compressed, u16, nullptr, &full_range,
                                       &dc_xsize, &dc_ysize));
  JxlBitDepth custom12;
  custom12.type = JXL_BIT_DEPTH_CUSTOM;
  custom12.bits_per_sample = 12;
  custom12.exponent_bits_per_sample = 0;
  std::vector<uint8_t> range12;
  ASSERT_NO_FATAL_FAILURE(
      DecodeDCOnly(compressed, u16, &custom12, &range12, &dc_xsize, &dc_ysize));
  const size_t num_samples = dc_xsize * dc_ysize * 3;
  ASSERT_EQ(num_samples * 2, full_range.size());
  ASSERT_EQ(full_range.size(), range12.size());
  size_t mismatches = 0;
  for (size_t i = 0; i < num_samples; ++i) {
    uint16_t v16;
    uint16_t v12;
    memcpy(&v16, &full_range[i * 2], 2);
    memcpy(&v12, &range12[i * 2], 2);
    if (v12 > 4095 || std::abs(v16 * 4095.0 / 65535.0 - v12) > 1.0) {
      ++mismatches;
    }
  }
  EXPECT_EQ(0u, mismatches) << "of " << num_samples << " samples";

  // Float output ignores the bit depth for its values, so the codestream bit
  // depth (16 here) must not change the output at all.
  const JxlPixelFormat f32 = {3, JXL_TYPE_FLOAT, JXL_NATIVE_ENDIAN, 0};
  JxlBitDepth from_codestream;
  from_codestream.type = JXL_BIT_DEPTH_FROM_CODESTREAM;
  from_codestream.bits_per_sample = 0;
  from_codestream.exponent_bits_per_sample = 0;
  std::vector<uint8_t> float_default;
  std::vector<uint8_t> float_codestream;
  ASSERT_NO_FATAL_FAILURE(DecodeDCOnly(compressed, f32, nullptr, &float_default,
                                       &dc_xsize, &dc_ysize));
  ASSERT_NO_FATAL_FAILURE(DecodeDCOnly(compressed, f32, &from_codestream,
                                       &float_codestream, &dc_xsize,
                                       &dc_ysize));
  EXPECT_TRUE(float_default == float_codestream);
}

// Multi-byte DC-only samples must follow the requested endianness.
TEST(DecodeTest, DCOnlyHonorsEndianness) {
  constexpr size_t xsize = 256;
  constexpr size_t ysize = 256;
  jxl::TestCodestreamParams params;
  params.cparams.speed_tier = jxl::SpeedTier::kLightning;
  // RGB source decoded to RGBA, so the alpha samples are the opaque fill.
  std::vector<uint8_t> compressed =
      CreateDCOnlyTestCodestream(xsize, ysize, 3, params);
  size_t dc_xsize = 0;
  size_t dc_ysize = 0;

  for (JxlDataType data_type :
       {JXL_TYPE_UINT16, JXL_TYPE_FLOAT16, JXL_TYPE_FLOAT}) {
    const size_t sample_size = data_type == JXL_TYPE_FLOAT ? 4 : 2;
    const JxlPixelFormat little = {4, data_type, JXL_LITTLE_ENDIAN, 0};
    const JxlPixelFormat big = {4, data_type, JXL_BIG_ENDIAN, 0};
    std::vector<uint8_t> little_out;
    std::vector<uint8_t> big_out;
    ASSERT_NO_FATAL_FAILURE(DecodeDCOnly(compressed, little, nullptr,
                                         &little_out, &dc_xsize, &dc_ysize));
    ASSERT_NO_FATAL_FAILURE(
        DecodeDCOnly(compressed, big, nullptr, &big_out, &dc_xsize, &dc_ysize));
    ASSERT_EQ(little_out.size(), big_out.size());
    ASSERT_EQ(0u, little_out.size() % sample_size);
    std::vector<uint8_t> swapped(big_out.size());
    for (size_t i = 0; i < big_out.size(); i += sample_size) {
      std::reverse_copy(&big_out[i], &big_out[i] + sample_size, &swapped[i]);
    }
    EXPECT_TRUE(little_out == swapped) << "data type " << data_type;
    // Opaque alpha in the little-endian output: the last sample of the first
    // pixel.
    const uint8_t* alpha = &little_out[3 * sample_size];
    if (data_type == JXL_TYPE_UINT16) {
      EXPECT_EQ(0xFF, alpha[0]);
      EXPECT_EQ(0xFF, alpha[1]);
    } else if (data_type == JXL_TYPE_FLOAT16) {
      EXPECT_EQ(0x00, alpha[0]);  // 1.0 in binary16 is 0x3C00.
      EXPECT_EQ(0x3C, alpha[1]);
    } else {
      const uint8_t one[4] = {0x00, 0x00, 0x80, 0x3F};
      EXPECT_EQ(0, memcmp(one, alpha, 4));
    }
  }
}

// Two-channel output of a grayscale image is gray + alpha: the second sample
// is opaque alpha, not a second color sample.
TEST(DecodeTest, DCOnlyGrayAlphaOutput) {
  constexpr size_t xsize = 256;
  constexpr size_t ysize = 256;
  jxl::TestCodestreamParams params;
  params.cparams.speed_tier = jxl::SpeedTier::kLightning;
  std::vector<uint8_t> compressed =
      CreateDCOnlyTestCodestream(xsize, ysize, 1, params);
  size_t dc_xsize = 0;
  size_t dc_ysize = 0;

  std::vector<uint8_t> gray;
  std::vector<uint8_t> gray_alpha;
  ASSERT_NO_FATAL_FAILURE(
      DecodeDCOnly(compressed, {1, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0},
                   nullptr, &gray, &dc_xsize, &dc_ysize));
  ASSERT_NO_FATAL_FAILURE(
      DecodeDCOnly(compressed, {2, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0},
                   nullptr, &gray_alpha, &dc_xsize, &dc_ysize));
  const size_t num_pixels = dc_xsize * dc_ysize;
  ASSERT_EQ(num_pixels, gray.size());
  ASSERT_EQ(num_pixels * 2, gray_alpha.size());
  size_t gray_mismatches = 0;
  size_t alpha_mismatches = 0;
  for (size_t i = 0; i < num_pixels; ++i) {
    if (gray_alpha[i * 2] != gray[i]) ++gray_mismatches;
    if (gray_alpha[i * 2 + 1] != 255) ++alpha_mismatches;
  }
  EXPECT_EQ(0u, gray_mismatches) << "of " << num_pixels << " pixels";
  EXPECT_EQ(0u, alpha_mismatches) << "of " << num_pixels << " pixels";
}

struct CallbackImage {
  std::vector<uint8_t> pixels;
  size_t xsize;
  size_t num_channels;
};

void StoreCallbackPixels(void* opaque, size_t x, size_t y, size_t num_pixels,
                         const void* pixels) {
  CallbackImage* image = static_cast<CallbackImage*>(opaque);
  memcpy(&image->pixels[(y * image->xsize + x) * image->num_channels], pixels,
         num_pixels * image->num_channels);
}

// A frame rendered from its DC delivers the same pixels to an output callback
// as to an output buffer.
TEST(DecodeTest, DCOnlyCallbackMatchesBuffer) {
  constexpr size_t xsize = 300;
  constexpr size_t ysize = 200;
  jxl::TestCodestreamParams params;
  params.cparams.speed_tier = jxl::SpeedTier::kLightning;
  std::vector<uint8_t> compressed =
      CreateDCOnlyTestCodestream(xsize, ysize, 3, params);
  const JxlPixelFormat format = {3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};

  std::vector<uint8_t> buffer;
  size_t dc_xsize = 0;
  size_t dc_ysize = 0;
  ASSERT_NO_FATAL_FAILURE(
      DecodeDCOnly(compressed, format, nullptr, &buffer, &dc_xsize, &dc_ysize));

  CallbackImage image;
  image.xsize = dc_xsize;
  image.num_channels = 3;
  image.pixels.assign(buffer.size(), 0);
  JxlDecoderPtr dec = JxlDecoderMake(nullptr);
  ASSERT_NE(dec, nullptr);
  ASSERT_EQ(
      JXL_DEC_SUCCESS,
      JxlDecoderSubscribeEvents(dec.get(), JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE));
  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec.get(), compressed.data(),
                                                compressed.size()));
  JxlDecoderCloseInput(dec.get());
  ASSERT_EQ(JXL_DEC_FRAME, JxlDecoderProcessInput(dec.get()));
  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetImageOutDownsampling(dec.get(), 8));
  ASSERT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER, JxlDecoderProcessInput(dec.get()));
  ASSERT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSetImageOutCallback(dec.get(), &format,
                                          StoreCallbackPixels, &image));
  ASSERT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec.get()));
  JxlImageOutDownsamplingMethod method = JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_NONE;
  ASSERT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderGetImageOutDownsamplingMethod(dec.get(), &method));
  EXPECT_EQ(JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_DC_ONLY, method);
  EXPECT_TRUE(buffer == image.pixels);
}

// The AC data a DC-only frame leaves undecoded is skipped before the next
// frame header is read.
TEST(DecodeTest, DCOnlyFrameThenNextFrame) {
  JxlMemoryManager* memory_manager = jxl::test::MemoryManager();
  constexpr size_t xsize = 256;
  constexpr size_t ysize = 192;
  constexpr size_t num_frames = 2;
  const JxlPixelFormat format = {3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
  std::vector<uint8_t> frames[num_frames];
  frames[0] = jxl::test::GetSomeTestImage(xsize, ysize, 3, 0);
  frames[1] = jxl::test::GetSomeTestImage(xsize, ysize, 3, 1);
  const JxlPixelFormat input_format = {3, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};

  auto io = jxl::make_unique<jxl::CodecInOut>(memory_manager);
  ASSERT_TRUE(io->SetSize(xsize, ysize));
  io->metadata.m.SetUintSamples(16);
  io->metadata.m.color_encoding = jxl::ColorEncoding::SRGB(false);
  io->metadata.m.have_animation = true;
  io->frames.clear();
  for (size_t i = 0; i < num_frames; ++i) {
    jxl::ImageBundle bundle(memory_manager, &io->metadata.m);
    ASSERT_TRUE(ConvertFromExternal(
        jxl::Bytes(frames[i].data(), frames[i].size()), xsize, ysize,
        jxl::ColorEncoding::SRGB(/*is_gray=*/false),
        /*bits_per_sample=*/16, input_format,
        /*pool=*/nullptr, &bundle));
    bundle.duration = 5;
    io->frames.push_back(std::move(bundle));
  }
  jxl::CompressParams cparams;
  cparams.speed_tier = jxl::SpeedTier::kLightning;
  std::vector<uint8_t> compressed;
  ASSERT_TRUE(jxl::test::EncodeFile(cparams, io.get(), &compressed));

  // Full-resolution reference for each frame.
  std::vector<std::vector<uint8_t>> full(num_frames);
  {
    JxlDecoderPtr dec = JxlDecoderMake(nullptr);
    ASSERT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderSubscribeEvents(dec.get(), JXL_DEC_FULL_IMAGE));
    ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec.get(), compressed.data(),
                                                  compressed.size()));
    JxlDecoderCloseInput(dec.get());
    for (size_t i = 0; i < num_frames; ++i) {
      full[i].resize(xsize * ysize * 3);
      ASSERT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER,
                JxlDecoderProcessInput(dec.get()));
      ASSERT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderSetImageOutBuffer(dec.get(), &format, full[i].data(),
                                            full[i].size()));
      ASSERT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec.get()));
    }
    ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderProcessInput(dec.get()));
  }

  JxlDecoderPtr dec = JxlDecoderMake(nullptr);
  ASSERT_EQ(
      JXL_DEC_SUCCESS,
      JxlDecoderSubscribeEvents(dec.get(), JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE));
  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec.get(), compressed.data(),
                                                compressed.size()));
  JxlDecoderCloseInput(dec.get());
  for (size_t i = 0; i < num_frames; ++i) {
    ASSERT_EQ(JXL_DEC_FRAME, JxlDecoderProcessInput(dec.get()));
    ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetImageOutDownsampling(dec.get(), 8));
    std::vector<uint8_t> dc(jxl::DivCeil(xsize, 8) * jxl::DivCeil(ysize, 8) *
                            3);
    ASSERT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER, JxlDecoderProcessInput(dec.get()));
    ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetImageOutBuffer(
                                   dec.get(), &format, dc.data(), dc.size()));
    ASSERT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec.get()));
    JxlImageOutDownsamplingMethod method =
        JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_NONE;
    ASSERT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderGetImageOutDownsamplingMethod(dec.get(), &method));
    EXPECT_EQ(JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_DC_ONLY, method)
        << "frame " << i;
    EXPECT_LT(MeanAbsDiffToBoxAverage(full[i], xsize, ysize, 3, dc, 8), 0.02)
        << "frame " << i;
  }
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderProcessInput(dec.get()));
}

// The AC data a DC-only frame leaves undecoded is skipped before the boxes
// that follow the codestream are read, across codestream box boundaries.
TEST(DecodeTest, DCOnlyFrameThenBoxes) {
  constexpr size_t xsize = 256;
  constexpr size_t ysize = 256;
  jxl::TestCodestreamParams params;
  params.cparams.speed_tier = jxl::SpeedTier::kLightning;
  params.box_format = kCSBF_Multi_Other_Terminated;
  std::vector<uint8_t> compressed =
      CreateDCOnlyTestCodestream(xsize, ysize, 3, params);
  const JxlPixelFormat format = {3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};

  JxlDecoderPtr dec = JxlDecoderMake(nullptr);
  ASSERT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSubscribeEvents(
                dec.get(), JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE | JXL_DEC_BOX));
  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec.get(), compressed.data(),
                                                compressed.size()));
  JxlDecoderCloseInput(dec.get());
  std::vector<uint8_t> dc(jxl::DivCeil(xsize, 8) * jxl::DivCeil(ysize, 8) * 3);
  std::vector<uint8_t> box_contents(64);
  std::string box_type;
  std::vector<std::string> boxes_after_image;
  std::string unk3_contents;
  bool got_image = false;
  for (;;) {
    JxlDecoderStatus status = JxlDecoderProcessInput(dec.get());
    if (box_type == "unk3") {
      const size_t remaining = JxlDecoderReleaseBoxBuffer(dec.get());
      unk3_contents.assign(reinterpret_cast<const char*>(box_contents.data()),
                           box_contents.size() - remaining);
    }
    box_type.clear();
    if (status == JXL_DEC_SUCCESS) break;
    ASSERT_NE(JXL_DEC_ERROR, status);
    ASSERT_NE(JXL_DEC_NEED_MORE_INPUT, status);
    if (status == JXL_DEC_FRAME) {
      ASSERT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderSetImageOutDownsampling(dec.get(), 8));
    } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
      ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetImageOutBuffer(
                                     dec.get(), &format, dc.data(), dc.size()));
    } else if (status == JXL_DEC_FULL_IMAGE) {
      JxlImageOutDownsamplingMethod method =
          JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_NONE;
      ASSERT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderGetImageOutDownsamplingMethod(dec.get(), &method));
      EXPECT_EQ(JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_DC_ONLY, method);
      got_image = true;
    } else if (status == JXL_DEC_BOX) {
      JxlBoxType type;
      ASSERT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderGetBoxType(dec.get(), type, JXL_FALSE));
      box_type.assign(type, 4);
      if (got_image) boxes_after_image.push_back(box_type);
      if (box_type == "unk3") {
        ASSERT_EQ(JXL_DEC_SUCCESS,
                  JxlDecoderSetBoxBuffer(dec.get(), box_contents.data(),
                                         box_contents.size()));
      }
    } else {
      FAIL() << "unexpected status " << status;
    }
  }
  EXPECT_TRUE(got_image);
  ASSERT_FALSE(boxes_after_image.empty());
  EXPECT_EQ("unk3", boxes_after_image.back());
  EXPECT_EQ(std::string(unk3_box_contents, unk3_box_size), unk3_contents);
}

// Lossless JPEG recompression of `jpeg_path`: a VarDCT YCbCr frame with the
// JPEG's chroma subsampling, and LF smoothing as `force_lfs` sets it (see
// CompressParams::force_lfs_jpeg_recompression).
std::vector<uint8_t> CreateJPEGRecompressionCodestream(
    const std::string& jpeg_path, size_t* xsize, size_t* ysize,
    int force_lfs = -1) {
  JxlMemoryManager* memory_manager = jxl::test::MemoryManager();
  const std::vector<uint8_t> orig = jxl::test::ReadTestData(jpeg_path);
  auto io = jxl::make_unique<jxl::CodecInOut>(memory_manager);
  JXL_TEST_ASSIGN_OR_DIE(std::unique_ptr<jxl::jpeg::JPEGData> jpeg_data,
                         jxl::jpeg::ParseJPG(memory_manager, jxl::Bytes(orig)));
  EXPECT_TRUE(jxl::test::JpegDataToCodecInOut(std::move(jpeg_data), io.get()));
  io->metadata.m.xyb_encoded = false;
  *xsize = io->xsize();
  *ysize = io->ysize();
  jxl::BitWriter writer{memory_manager};
  EXPECT_TRUE(WriteCodestreamHeaders(&io->metadata, &writer, nullptr));
  writer.ZeroPadToByte();
  jxl::CompressParams cparams;
  cparams.color_transform = jxl::ColorTransform::kNone;
  cparams.force_lfs_jpeg_recompression = force_lfs;
  EXPECT_TRUE(jxl::EncodeFrame(memory_manager, cparams, jxl::FrameInfo{},
                               &io->metadata, io->Main(), *JxlGetDefaultCms(),
                               /*pool=*/nullptr, &writer, /*aux_out=*/nullptr));
  jxl::PaddedBytes codestream = std::move(writer).TakeBytes();
  return std::vector<uint8_t>(codestream.data(),
                              codestream.data() + codestream.size());
}

// Recompressed JPEGs, most of them chroma subsampled, are previewed from their
// DC at 1/8: the subsampled chroma DC must be upsampled, not read as if it had
// the luma resolution.
JXL_TRANSCODE_JPEG_TEST(DecodeTest,
                        PreviewDownsamplingJPEGRecompressionDcOnly) {
  // Measured: 0.0126 (4:2:0), 0.0082 (4:2:2), 0.0086 (4:4:0), 5e-8 (4:4:4,
  // with and without LF smoothing). Reading the chroma DC at luma coordinates
  // gave about 0.08 for the subsampled ones.
  constexpr double kMaxMAE = 0.025;
  struct Case {
    const char* jpeg_path;
    int force_lfs;
  };
  // LF smoothing (the DC-only preview renders the smoothed DC) is only
  // defined for 4:4:4.
  for (const Case& test_case :
       {Case{"jxl/flower/flower.png.im_q85_420.jpg", 0},
        Case{"jxl/flower/flower.png.im_q85_422.jpg", 0},
        Case{"jxl/flower/flower.png.im_q85_440.jpg", 0},
        Case{"jxl/flower/flower.png.im_q85_444.jpg", 0},
        Case{"jxl/flower/flower.png.im_q85_444.jpg", 1}}) {
    SCOPED_TRACE(testing::Message() << test_case.jpeg_path << " LF smoothing "
                                    << test_case.force_lfs);
    size_t xsize = 0;
    size_t ysize = 0;
    std::vector<uint8_t> compressed = CreateJPEGRecompressionCodestream(
        test_case.jpeg_path, &xsize, &ysize, test_case.force_lfs);
    VerifyPreviewDownsamplingOfCodestream(
        compressed, xsize, ysize, /*factor=*/8,
        jxl::extras::JXLPreviewBackend::kNativeDcOnly, kMaxMAE);
  }
}

// The DC image holds no patches, so a frame with patches is not rendered by the
// DC-only path, which would leave its text out of the preview. At 1/8 it is
// flushed at its DC progression step instead, through the full pipeline, which
// draws the patches.
TEST(DecodeTest, PreviewDownsamplingPatchesFactor8) {
  const std::vector<uint8_t> orig =
      jxl::test::ReadTestData("jxl/grayscale_patches.png");
  jxl::extras::PackedPixelFile ppf;
  ASSERT_TRUE(jxl::extras::DecodeBytes(jxl::Bytes(orig),
                                       jxl::extras::ColorHints(), &ppf));
  jxl::extras::JXLCompressParams cparams;
  cparams.AddOption(JXL_ENC_FRAME_SETTING_PATCHES, 1);
  std::vector<uint8_t> compressed;
  ASSERT_TRUE(jxl::extras::EncodeImageJXL(cparams, ppf, nullptr, &compressed));
  // As in PatchDictionaryTest.GrayscaleVarDCT: about 47k without patches.
  ASSERT_LE(compressed.size(), 14000u) << "patches were not used";
  // Measured: 0.0006. Rendering from the DC alone gave about 0.03.
  VerifyPreviewDownsamplingOfCodestream(
      compressed, ppf.xsize(), ppf.ysize(), /*factor=*/8,
      jxl::extras::JXLPreviewBackend::kNativeProgressionFlush,
      /*max_mae=*/0.01);
}

// The DC image holds no splines either. The frame has no other progression
// step, so it is decoded in full and box-downsampled; without EPF and gaborish,
// which previews skip at 1/4 and below, that matches the full decode.
TEST(DecodeTest, PreviewDownsamplingSplinesNotFromDC) {
  constexpr size_t xsize = 256;
  constexpr size_t ysize = 256;
  const jxl::ColorCorrelation color_correlation{};
  const jxl::Spline spline{
      {{20, 30}, {120, 200}, {230, 60}},
      /*color_dct=*/
      {jxl::Dct32{0.f}, jxl::Dct32{0.5f}, jxl::Dct32{0.5f}},
      /*sigma_dct=*/{4.f}};
  std::vector<jxl::QuantizedSpline> quantized_splines;
  JXL_TEST_ASSIGN_OR_DIE(
      jxl::QuantizedSpline quantized_spline,
      jxl::QuantizedSpline::Create(spline, /*quantization_adjustment=*/0,
                                   color_correlation.YtoXRatio(0),
                                   color_correlation.YtoBRatio(0)));
  quantized_splines.emplace_back(std::move(quantized_spline));
  const std::vector<jxl::Spline::Point> starting_points = {
      spline.control_points.front()};
  jxl::TestCodestreamParams params;
  params.cparams.speed_tier = jxl::SpeedTier::kLightning;
  // A view: the vectors outlive the encode below.
  params.cparams.custom_splines = {
      jxl::Span<const jxl::QuantizedSpline>(quantized_splines),
      jxl::Span<const jxl::Spline::Point>(starting_points)};
  params.cparams.epf = 0;
  params.cparams.gaborish = jxl::Override::kOff;
  std::vector<uint8_t> compressed =
      CreateDCOnlyTestCodestream(xsize, ysize, 3, params);
  VerifyPreviewDownsamplingOfCodestream(
      compressed, xsize, ysize, /*factor=*/8,
      jxl::extras::JXLPreviewBackend::kFallbackDownsample,
      /*max_mae=*/0.1);
}

// Previews do not synthesize noise. Their pipelines have no noise channels,
// which the AC groups must not fill either.
TEST(DecodeTest, PreviewDownsamplingWithNoise) {
  jxl::TestCodestreamParams params;
  params.cparams.progressive_dc = 0;
  params.cparams.responsive = 0;
  params.cparams.speed_tier = jxl::SpeedTier::kThunder;
  params.cparams.photon_noise_iso = 3200;
  for (size_t factor : {2u, 4u, 8u}) {
    SCOPED_TRACE(factor);
    // Measured: at most 0.005.
    VerifyPreviewDownsamplingRoundtrip(
        params, /*xsize=*/320, /*ysize=*/240, /*num_channels=*/3, factor,
        factor == 8 ? jxl::extras::JXLPreviewBackend::kNativeDcOnly
                    : jxl::extras::JXLPreviewBackend::kNativeReducedInput,
        /*max_mae=*/0.01);
  }
}

// The output orientation is applied to frames rendered from their DC.
TEST(DecodeTest, PreviewDownsamplingDcOnlyOrientation) {
  constexpr size_t xsize = 320;
  constexpr size_t ysize = 200;
  for (JxlOrientation orientation :
       {JXL_ORIENT_FLIP_HORIZONTAL, JXL_ORIENT_ROTATE_90_CW,
        JXL_ORIENT_TRANSPOSE}) {
    SCOPED_TRACE(orientation);
    jxl::TestCodestreamParams params;
    params.cparams.speed_tier = jxl::SpeedTier::kLightning;
    params.orientation = orientation;
    std::vector<uint8_t> compressed =
        CreateDCOnlyTestCodestream(xsize, ysize, 3, params);
    const bool swap = orientation >= JXL_ORIENT_TRANSPOSE;
    // Measured: 0.0008.
    VerifyPreviewDownsamplingOfCodestream(
        compressed, swap ? ysize : xsize, swap ? xsize : ysize, /*factor=*/8,
        jxl::extras::JXLPreviewBackend::kNativeDcOnly, /*max_mae=*/0.002);
  }
}

// Half float previews, rendered from the DC and from reduced-resolution AC,
// match float previews to half float precision.
TEST(DecodeTest, PreviewDownsamplingFloat16) {
  constexpr size_t xsize = 320;
  constexpr size_t ysize = 240;
  jxl::TestCodestreamParams params;
  params.cparams.speed_tier = jxl::SpeedTier::kLightning;
  std::vector<uint8_t> compressed =
      CreateDCOnlyTestCodestream(xsize, ysize, 3, params);
  for (size_t factor : {2u, 8u}) {
    SCOPED_TRACE(factor);
    jxl::extras::PackedPixelFile previews[2];
    const JxlDataType data_types[2] = {JXL_TYPE_FLOAT, JXL_TYPE_FLOAT16};
    for (size_t i = 0; i < 2; ++i) {
      jxl::extras::JXLDecompressParams dparams;
      dparams.accepted_formats = {{3, data_types[i], JXL_LITTLE_ENDIAN, 0}};
      dparams.preview_downsampling = factor;
      ASSERT_TRUE(jxl::extras::DecodeImageJXL(
          compressed.data(), compressed.size(), dparams,
          /*decoded_bytes=*/nullptr, &previews[i]));
      ASSERT_EQ(1u, previews[i].frames.size());
      ASSERT_EQ(data_types[i], previews[i].frames[0].color.format.data_type);
    }
    const jxl::extras::PackedImage& f32 = previews[0].frames[0].color;
    const jxl::extras::PackedImage& f16 = previews[1].frames[0].color;
    ASSERT_EQ(jxl::DivCeil(xsize, factor), f16.xsize);
    ASSERT_EQ(jxl::DivCeil(ysize, factor), f16.ysize);
    for (size_t y = 0; y < f16.ysize; ++y) {
      const uint8_t* row =
          static_cast<const uint8_t*>(f16.pixels()) + y * f16.stride;
      for (size_t x = 0; x < f16.xsize; ++x) {
        for (size_t c = 0; c < 3; ++c) {
          const float expected = f32.GetPixelValue(y, x, c);
          // Half floats have an 11-bit significand.
          EXPECT_NEAR(expected, jxl::test::LoadLEFloat16(&row[(x * 3 + c) * 2]),
                      std::abs(expected) / 1024.0f + 1e-6f);
        }
      }
    }
  }
}

// Box-downsampling in the output writer, which the native paths can be
// excluded down to. On ARM, the fast XYB to 8-bit stage used to write the full
// resolution output here, cropped to the preview size.
TEST(DecodeTest, PreviewDownsamplingRegularPathXYBUint8) {
  constexpr size_t xsize = 320;
  constexpr size_t ysize = 240;
  jxl::TestCodestreamParams params;
  params.cparams.speed_tier = jxl::SpeedTier::kLightning;
  // Previews skip EPF and gaborish at 1/4 and below.
  params.cparams.epf = 0;
  params.cparams.gaborish = jxl::Override::kOff;
  std::vector<uint8_t> compressed =
      CreateDCOnlyTestCodestream(xsize, ysize, 3, params);
  for (size_t factor : {2u, 8u}) {
    SCOPED_TRACE(factor);
    // The writer averages before it clamps and quantizes to 8 bits, so the
    // reference is the box average of a float decode, clamped.
    jxl::extras::JXLDecompressParams dparams;
    dparams.accepted_formats = {{3, JXL_TYPE_FLOAT, JXL_NATIVE_ENDIAN, 0}};
    jxl::extras::PackedPixelFile full;
    ASSERT_TRUE(jxl::extras::DecodeImageJXL(compressed.data(),
                                            compressed.size(), dparams,
                                            /*decoded_bytes=*/nullptr, &full));
    dparams.accepted_formats = {{3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0}};
    dparams.preview_downsampling = factor;
    dparams.preview_hooks = jxl::GetDecoderPreviewHooks();
    dparams.preview_allowed_backends =
        PreviewBackendBit(jxl::extras::JXLPreviewBackend::kFallbackDownsample);
    jxl::extras::JXLPreviewBackend backend =
        jxl::extras::JXLPreviewBackend::kNone;
    dparams.preview_backend = &backend;
    jxl::extras::PackedPixelFile preview;
    ASSERT_TRUE(jxl::extras::DecodeImageJXL(
        compressed.data(), compressed.size(), dparams,
        /*decoded_bytes=*/nullptr, &preview));
    EXPECT_EQ(jxl::extras::JXLPreviewBackend::kFallbackDownsample, backend);
    const jxl::extras::PackedImage& small = preview.frames[0].color;
    ASSERT_EQ(jxl::DivCeil(xsize, factor), small.xsize);
    ASSERT_EQ(jxl::DivCeil(ysize, factor), small.ysize);
    // Rounding and dithering to 8 bits.
    constexpr float kTolerance = 1.0f / 255.0f + 1e-6f;
    for (size_t y = 0; y < small.ysize; ++y) {
      for (size_t x = 0; x < small.xsize; ++x) {
        for (size_t c = 0; c < 3; ++c) {
          const float expected = jxl::Clamp1(
              BoxAveragePixel(full.frames[0].color, factor, x, y, c), 0.0f,
              1.0f);
          EXPECT_NEAR(expected, small.GetPixelValue(y, x, c), kTolerance);
        }
      }
    }
  }
}

// The pixel of an `xsize` x `ysize` frame that is displayed at (x, y) under
// `orientation` (the Exif definitions).
std::pair<size_t, size_t> FramePixelDisplayedAt(uint32_t orientation,
                                                size_t xsize, size_t ysize,
                                                size_t x, size_t y) {
  switch (orientation) {
    case JXL_ORIENT_FLIP_HORIZONTAL:
      return {xsize - 1 - x, y};
    case JXL_ORIENT_ROTATE_180:
      return {xsize - 1 - x, ysize - 1 - y};
    case JXL_ORIENT_FLIP_VERTICAL:
      return {x, ysize - 1 - y};
    case JXL_ORIENT_TRANSPOSE:
      return {y, x};
    case JXL_ORIENT_ROTATE_90_CW:
      return {y, ysize - 1 - x};
    case JXL_ORIENT_ANTI_TRANSPOSE:
      return {xsize - 1 - y, ysize - 1 - x};
    case JXL_ORIENT_ROTATE_90_CCW:
      return {xsize - 1 - y, x};
    default:
      return {x, y};
  }
}

// The output writer's box-downsampling works in frame coordinates, before the
// orientation is undone; on a non-square image, orientations that transpose
// used to clip the frame to the displayed width and height. Its boxes start at
// the frame origin, as documented: with a size that is not a multiple of the
// factor, the partial boxes of a flipped image are at its displayed left or
// top.
TEST(DecodeTest, PreviewDownsamplingRegularPathOrientation) {
  constexpr size_t xsize = 301;
  constexpr size_t ysize = 203;
  for (uint32_t orientation = JXL_ORIENT_IDENTITY;
       orientation <= JXL_ORIENT_ROTATE_90_CCW; ++orientation) {
    SCOPED_TRACE(orientation);
    jxl::TestCodestreamParams params;
    params.cparams.speed_tier = jxl::SpeedTier::kLightning;
    // Previews skip EPF and gaborish at 1/4 and below.
    params.cparams.epf = 0;
    params.cparams.gaborish = jxl::Override::kOff;
    params.orientation = static_cast<JxlOrientation>(orientation);
    std::vector<uint8_t> compressed =
        CreateDCOnlyTestCodestream(xsize, ysize, 3, params);
    jxl::extras::JXLDecompressParams dparams;
    dparams.accepted_formats = {{3, JXL_TYPE_FLOAT, JXL_NATIVE_ENDIAN, 0}};
    // The full decode in frame coordinates, which the boxes are anchored in.
    dparams.keep_orientation = true;
    jxl::extras::PackedPixelFile full;
    ASSERT_TRUE(jxl::extras::DecodeImageJXL(compressed.data(),
                                            compressed.size(), dparams,
                                            /*decoded_bytes=*/nullptr, &full));
    const jxl::extras::PackedImage& big = full.frames[0].color;
    ASSERT_EQ(xsize, big.xsize);
    dparams.keep_orientation = false;
    const bool swap = orientation >= JXL_ORIENT_TRANSPOSE;
    for (size_t factor : {2u, 4u, 8u}) {
      SCOPED_TRACE(factor);
      dparams.preview_downsampling = factor;
      dparams.preview_hooks = jxl::GetDecoderPreviewHooks();
      dparams.preview_allowed_backends = PreviewBackendBit(
          jxl::extras::JXLPreviewBackend::kFallbackDownsample);
      jxl::extras::JXLPreviewBackend backend =
          jxl::extras::JXLPreviewBackend::kNone;
      dparams.preview_backend = &backend;
      jxl::extras::PackedPixelFile preview;
      ASSERT_TRUE(jxl::extras::DecodeImageJXL(
          compressed.data(), compressed.size(), dparams,
          /*decoded_bytes=*/nullptr, &preview));
      EXPECT_EQ(jxl::extras::JXLPreviewBackend::kFallbackDownsample, backend);
      const jxl::extras::PackedImage& small = preview.frames[0].color;
      const size_t frame_xsize = jxl::DivCeil(xsize, factor);
      const size_t frame_ysize = jxl::DivCeil(ysize, factor);
      ASSERT_EQ(swap ? frame_ysize : frame_xsize, small.xsize);
      ASSERT_EQ(swap ? frame_xsize : frame_ysize, small.ysize);
      double max_error = 0.0;
      for (size_t y = 0; y < small.ysize; ++y) {
        for (size_t x = 0; x < small.xsize; ++x) {
          const std::pair<size_t, size_t> box = FramePixelDisplayedAt(
              orientation, frame_xsize, frame_ysize, x, y);
          for (size_t c = 0; c < 3; ++c) {
            max_error = std::max<double>(
                max_error, std::abs(BoxAveragePixel(big, factor, box.first,
                                                    box.second, c) -
                                    small.GetPixelValue(y, x, c)));
          }
        }
      }
      EXPECT_LT(max_error, 1e-5);
    }
  }
}

// What DecodeWithWriterDownsampling received.
struct WriterOutput {
  std::vector<uint8_t> pixels;
  size_t xsize = 0;
  size_t bytes_per_pixel = 0;
  // The pixels per run call the decoder declared to the init callback, and the
  // most it passed to one run call, per thread.
  size_t init_num_pixels = 0;
  std::vector<size_t> max_run_pixels;
};

void* WriterOutputInit(void* opaque, size_t num_threads,
                       size_t num_pixels_per_thread) {
  WriterOutput* out = static_cast<WriterOutput*>(opaque);
  out->init_num_pixels = num_pixels_per_thread;
  out->max_run_pixels.assign(num_threads, 0);
  return out;
}

void WriterOutputRun(void* opaque, size_t thread_id, size_t x, size_t y,
                     size_t num_pixels, const void* pixels) {
  WriterOutput* out = static_cast<WriterOutput*>(opaque);
  ASSERT_LT(thread_id, out->max_run_pixels.size());
  ASSERT_LE(x + num_pixels, out->xsize);
  out->max_run_pixels[thread_id] =
      std::max(out->max_run_pixels[thread_id], num_pixels);
  memcpy(&out->pixels[(y * out->xsize + x) * out->bytes_per_pixel], pixels,
         num_pixels * out->bytes_per_pixel);
}

void WriterOutputDestroy(void* opaque) {}

// Decodes the displayed frame of `compressed`, box-downsampled by `factor` in
// the output writer (the native preview paths are disabled), unpremultiplying
// alpha, with `num_threads` worker threads (0: no parallel runner), to an
// output buffer or, if `use_callback`, to a multithreaded output callback.
void DecodeWithWriterDownsampling(const std::vector<uint8_t>& compressed,
                                  size_t factor, const JxlPixelFormat& format,
                                  size_t num_threads, bool use_callback,
                                  WriterOutput* out) {
  JxlDecoderPtr dec = JxlDecoderMake(nullptr);
  ASSERT_NE(dec, nullptr);
  JxlThreadParallelRunnerPtr runner;
  if (num_threads != 0) {
    runner = JxlThreadParallelRunnerMake(nullptr, num_threads);
    ASSERT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderSetParallelRunner(dec.get(), JxlThreadParallelRunner,
                                          runner.get()));
  }
  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSubscribeEvents(
                                 dec.get(), JXL_DEC_BASIC_INFO | JXL_DEC_FRAME |
                                                JXL_DEC_FULL_IMAGE));
  ASSERT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSetUnpremultiplyAlpha(dec.get(), JXL_TRUE));
  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec.get(), compressed.data(),
                                                compressed.size()));
  JxlDecoderCloseInput(dec.get());
  ASSERT_EQ(JXL_DEC_BASIC_INFO, JxlDecoderProcessInput(dec.get()));
  JxlBasicInfo info;
  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBasicInfo(dec.get(), &info));
  ASSERT_EQ(JXL_DEC_FRAME, JxlDecoderProcessInput(dec.get()));
  ASSERT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSetImageOutDownsampling(dec.get(), factor));
  ASSERT_EQ(JXL_DEC_SUCCESS, jxl::GetDecoderPreviewHooks()->set_native_paths(
                                 dec.get(), JXL_FALSE, JXL_FALSE, JXL_FALSE));
  out->xsize = jxl::DivCeil(info.xsize, factor);
  out->bytes_per_pixel = format.num_channels *
                         jxl::test::GetDataBits(format.data_type) /
                         jxl::kBitsPerByte;
  size_t buffer_size = 0;
  ASSERT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderImageOutBufferSize(dec.get(), &format, &buffer_size));
  ASSERT_EQ(
      out->xsize * jxl::DivCeil(info.ysize, factor) * out->bytes_per_pixel,
      buffer_size);
  out->pixels.assign(buffer_size, 0);
  ASSERT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER, JxlDecoderProcessInput(dec.get()));
  if (use_callback) {
    ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetMultithreadedImageOutCallback(
                                   dec.get(), &format, WriterOutputInit,
                                   WriterOutputRun, WriterOutputDestroy, out));
  } else {
    ASSERT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderSetImageOutBuffer(
                  dec.get(), &format, out->pixels.data(), out->pixels.size()));
  }
  ASSERT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec.get()));
  if (factor > 1) {
    JxlImageOutDownsamplingMethod method =
        JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_NONE;
    ASSERT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderGetImageOutDownsamplingMethod(dec.get(), &method));
    EXPECT_EQ(JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_FULL_RESOLUTION, method);
  }
}

float FloatSample(const WriterOutput& image, size_t num_channels, size_t x,
                  size_t y, size_t c) {
  float value;
  memcpy(
      &value,
      &image.pixels[((y * image.xsize + x) * num_channels + c) * sizeof(float)],
      sizeof(value));
  return value;
}

// Decodes `compressed` (float samples, `num_channels` per pixel) in full and
// with the output writer's box downsampling at factors 2, 4 and 8. Each
// preview must equal the box average of the full decode, whether decoded
// without threads to a buffer or with threads to a callback, and must be
// delivered to the callback in runs of at most the size the decoder declared.
void VerifyWriterDownsampling(const std::vector<uint8_t>& compressed,
                              size_t num_channels) {
  const JxlPixelFormat format = {static_cast<uint32_t>(num_channels),
                                 JXL_TYPE_FLOAT, JXL_NATIVE_ENDIAN, 0};
  WriterOutput full;
  ASSERT_NO_FATAL_FAILURE(DecodeWithWriterDownsampling(
      compressed, /*factor=*/1, format, /*num_threads=*/0,
      /*use_callback=*/false, &full));
  const size_t xsize = full.xsize;
  const size_t ysize = full.pixels.size() / (xsize * full.bytes_per_pixel);
  for (size_t factor : {2u, 4u, 8u}) {
    SCOPED_TRACE(factor);
    WriterOutput single;
    ASSERT_NO_FATAL_FAILURE(DecodeWithWriterDownsampling(
        compressed, factor, format, /*num_threads=*/0, /*use_callback=*/false,
        &single));
    WriterOutput threaded;
    ASSERT_NO_FATAL_FAILURE(DecodeWithWriterDownsampling(
        compressed, factor, format, /*num_threads=*/4, /*use_callback=*/true,
        &threaded));
    // Each box is summed whole by one thread, in the same order.
    EXPECT_TRUE(single.pixels == threaded.pixels);
    ASSERT_GT(threaded.init_num_pixels, 0u);
    for (size_t run_pixels : threaded.max_run_pixels) {
      EXPECT_LE(run_pixels, threaded.init_num_pixels);
    }
    const size_t small_ysize = jxl::DivCeil(ysize, factor);
    // The writer sums in float: the error scales with the magnitude of the
    // samples, which unpremultiplying makes large where alpha is small.
    double max_relative_error = 0.0;
    for (size_t y = 0; y < small_ysize; ++y) {
      for (size_t x = 0; x < single.xsize; ++x) {
        for (size_t c = 0; c < num_channels; ++c) {
          double sum = 0.0;
          double sum_abs = 0.0;
          size_t count = 0;
          for (size_t fy = y * factor; fy < std::min(ysize, (y + 1) * factor);
               ++fy) {
            for (size_t fx = x * factor; fx < std::min(xsize, (x + 1) * factor);
                 ++fx) {
              const float sample = FloatSample(full, num_channels, fx, fy, c);
              sum += sample;
              sum_abs += std::abs(sample);
              ++count;
            }
          }
          const double error = std::abs(
              sum / count - FloatSample(single, num_channels, x, y, c));
          max_relative_error = std::max(max_relative_error,
                                        error / std::max(1.0, sum_abs / count));
        }
      }
    }
    EXPECT_LT(max_relative_error, 1e-5);
  }
}

// In a 4:2:0 frame the output writer is fed rects whose edges lay on odd rows
// around group boundaries: boxes there were split between rects, averaged in
// parts (seams), and written by two threads. Output rows of more than 1024
// pixels (factor 2) were also passed to callbacks in one run.
JXL_TRANSCODE_JPEG_TEST(DecodeTest,
                        PreviewDownsamplingRegularPathChromaSubsampled) {
  size_t xsize = 0;
  size_t ysize = 0;
  const std::vector<uint8_t> compressed = CreateJPEGRecompressionCodestream(
      "jxl/flower/flower.png.im_q85_420.jpg", &xsize, &ysize);
  ASSERT_GT(xsize, 2 * 1024u);
  VerifyWriterDownsampling(compressed, /*num_channels=*/3);
}

// A layered image with an alpha-blended crop at `crop_x0`, `crop_y0`, of
// `crop_xsize` x `crop_ysize` pixels, over a background.
std::vector<uint8_t> CreateBlendedCropCodestream(size_t xsize, size_t ysize,
                                                 int crop_x0, int crop_y0,
                                                 size_t crop_xsize,
                                                 size_t crop_ysize,
                                                 uint32_t orientation = 1) {
  JxlMemoryManager* memory_manager = jxl::test::MemoryManager();
  auto io = jxl::make_unique<jxl::CodecInOut>(memory_manager);
  EXPECT_TRUE(io->SetSize(xsize, ysize));
  io->metadata.m.SetUintSamples(16);
  io->metadata.m.SetAlphaBits(16, /*alpha_is_premultiplied=*/true);
  io->metadata.m.color_encoding = jxl::ColorEncoding::SRGB(false);
  io->metadata.m.orientation = orientation;
  io->frames.clear();
  const JxlPixelFormat format = {4, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};
  for (size_t i = 0; i < 2; ++i) {
    const size_t frame_xsize = i == 0 ? xsize : crop_xsize;
    const size_t frame_ysize = i == 0 ? ysize : crop_ysize;
    std::vector<uint8_t> pixels =
        jxl::test::GetSomeTestImage(frame_xsize, frame_ysize, 4, i);
    jxl::ImageBundle bundle(memory_manager, &io->metadata.m);
    EXPECT_TRUE(ConvertFromExternal(
        jxl::Bytes(pixels.data(), pixels.size()), frame_xsize, frame_ysize,
        jxl::ColorEncoding::SRGB(false), /*bits_per_sample=*/16, format,
        /*pool=*/nullptr, &bundle, /*set_alpha=*/true));
    if (i == 0) {
      bundle.use_for_next_frame = true;
    } else {
      bundle.origin = {crop_x0, crop_y0};
      bundle.blend = true;
      bundle.blendmode = jxl::BlendMode::kBlend;
    }
    io->frames.push_back(std::move(bundle));
  }
  jxl::CompressParams cparams;
  cparams.speed_tier = jxl::SpeedTier::kLightning;
  std::vector<uint8_t> compressed;
  EXPECT_TRUE(jxl::test::EncodeFile(cparams, io.get(), &compressed));
  return compressed;
}

// After blending, the output writer works in image coordinates, where the
// frame's own edges separate its rects from the out-of-frame ones. A crop at
// an origin that is not a multiple of the factor is rendered in full and then
// written downsampled; an aligned one is downsampled as it is rendered.
TEST(DecodeTest, PreviewDownsamplingRegularPathBlendedCrop) {
  constexpr size_t xsize = 301;
  constexpr size_t ysize = 203;
  struct Crop {
    int x0, y0;
    size_t xsize, ysize;
  };
  for (const Crop& crop : {Crop{37, 21, 150, 100}, Crop{-5, 150, 150, 100},
                           Crop{40, 24, 152, 96}}) {
    SCOPED_TRACE(::testing::Message() << crop.x0 << "," << crop.y0);
    const std::vector<uint8_t> compressed = CreateBlendedCropCodestream(
        xsize, ysize, crop.x0, crop.y0, crop.xsize, crop.ysize);
    VerifyWriterDownsampling(compressed, /*num_channels=*/4);
  }
}

// A 1024-pixel modular group with 8x frame upsampling renders this frame as a
// single rect 2400 pixels wide: the output writer used to pass such a row to
// callbacks in one run at factor 2, more pixels than it declared.
TEST(DecodeTest, PreviewDownsamplingRegularPathWideRects) {
  jxl::TestCodestreamParams params;
  params.cparams.speed_tier = jxl::SpeedTier::kLightning;
  params.cparams.modular_mode = true;
  params.cparams.modular_group_size_shift = 3;
  params.cparams.resampling = 8;
  params.cparams.ec_resampling = 8;
  // Previews skip EPF and gaborish at 1/4 and below.
  params.cparams.epf = 0;
  params.cparams.gaborish = jxl::Override::kOff;
  const std::vector<uint8_t> compressed = CreateDCOnlyTestCodestream(
      /*xsize=*/2400, /*ysize=*/40, /*num_channels=*/4, params);
  VerifyWriterDownsampling(compressed, /*num_channels=*/4);
}

// A preview keeps the extra channel images: consumers of a PackedPixelFile,
// such as the PNM encoders, expect one per extra channel in every frame.
TEST(DecodeTest, PreviewDownsamplingExtraChannelImages) {
  constexpr size_t xsize = 301;
  constexpr size_t ysize = 203;
  jxl::TestCodestreamParams params;
  params.cparams.speed_tier = jxl::SpeedTier::kLightning;
  std::vector<uint8_t> compressed =
      CreateDCOnlyTestCodestream(xsize, ysize, /*num_channels=*/4, params);
  jxl::extras::JXLDecompressParams dparams;
  // Without alpha in the color format, alpha is decoded as an extra channel.
  // The PFM encoder accepts explicit endianness only.
  dparams.accepted_formats = {{3, JXL_TYPE_FLOAT, JXL_LITTLE_ENDIAN, 0}};
  jxl::extras::PackedPixelFile full;
  ASSERT_TRUE(jxl::extras::DecodeImageJXL(compressed.data(), compressed.size(),
                                          dparams, /*decoded_bytes=*/nullptr,
                                          &full));
  ASSERT_EQ(1u, full.extra_channels_info.size());
  const jxl::extras::PackedImage& full_alpha = full.frames[0].extra_channels[0];
  for (size_t factor : {2u, 8u}) {
    SCOPED_TRACE(factor);
    dparams.preview_downsampling = factor;
    jxl::extras::PackedPixelFile preview;
    ASSERT_TRUE(jxl::extras::DecodeImageJXL(
        compressed.data(), compressed.size(), dparams,
        /*decoded_bytes=*/nullptr, &preview));
    ASSERT_EQ(1u, preview.frames.size());
    ASSERT_EQ(preview.extra_channels_info.size(),
              preview.frames[0].extra_channels.size());
    const jxl::extras::PackedImage& alpha = preview.frames[0].extra_channels[0];
    ASSERT_EQ(jxl::DivCeil(xsize, factor), alpha.xsize);
    ASSERT_EQ(jxl::DivCeil(ysize, factor), alpha.ysize);
    // The alpha channel is averaged as integer samples.
    double max_error = 0.0;
    for (size_t y = 0; y < alpha.ysize; ++y) {
      for (size_t x = 0; x < alpha.xsize; ++x) {
        max_error = std::max<double>(
            max_error, std::abs(BoxAveragePixel(full_alpha, factor, x, y, 0) -
                                alpha.GetPixelValue(y, x, 0)));
      }
    }
    EXPECT_LT(max_error, 1.0 / 255 + 1e-6);
    std::unique_ptr<jxl::extras::Encoder> encoder =
        jxl::extras::Encoder::FromExtension(".pfm");
    ASSERT_NE(nullptr, encoder);
    jxl::extras::EncodedImage encoded;
    ASSERT_TRUE(encoder->Encode(preview, &encoded, /*pool=*/nullptr));
    EXPECT_EQ(1u, encoded.extra_channel_bitstreams.size());
  }
}

// The one-shot preview API's NULL color_encoding means sRGB, grey sRGB for
// grey sources; with tone mapping off, the decoder's default output encoding.
TEST(DecodeTest, PreviewApiDefaultColorEncoding) {
  struct Source {
    const char* color_space;
    uint32_t num_channels;
    float intensity_target;
    JxlTransferFunction transfer_function;
  };
  for (const Source& source :
       {Source{"RGB_D65_202_Rel_PeQ", 3, 4000.0f, JXL_TRANSFER_FUNCTION_PQ},
        Source{"Gra_D65_Rel_Lin", 1, 0.0f, JXL_TRANSFER_FUNCTION_LINEAR}}) {
    SCOPED_TRACE(source.color_space);
    jxl::TestCodestreamParams params;
    params.cparams.speed_tier = jxl::SpeedTier::kLightning;
    params.color_space = source.color_space;
    params.intensity_target = source.intensity_target;
    const std::vector<uint8_t> compressed = CreateDCOnlyTestCodestream(
        /*xsize=*/64, /*ysize=*/48, source.num_channels, params);
    struct Preview {
      JxlPixelFormat format = {};
      JxlColorEncoding color_encoding = {};
      std::vector<uint8_t> pixels;
    };
    auto generate = [&](const JxlColorEncoding* color_encoding,
                        float display_nits, Preview* preview) {
      JxlPreviewOptions options;
      JxlPreviewOptionsInit(&options);
      uint32_t xsize = 0;
      uint32_t ysize = 0;
      uint8_t* pixels = nullptr;
      size_t pixels_size = 0;
      options.preview_downsampling = 2;
      options.color_encoding = color_encoding;
      options.display_nits = display_nits;
      options.out_xsize = &xsize;
      options.out_ysize = &ysize;
      options.out_pixels = &pixels;
      options.out_pixels_size = &pixels_size;
      options.out_format = &preview->format;
      options.out_color_encoding = &preview->color_encoding;
      ASSERT_EQ(
          JXL_PREVIEW_SUCCESS,
          JxlGeneratePreview(compressed.data(), compressed.size(), &options));
      preview->pixels.assign(pixels, pixels + pixels_size);
      free(pixels);
    };
    const JxlColorEncoding srgb =
        jxl::ColorEncoding::SRGB(source.num_channels == 1).ToExternal();
    Preview explicit_srgb;
    Preview by_default;
    generate(&srgb, 0.0f, &explicit_srgb);
    generate(nullptr, 0.0f, &by_default);
    EXPECT_EQ(source.num_channels, by_default.format.num_channels);
    EXPECT_EQ(explicit_srgb.format.num_channels,
              by_default.format.num_channels);
    EXPECT_EQ(explicit_srgb.pixels, by_default.pixels);
    // The output color encoding is reported.
    EXPECT_EQ(srgb.color_space, by_default.color_encoding.color_space);
    EXPECT_EQ(srgb.white_point, by_default.color_encoding.white_point);
    EXPECT_EQ(JXL_TRANSFER_FUNCTION_SRGB,
              by_default.color_encoding.transfer_function);

    // Without tone mapping: the source's encoding, as DecodeImageJXL gives it
    // when no color space is requested.
    Preview untouched;
    generate(nullptr, JXL_PREVIEW_NO_TONE_MAPPING, &untouched);
    jxl::extras::JXLDecompressParams dparams;
    for (uint32_t num_channels : {1u, 2u, 3u, 4u}) {
      dparams.accepted_formats.push_back(
          {num_channels, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0});
    }
    dparams.preview_downsampling = 2;
    jxl::extras::PackedPixelFile ppf;
    ASSERT_TRUE(jxl::extras::DecodeImageJXL(compressed.data(),
                                            compressed.size(), dparams,
                                            /*decoded_bytes=*/nullptr, &ppf));
    EXPECT_EQ(source.transfer_function, ppf.color_encoding.transfer_function);
    EXPECT_EQ(ppf.color_encoding.color_space,
              untouched.color_encoding.color_space);
    EXPECT_EQ(source.transfer_function,
              untouched.color_encoding.transfer_function);
    const jxl::extras::PackedImage& image = ppf.frames[0].color;
    const uint8_t* image_pixels = static_cast<const uint8_t*>(image.pixels());
    EXPECT_EQ(
        std::vector<uint8_t>(image_pixels, image_pixels + image.pixels_size),
        untouched.pixels);
    EXPECT_NE(by_default.pixels, untouched.pixels);
  }
}

// Without the decoder's preview hooks, the render method is still reported
// (through JxlDecoderGetImageOutDownsamplingMethod), but can only be
// restricted as a whole.
TEST(DecodeTest, PreviewDownsamplingWithoutHooks) {
  jxl::TestCodestreamParams params;
  params.cparams.speed_tier = jxl::SpeedTier::kLightning;
  std::vector<uint8_t> compressed =
      CreateDCOnlyTestCodestream(/*xsize=*/256, /*ysize=*/256, 3, params);
  auto decode = [&](uint32_t mask, jxl::extras::JXLPreviewBackend* backend,
                    jxl::extras::JXLPreviewFailureReason* failure_reason) {
    jxl::extras::JXLDecompressParams dparams;
    jxl::test::DefaultAcceptedFormats(dparams);
    dparams.preview_downsampling = 8;
    dparams.preview_allowed_backends = mask;
    dparams.preview_backend = backend;
    dparams.preview_failure_reason = failure_reason;
    jxl::extras::PackedPixelFile preview;
    return jxl::extras::DecodeImageJXL(compressed.data(), compressed.size(),
                                       dparams, nullptr, &preview);
  };
  using jxl::extras::JXLPreviewBackend;
  using jxl::extras::JXLPreviewFailureReason;
  JXLPreviewBackend backend = JXLPreviewBackend::kNone;
  JXLPreviewFailureReason failure_reason = JXLPreviewFailureReason::kNone;
  EXPECT_TRUE(decode(0, &backend, &failure_reason));
  EXPECT_EQ(JXLPreviewBackend::kNativeDcOnly, backend);

  backend = JXLPreviewBackend::kNone;
  EXPECT_TRUE(decode(PreviewBackendBit(JXLPreviewBackend::kDecoderDownsample),
                     &backend, &failure_reason));
  EXPECT_EQ(JXLPreviewBackend::kNativeDcOnly, backend);

  backend = JXLPreviewBackend::kNone;
  EXPECT_TRUE(
      decode(PreviewBackendBit(JXLPreviewBackend::kNativeDcOnly) |
                 PreviewBackendBit(JXLPreviewBackend::kNativeReducedInput) |
                 PreviewBackendBit(JXLPreviewBackend::kNativeFusedUpsampling) |
                 PreviewBackendBit(JXLPreviewBackend::kFallbackDownsample),
             &backend, &failure_reason));
  EXPECT_EQ(JXLPreviewBackend::kNativeDcOnly, backend);

  EXPECT_FALSE(decode(PreviewBackendBit(JXLPreviewBackend::kNativeDcOnly),
                      &backend, &failure_reason));
  EXPECT_EQ(JXLPreviewFailureReason::kNoBackendAvailable, failure_reason);
}

void ExpectPreviewSettingsRefused(JxlDecoder* dec) {
  EXPECT_EQ(JXL_DEC_ERROR, JxlDecoderSetImageOutDownsampling(dec, 1));
  EXPECT_EQ(JXL_DEC_ERROR, JxlDecoderSetImageOutDownsampling(dec, 2));
  EXPECT_EQ(JXL_DEC_ERROR,
            JxlDecoderSetPreferPreviewInplaceFlush(dec, JXL_FALSE));
  EXPECT_EQ(JXL_DEC_ERROR, jxl::GetDecoderPreviewHooks()->set_native_paths(
                               dec, JXL_TRUE, JXL_TRUE, JXL_TRUE));
}

// The preview settings apply to the frame announced by JXL_DEC_FRAME and
// change the size of its outputs: they are refused before that event, after
// the frame, and once any output of the frame is set, an extra channel buffer
// included (the buffer below is sized for factor 8, and was overrun at
// factor 1).
TEST(DecodeTest, PreviewDownsamplingSettingsTiming) {
  constexpr size_t xsize = 64;
  constexpr size_t ysize = 64;
  jxl::TestCodestreamParams params;
  params.cparams.speed_tier = jxl::SpeedTier::kLightning;
  const std::vector<uint8_t> compressed =
      CreateDCOnlyTestCodestream(xsize, ysize, /*num_channels=*/4, params);
  JxlDecoderPtr dec = JxlDecoderMake(nullptr);
  ASSERT_NE(dec, nullptr);
  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSubscribeEvents(
                                 dec.get(), JXL_DEC_BASIC_INFO | JXL_DEC_FRAME |
                                                JXL_DEC_FULL_IMAGE));
  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec.get(), compressed.data(),
                                                compressed.size()));
  JxlDecoderCloseInput(dec.get());
  ASSERT_EQ(JXL_DEC_BASIC_INFO, JxlDecoderProcessInput(dec.get()));
  ExpectPreviewSettingsRefused(dec.get());
  ASSERT_EQ(JXL_DEC_FRAME, JxlDecoderProcessInput(dec.get()));
  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetImageOutDownsampling(dec.get(), 8));
  const JxlPixelFormat alpha_format = {1, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
  size_t alpha_size = 0;
  ASSERT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderExtraChannelBufferSize(dec.get(), &alpha_format,
                                             &alpha_size, /*index=*/0));
  ASSERT_EQ((xsize / 8) * (ysize / 8), alpha_size);
  // Allocated at full size, so that the old decoder failed the expectations
  // without corrupting memory.
  std::vector<uint8_t> alpha(xsize * ysize);
  ASSERT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSetExtraChannelBuffer(dec.get(), &alpha_format,
                                            alpha.data(), alpha_size,
                                            /*index=*/0));
  ExpectPreviewSettingsRefused(dec.get());
  ASSERT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER, JxlDecoderProcessInput(dec.get()));
  ExpectPreviewSettingsRefused(dec.get());
  const JxlPixelFormat format = {3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
  size_t buffer_size = 0;
  ASSERT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderImageOutBufferSize(dec.get(), &format, &buffer_size));
  ASSERT_EQ((xsize / 8) * (ysize / 8) * 3, buffer_size);
  std::vector<uint8_t> pixels(xsize * ysize * 3);
  ASSERT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSetImageOutBuffer(dec.get(), &format, pixels.data(),
                                        buffer_size));
  ASSERT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec.get()));
  ExpectPreviewSettingsRefused(dec.get());
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderProcessInput(dec.get()));
}

// The preview settings can be changed until the frame's output is set, at
// JXL_DEC_NEED_IMAGE_OUT_BUFFER too, and then apply to the frame.
TEST(DecodeTest, PreviewDownsamplingSettingsAtNeedImageOutBuffer) {
  constexpr size_t xsize = 256;
  constexpr size_t ysize = 256;
  jxl::TestCodestreamParams params;
  params.cparams.speed_tier = jxl::SpeedTier::kLightning;
  const std::vector<uint8_t> compressed =
      CreateDCOnlyTestCodestream(xsize, ysize, /*num_channels=*/3, params);
  for (bool allow_native : {true, false}) {
    SCOPED_TRACE(allow_native);
    JxlDecoderPtr dec = JxlDecoderMake(nullptr);
    ASSERT_NE(dec, nullptr);
    ASSERT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderSubscribeEvents(dec.get(), JXL_DEC_FULL_IMAGE));
    ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec.get(), compressed.data(),
                                                  compressed.size()));
    JxlDecoderCloseInput(dec.get());
    ASSERT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER, JxlDecoderProcessInput(dec.get()));
    ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetImageOutDownsampling(dec.get(), 2));
    ASSERT_EQ(JXL_DEC_SUCCESS,
              jxl::GetDecoderPreviewHooks()->set_native_paths(
                  dec.get(), TO_JXL_BOOL(allow_native),
                  TO_JXL_BOOL(allow_native), TO_JXL_BOOL(allow_native)));
    const JxlPixelFormat format = {3, JXL_TYPE_FLOAT, JXL_NATIVE_ENDIAN, 0};
    size_t buffer_size = 0;
    ASSERT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderImageOutBufferSize(dec.get(), &format, &buffer_size));
    ASSERT_EQ((xsize / 2) * (ysize / 2) * 3 * sizeof(float), buffer_size);
    std::vector<uint8_t> pixels(buffer_size);
    ASSERT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderSetImageOutBuffer(dec.get(), &format, pixels.data(),
                                          pixels.size()));
    ASSERT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec.get()));
    JxlImageOutDownsamplingMethod method =
        JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_NONE;
    ASSERT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderGetImageOutDownsamplingMethod(dec.get(), &method));
    EXPECT_EQ(allow_native ? JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_REDUCED_INPUT
                           : JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_FULL_RESOLUTION,
              method);
  }
}

// A two-frame animation, `xsize` x `ysize`, encoded with `progressive_dc`.
std::vector<uint8_t> CreateAnimationCodestream(size_t xsize, size_t ysize,
                                               int progressive_dc) {
  JxlMemoryManager* memory_manager = jxl::test::MemoryManager();
  auto io = jxl::make_unique<jxl::CodecInOut>(memory_manager);
  EXPECT_TRUE(io->SetSize(xsize, ysize));
  io->metadata.m.SetUintSamples(16);
  io->metadata.m.color_encoding = jxl::ColorEncoding::SRGB(false);
  io->metadata.m.have_animation = true;
  io->frames.clear();
  const JxlPixelFormat format = {3, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};
  for (size_t frame = 0; frame < 2; ++frame) {
    std::vector<uint8_t> pixels =
        jxl::test::GetSomeTestImage(xsize, ysize, 3, frame);
    jxl::ImageBundle bundle(memory_manager, &io->metadata.m);
    EXPECT_TRUE(ConvertFromExternal(
        jxl::Bytes(pixels.data(), pixels.size()), xsize, ysize,
        jxl::ColorEncoding::SRGB(false), /*bits_per_sample=*/16, format,
        /*pool=*/nullptr, &bundle));
    bundle.duration = 1;
    io->frames.push_back(std::move(bundle));
  }
  jxl::CompressParams cparams;
  cparams.progressive_dc = progressive_dc;
  cparams.speed_tier = jxl::SpeedTier::kLightning;
  std::vector<uint8_t> compressed;
  EXPECT_TRUE(jxl::test::EncodeFile(cparams, io.get(), &compressed));
  return compressed;
}

void SetImageOutBufferForFrame(JxlDecoder* dec, const JxlPixelFormat& format,
                               std::vector<uint8_t>* pixels) {
  size_t buffer_size = 0;
  ASSERT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderImageOutBufferSize(dec, &format, &buffer_size));
  pixels->assign(buffer_size, 0);
  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetImageOutBuffer(
                                 dec, &format, pixels->data(), pixels->size()));
}

// Decodes the two frames of `compressed`, an animation, with output
// downsampling `factors[i]` set for frame i at its JXL_DEC_FRAME (none if 0),
// and returns the second one in `pixels`. With `early_buffer`, the output
// buffer of the second frame is set as soon as the first frame is decoded,
// before the second one is announced.
void DecodeLastAnimationFrame(const std::vector<uint8_t>& compressed,
                              const JxlPixelFormat& format,
                              const std::vector<size_t>& factors,
                              bool early_buffer, std::vector<uint8_t>* pixels) {
  JxlDecoderPtr dec = JxlDecoderMake(nullptr);
  ASSERT_NE(dec, nullptr);
  ASSERT_EQ(
      JXL_DEC_SUCCESS,
      JxlDecoderSubscribeEvents(dec.get(), JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE));
  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec.get(), compressed.data(),
                                                compressed.size()));
  JxlDecoderCloseInput(dec.get());
  size_t num_frames = 0;
  bool buffer_set = false;
  for (;;) {
    const JxlDecoderStatus status = JxlDecoderProcessInput(dec.get());
    if (status == JXL_DEC_FRAME) {
      ASSERT_LT(num_frames, factors.size());
      if (factors[num_frames] != 0) {
        EXPECT_EQ(
            buffer_set ? JXL_DEC_ERROR : JXL_DEC_SUCCESS,
            JxlDecoderSetImageOutDownsampling(dec.get(), factors[num_frames]));
      }
    } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
      ASSERT_FALSE(buffer_set);
      ASSERT_NO_FATAL_FAILURE(
          SetImageOutBufferForFrame(dec.get(), format, pixels));
    } else if (status == JXL_DEC_FULL_IMAGE) {
      ++num_frames;
      buffer_set = false;
      if (early_buffer && num_frames == 1) {
        ASSERT_NO_FATAL_FAILURE(
            SetImageOutBufferForFrame(dec.get(), format, pixels));
        buffer_set = true;
      }
    } else {
      ASSERT_EQ(JXL_DEC_SUCCESS, status);
      break;
    }
  }
  EXPECT_EQ(2u, num_frames);
}

// The output downsampling and the preview render settings of a frame end with
// it: the next frame is decoded without them unless they are set again. That
// includes an output buffer set for the next frame before it is announced;
// with progressive DC, the DC frame that precedes it is decoded with that
// buffer, and never sees a factor either. The settings of the first frame used
// to carry over.
TEST(DecodeTest, PreviewSettingsEndWithFrame) {
  constexpr size_t xsize = 256;
  constexpr size_t ysize = 256;
  const std::vector<uint8_t> compressed =
      CreateAnimationCodestream(xsize, ysize, /*progressive_dc=*/1);
  const JxlPixelFormat format = {3, JXL_TYPE_FLOAT, JXL_NATIVE_ENDIAN, 0};
  std::vector<uint8_t> expected;
  ASSERT_NO_FATAL_FAILURE(DecodeLastAnimationFrame(
      compressed, format, {1, 1}, /*early_buffer=*/false, &expected));
  ASSERT_EQ(xsize * ysize * 3 * sizeof(float), expected.size());
  for (size_t factor : {2u, 4u, 8u}) {
    for (bool early_buffer : {false, true}) {
      SCOPED_TRACE(testing::Message()
                   << "factor " << factor << " early_buffer " << early_buffer);
      std::vector<uint8_t> second;
      ASSERT_NO_FATAL_FAILURE(DecodeLastAnimationFrame(
          compressed, format, {factor, 0}, early_buffer, &second));
      EXPECT_TRUE(expected == second);
    }
  }

  // The native render methods disabled for the first frame only.
  const JxlDecoderPreviewHooks* hooks = jxl::GetDecoderPreviewHooks();
  JxlDecoderPtr dec = JxlDecoderMake(nullptr);
  ASSERT_NE(dec, nullptr);
  ASSERT_EQ(
      JXL_DEC_SUCCESS,
      JxlDecoderSubscribeEvents(dec.get(), JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE));
  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec.get(), compressed.data(),
                                                compressed.size()));
  JxlDecoderCloseInput(dec.get());
  for (size_t frame = 0; frame < 2; ++frame) {
    SCOPED_TRACE(frame);
    ASSERT_EQ(JXL_DEC_FRAME, JxlDecoderProcessInput(dec.get()));
    ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetImageOutDownsampling(dec.get(), 2));
    if (frame == 0) {
      ASSERT_EQ(JXL_DEC_SUCCESS, hooks->set_native_paths(dec.get(), JXL_FALSE,
                                                         JXL_FALSE, JXL_FALSE));
    }
    ASSERT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER, JxlDecoderProcessInput(dec.get()));
    std::vector<uint8_t> pixels;
    ASSERT_NO_FATAL_FAILURE(
        SetImageOutBufferForFrame(dec.get(), format, &pixels));
    ASSERT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec.get()));
    JxlImageOutDownsamplingMethod method =
        JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_NONE;
    ASSERT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderGetImageOutDownsamplingMethod(dec.get(), &method));
    EXPECT_EQ(frame == 0 ? JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_FULL_RESOLUTION
                         : JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_REDUCED_INPUT,
              method);
  }
  EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderProcessInput(dec.get()));
}

// The downsampling method of the current frame is NONE until the decoder sets
// up the frame's rendering, then the method it chose, until the next frame
// starts.
TEST(DecodeTest, ImageOutDownsamplingMethod) {
  const auto method_of =
      [](const JxlDecoder* dec) -> JxlImageOutDownsamplingMethod {
    // Not a method: shows whether the getter wrote its output.
    JxlImageOutDownsamplingMethod method =
        static_cast<JxlImageOutDownsamplingMethod>(7);
    EXPECT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderGetImageOutDownsamplingMethod(dec, &method));
    return method;
  };
  JxlDecoderPtr dec = JxlDecoderMake(nullptr);
  ASSERT_NE(dec, nullptr);
  EXPECT_EQ(JXL_DEC_ERROR,
            JxlDecoderGetImageOutDownsamplingMethod(dec.get(), nullptr));
  EXPECT_EQ(JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_NONE, method_of(dec.get()));

  const std::vector<uint8_t> compressed =
      CreateAnimationCodestream(/*xsize=*/256, /*ysize=*/256,
                                /*progressive_dc=*/0);
  const JxlPixelFormat format = {3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
  ASSERT_EQ(
      JXL_DEC_SUCCESS,
      JxlDecoderSubscribeEvents(dec.get(), JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE));
  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec.get(), compressed.data(),
                                                compressed.size()));
  JxlDecoderCloseInput(dec.get());
  const size_t factors[2] = {8, 2};
  const JxlImageOutDownsamplingMethod methods[2] = {
      JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_DC_ONLY,
      JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_REDUCED_INPUT};
  for (size_t frame = 0; frame < 2; ++frame) {
    SCOPED_TRACE(frame);
    ASSERT_EQ(JXL_DEC_FRAME, JxlDecoderProcessInput(dec.get()));
    EXPECT_EQ(JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_NONE, method_of(dec.get()));
    ASSERT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderSetImageOutDownsampling(dec.get(), factors[frame]));
    ASSERT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER, JxlDecoderProcessInput(dec.get()));
    EXPECT_EQ(JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_NONE, method_of(dec.get()));
    std::vector<uint8_t> pixels;
    ASSERT_NO_FATAL_FAILURE(
        SetImageOutBufferForFrame(dec.get(), format, &pixels));
    ASSERT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec.get()));
    EXPECT_EQ(methods[frame], method_of(dec.get()));
  }
  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderProcessInput(dec.get()));
  EXPECT_EQ(methods[1], method_of(dec.get()));
  JxlDecoderRewind(dec.get());
  EXPECT_EQ(JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_NONE, method_of(dec.get()));

  // A frame-upsampled image.
  jxl::TestCodestreamParams params;
  params.cparams.SetLossless();
  params.cparams.speed_tier = jxl::SpeedTier::kThunder;
  params.cparams.responsive = 0;
  params.cparams.resampling = 2;
  const std::vector<uint8_t> upsampled = CreateDCOnlyTestCodestream(
      /*xsize=*/128, /*ysize=*/96, /*num_channels=*/3, params);
  std::vector<uint8_t> pixels;
  size_t xsize = 0;
  size_t ysize = 0;
  JxlImageOutDownsamplingMethod method = JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_NONE;
  ASSERT_NO_FATAL_FAILURE(DecodeFirstFrameDownsampled(upsampled, /*factor=*/4,
                                                      format, nullptr, &pixels,
                                                      &xsize, &ysize, &method));
  EXPECT_EQ(JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_FUSED_UPSAMPLING, method);
}

// A frame decoded with JxlDecoderSetPreferPreviewInplaceFlush is neither saved
// for later frames nor intact after a flush (see also
// VerifyPreviewInplaceFlushResponsiveModular): decoding ends with it until the
// decoder is rewound. It used to continue, with the next frame blended onto a
// reference that was never saved.
TEST(DecodeTest, PreviewInplaceFlushEndsDecoding) {
  const std::vector<uint8_t> compressed =
      CreateReferenceableFirstFrameCodestream(
          /*xsize=*/64, /*ysize=*/64, /*num_channels=*/3);
  const JxlPixelFormat format = {3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
  JxlDecoderPtr dec = JxlDecoderMake(nullptr);
  ASSERT_NE(dec, nullptr);
  ASSERT_EQ(
      JXL_DEC_SUCCESS,
      JxlDecoderSubscribeEvents(dec.get(), JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE));
  for (bool inplace : {true, false}) {
    SCOPED_TRACE(inplace);
    ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec.get(), compressed.data(),
                                                  compressed.size()));
    JxlDecoderCloseInput(dec.get());
    ASSERT_EQ(JXL_DEC_FRAME, JxlDecoderProcessInput(dec.get()));
    ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetImageOutDownsampling(dec.get(), 2));
    ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetPreferPreviewInplaceFlush(
                                   dec.get(), TO_JXL_BOOL(inplace)));
    ASSERT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER, JxlDecoderProcessInput(dec.get()));
    std::vector<uint8_t> pixels;
    ASSERT_NO_FATAL_FAILURE(
        SetImageOutBufferForFrame(dec.get(), format, &pixels));
    ASSERT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec.get()));
    if (inplace) {
      EXPECT_EQ(JXL_DEC_ERROR, JxlDecoderProcessInput(dec.get()));
      EXPECT_EQ(JXL_DEC_ERROR, JxlDecoderProcessInput(dec.get()));
    } else {
      ASSERT_EQ(JXL_DEC_FRAME, JxlDecoderProcessInput(dec.get()));
      ASSERT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER,
                JxlDecoderProcessInput(dec.get()));
      ASSERT_NO_FATAL_FAILURE(
          SetImageOutBufferForFrame(dec.get(), format, &pixels));
      ASSERT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec.get()));
      EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderProcessInput(dec.get()));
    }
    // Decoding can start over.
    JxlDecoderRewind(dec.get());
  }
}

// A crop of `crop_xsize` x `crop_ysize` at (`crop_x0`, `crop_y0`) over a full
// frame that it replaces, saved for it with photon noise: the second frame
// shows the first, noise included, outside the crop.
std::vector<uint8_t> CreateNoisyCropOverReferenceCodestream(
    size_t xsize, size_t ysize, int crop_x0, int crop_y0, size_t crop_xsize,
    size_t crop_ysize) {
  JxlMemoryManager* memory_manager = jxl::test::MemoryManager();
  auto io = jxl::make_unique<jxl::CodecInOut>(memory_manager);
  EXPECT_TRUE(io->SetSize(xsize, ysize));
  io->metadata.m.SetUintSamples(16);
  io->metadata.m.color_encoding = jxl::ColorEncoding::SRGB(false);
  io->metadata.m.have_animation = true;
  io->frames.clear();
  const JxlPixelFormat format = {3, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};
  for (size_t i = 0; i < 2; ++i) {
    const size_t frame_xsize = i == 0 ? xsize : crop_xsize;
    const size_t frame_ysize = i == 0 ? ysize : crop_ysize;
    std::vector<uint8_t> pixels = jxl::test::GetSomeTestImage(
        frame_xsize, frame_ysize, /*num_channels=*/3, i);
    jxl::ImageBundle bundle(memory_manager, &io->metadata.m);
    EXPECT_TRUE(ConvertFromExternal(
        jxl::Bytes(pixels.data(), pixels.size()), frame_xsize, frame_ysize,
        jxl::ColorEncoding::SRGB(false), /*bits_per_sample=*/16, format,
        /*pool=*/nullptr, &bundle));
    bundle.duration = 1;
    if (i == 0) {
      bundle.use_for_next_frame = true;
    } else {
      bundle.origin = {crop_x0, crop_y0};
      bundle.blend = true;
      bundle.blendmode = jxl::BlendMode::kReplace;
    }
    io->frames.push_back(std::move(bundle));
  }
  jxl::CompressParams cparams;
  cparams.speed_tier = jxl::SpeedTier::kLightning;
  cparams.photon_noise_iso = 12800;
  std::vector<uint8_t> compressed;
  EXPECT_TRUE(jxl::test::EncodeFile(cparams, io.get(), &compressed));
  return compressed;
}

// A frame saved for later frames is saved as it is decoded at full
// resolution, noise included, whatever its own output downsampling: the next
// frame, a crop over it, is the same as when neither is downsampled. Noise
// used to be skipped for the saved frame too.
TEST(DecodeTest, PreviewDownsamplingKeepsNoiseOfReferenceFrames) {
  const std::vector<uint8_t> compressed =
      CreateNoisyCropOverReferenceCodestream(/*xsize=*/256, /*ysize=*/256,
                                             /*crop_x0=*/64, /*crop_y0=*/64,
                                             /*crop_xsize=*/128,
                                             /*crop_ysize=*/128);
  const JxlPixelFormat format = {3, JXL_TYPE_FLOAT, JXL_NATIVE_ENDIAN, 0};
  std::vector<uint8_t> expected;
  ASSERT_NO_FATAL_FAILURE(DecodeLastAnimationFrame(
      compressed, format, {1, 1}, /*early_buffer=*/false, &expected));
  for (size_t factor : {2u, 4u, 8u}) {
    SCOPED_TRACE(factor);
    std::vector<uint8_t> second;
    ASSERT_NO_FATAL_FAILURE(DecodeLastAnimationFrame(
        compressed, format, {factor, 1}, /*early_buffer=*/false, &second));
    EXPECT_TRUE(expected == second);
  }
}

// The frame header describes the frame at full resolution, whatever the
// output downsampling (which changes only the output sizes). Without
// coalescing, a downsampled layer size used to be reported with a full
// resolution crop, oriented with the full resolution image size.
TEST(DecodeTest, PreviewDownsamplingFrameHeaderAtFullResolution) {
  for (uint32_t orientation : {1u, 2u, 6u}) {
    SCOPED_TRACE(orientation);
    const std::vector<uint8_t> compressed = CreateBlendedCropCodestream(
        /*xsize=*/256, /*ysize=*/192, /*crop_x0=*/16, /*crop_y0=*/16,
        /*crop_xsize=*/128, /*crop_ysize=*/96, orientation);
    JxlDecoderPtr dec = JxlDecoderMake(nullptr);
    ASSERT_NE(dec, nullptr);
    ASSERT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderSubscribeEvents(dec.get(), JXL_DEC_FRAME));
    ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetCoalescing(dec.get(), JXL_FALSE));
    ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec.get(), compressed.data(),
                                                  compressed.size()));
    JxlDecoderCloseInput(dec.get());
    size_t num_frames = 0;
    for (JxlDecoderStatus status = JxlDecoderProcessInput(dec.get());
         status != JXL_DEC_SUCCESS;
         status = JxlDecoderProcessInput(dec.get())) {
      ASSERT_EQ(JXL_DEC_FRAME, status);
      ++num_frames;
      JxlFrameHeader full;
      ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetFrameHeader(dec.get(), &full));
      ASSERT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderSetImageOutDownsampling(dec.get(), 2));
      JxlFrameHeader downsampled;
      ASSERT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderGetFrameHeader(dec.get(), &downsampled));
      EXPECT_EQ(0, memcmp(&full, &downsampled, sizeof(full)));
      if (num_frames == 2) {
        // Oriented: mirrored (2), or turned clockwise in a 192x256 image (6).
        EXPECT_EQ(orientation > 4 ? 96u : 128u, full.layer_info.xsize);
        EXPECT_EQ(orientation > 4 ? 128u : 96u, full.layer_info.ysize);
        EXPECT_EQ(orientation == 1   ? 16
                  : orientation == 2 ? 256 - 16 - 128
                                     : 192 - 16 - 96,
                  full.layer_info.crop_x0);
        EXPECT_EQ(16, full.layer_info.crop_y0);
      }
    }
    EXPECT_EQ(2u, num_frames);
  }
}

struct CountingOutput {
  size_t num_pixels = 0;
};

void CountingOutputRun(void* opaque, size_t x, size_t y, size_t num_pixels,
                       const void* pixels) {
  static_cast<CountingOutput*>(opaque)->num_pixels += num_pixels;
}

// The image output is a buffer or a callback, and may change from one frame to
// the next: only the one set for the frame is written. The callback of the
// first frame used to be called instead of the buffer of the second; on ARM,
// the fast XYB to sRGB stage wrote the second frame to the buffer of the first
// instead of calling the callback.
TEST(DecodeTest, ImageOutBufferAndCallbackAcrossFrames) {
  constexpr size_t xsize = 64;
  constexpr size_t ysize = 64;
  const std::vector<uint8_t> compressed =
      CreateAnimationCodestream(xsize, ysize, /*progressive_dc=*/0);
  const JxlPixelFormat format = {3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
  std::vector<uint8_t> expected;
  ASSERT_NO_FATAL_FAILURE(DecodeLastAnimationFrame(
      compressed, format, {1, 1}, /*early_buffer=*/false, &expected));
  for (bool callback_first : {true, false}) {
    SCOPED_TRACE(callback_first);
    JxlDecoderPtr dec = JxlDecoderMake(nullptr);
    ASSERT_NE(dec, nullptr);
    ASSERT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderSubscribeEvents(dec.get(),
                                        JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE));
    ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec.get(), compressed.data(),
                                                  compressed.size()));
    JxlDecoderCloseInput(dec.get());
    CountingOutput counter;
    // Allocated at full size: the old decoder wrote the second frame here.
    std::vector<uint8_t> first_buffer(xsize * ysize * 3);
    std::vector<uint8_t> second_buffer;
    for (size_t frame = 0; frame < 2; ++frame) {
      ASSERT_EQ(JXL_DEC_FRAME, JxlDecoderProcessInput(dec.get()));
      // The first frame is smaller, so a stale output is also too small.
      ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetImageOutDownsampling(
                                     dec.get(), frame == 0 ? 8 : 1));
      ASSERT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER,
                JxlDecoderProcessInput(dec.get()));
      if ((frame == 0) == callback_first) {
        ASSERT_EQ(JXL_DEC_SUCCESS,
                  JxlDecoderSetImageOutCallback(dec.get(), &format,
                                                CountingOutputRun, &counter));
      } else if (frame == 0) {
        size_t buffer_size = 0;
        ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderImageOutBufferSize(
                                       dec.get(), &format, &buffer_size));
        ASSERT_EQ(JXL_DEC_SUCCESS,
                  JxlDecoderSetImageOutBuffer(
                      dec.get(), &format, first_buffer.data(), buffer_size));
      } else {
        ASSERT_NO_FATAL_FAILURE(
            SetImageOutBufferForFrame(dec.get(), format, &second_buffer));
      }
      const std::vector<uint8_t> first_buffer_before = first_buffer;
      const size_t num_pixels_before = counter.num_pixels;
      ASSERT_EQ(JXL_DEC_FULL_IMAGE, JxlDecoderProcessInput(dec.get()));
      if (frame == 0) continue;
      if (callback_first) {
        EXPECT_EQ(num_pixels_before, counter.num_pixels);
        EXPECT_TRUE(expected == second_buffer);
      } else {
        EXPECT_EQ(num_pixels_before + xsize * ysize, counter.num_pixels);
        EXPECT_TRUE(first_buffer_before == first_buffer);
      }
    }
    EXPECT_EQ(JXL_DEC_SUCCESS, JxlDecoderProcessInput(dec.get()));
  }
}

// Without coalescing, the output size is that of the current frame. A buffer
// set while a frame used only by later frames is decoded (here the reference
// frame holding the patches, when the input runs out) is checked against that
// frame's size. The displayed frame that follows is larger: the buffer is
// checked again before it is written, instead of being written past its size.
TEST(DecodeTest, OutputBufferSetDuringReferenceFrame) {
  const std::vector<uint8_t> orig =
      jxl::test::ReadTestData("jxl/grayscale_patches.png");
  jxl::extras::PackedPixelFile ppf;
  ASSERT_TRUE(jxl::extras::DecodeBytes(jxl::Bytes(orig),
                                       jxl::extras::ColorHints(), &ppf));
  jxl::extras::JXLCompressParams cparams;
  cparams.AddOption(JXL_ENC_FRAME_SETTING_PATCHES, 1);
  std::vector<uint8_t> compressed;
  ASSERT_TRUE(jxl::extras::EncodeImageJXL(cparams, ppf, nullptr, &compressed));
  JxlDecoderPtr dec = JxlDecoderMake(nullptr);
  ASSERT_NE(dec, nullptr);
  ASSERT_EQ(
      JXL_DEC_SUCCESS,
      JxlDecoderSubscribeEvents(dec.get(), JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE));
  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetCoalescing(dec.get(), JXL_FALSE));
  const JxlPixelFormat format = {1, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
  // Allocated at the size of the displayed frame: the old decoder wrote it in
  // full.
  std::vector<uint8_t> pixels(ppf.xsize() * ppf.ysize());
  size_t buffer_size = 0;
  bool frame_announced = false;
  size_t supplied = 0;
  JxlDecoderStatus status;
  for (;;) {
    status = JxlDecoderProcessInput(dec.get());
    if (status == JXL_DEC_FRAME) {
      frame_announced = true;
      continue;
    }
    if (status != JXL_DEC_NEED_MORE_INPUT) break;
    ASSERT_LT(supplied, compressed.size());
    // Succeeds without coalescing only once a frame header is read.
    if (buffer_size == 0 &&
        JXL_DEC_SUCCESS ==
            JxlDecoderImageOutBufferSize(dec.get(), &format, &buffer_size)) {
      ASSERT_FALSE(frame_announced);
      ASSERT_LT(buffer_size, pixels.size());
      ASSERT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderSetImageOutBuffer(dec.get(), &format, pixels.data(),
                                            buffer_size));
    }
    const size_t consumed = supplied - JxlDecoderReleaseInput(dec.get());
    supplied = std::min(compressed.size(), supplied + 64);
    ASSERT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderSetInput(dec.get(), compressed.data() + consumed,
                                 supplied - consumed));
    if (supplied == compressed.size()) JxlDecoderCloseInput(dec.get());
  }
  ASSERT_GT(buffer_size, 0u) << "the input never ran out in a hidden frame";
  EXPECT_TRUE(frame_announced);
  EXPECT_EQ(JXL_DEC_ERROR, status);
}

// Flushed before any AC pass, a VarDCT frame is drawn from its DC, upsampled.
// At reduced pipeline resolution the upsampled DC is box-averaged, so the
// flush is the full resolution flush downsampled; each DC value used to be
// repeated over its reduced block instead (blocky). Without colour transform,
// EPF and gaborish, and with sizes multiple of 8, the native path and the
// regular path (full resolution, averaged by the writer) compute the same
// averages.
TEST(DecodeTest, PreviewDownsamplingDCFlushMatchesRegularPath) {
  constexpr size_t xsize = 512;
  constexpr size_t ysize = 384;
  jxl::TestCodestreamParams params;
  params.cparams.color_transform = jxl::ColorTransform::kNone;
  params.cparams.epf = 0;
  params.cparams.gaborish = jxl::Override::kOff;
  const std::vector<uint8_t> pixels =
      jxl::test::GetSomeTestImage(xsize, ysize, /*num_channels=*/3, 0);
  const std::vector<uint8_t> compressed = jxl::CreateTestJXLCodestream(
      jxl::Bytes(pixels.data(), pixels.size()), xsize, ysize,
      /*num_channels=*/3, params);
  const JxlPixelFormat format = {3, JXL_TYPE_FLOAT, JXL_NATIVE_ENDIAN, 0};
  const JxlDecoderPreviewHooks* hooks = jxl::GetDecoderPreviewHooks();
  for (size_t factor : {2u, 4u}) {
    SCOPED_TRACE(factor);
    std::vector<uint8_t> flushed[2];
    for (bool native : {true, false}) {
      SCOPED_TRACE(native);
      JxlDecoderPtr dec = JxlDecoderMake(nullptr);
      ASSERT_NE(dec, nullptr);
      ASSERT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderSubscribeEvents(
                    dec.get(), JXL_DEC_FRAME | JXL_DEC_FRAME_PROGRESSION |
                                   JXL_DEC_FULL_IMAGE));
      ASSERT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderSetProgressiveDetail(dec.get(), kDC));
      ASSERT_EQ(
          JXL_DEC_SUCCESS,
          JxlDecoderSetInput(dec.get(), compressed.data(), compressed.size()));
      JxlDecoderCloseInput(dec.get());
      ASSERT_EQ(JXL_DEC_FRAME, JxlDecoderProcessInput(dec.get()));
      ASSERT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderSetImageOutDownsampling(dec.get(), factor));
      ASSERT_EQ(JXL_DEC_SUCCESS,
                hooks->set_native_paths(dec.get(), TO_JXL_BOOL(native),
                                        JXL_FALSE, JXL_FALSE));
      ASSERT_EQ(JXL_DEC_NEED_IMAGE_OUT_BUFFER,
                JxlDecoderProcessInput(dec.get()));
      std::vector<uint8_t>& out = flushed[native ? 0 : 1];
      ASSERT_NO_FATAL_FAILURE(
          SetImageOutBufferForFrame(dec.get(), format, &out));
      ASSERT_EQ(JXL_DEC_FRAME_PROGRESSION, JxlDecoderProcessInput(dec.get()));
      ASSERT_EQ(8u, JxlDecoderGetIntendedDownsamplingRatio(dec.get()));
      ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderFlushImage(dec.get()));
      JxlImageOutDownsamplingMethod method =
          JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_NONE;
      ASSERT_EQ(JXL_DEC_SUCCESS,
                JxlDecoderGetImageOutDownsamplingMethod(dec.get(), &method));
      ASSERT_EQ(native ? JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_REDUCED_INPUT
                       : JXL_IMAGE_OUT_DOWNSAMPLING_METHOD_FULL_RESOLUTION,
                method);
    }
    ASSERT_EQ(flushed[0].size(), flushed[1].size());
    const size_t num_samples = flushed[0].size() / sizeof(float);
    float max_error = 0.0f;
    for (size_t i = 0; i < num_samples; ++i) {
      float a;
      float b;
      memcpy(&a, flushed[0].data() + i * sizeof(float), sizeof(float));
      memcpy(&b, flushed[1].data() + i * sizeof(float), sizeof(float));
      max_error = std::max(max_error, std::abs(a - b));
    }
    EXPECT_LT(max_error, 1e-5f);
  }
}

// Lossless float image with signed colour samples and float alpha.
std::vector<uint8_t> CreateLosslessFloatCodestream(size_t xsize, size_t ysize,
                                                   int group_size_shift) {
  JxlMemoryManager* memory_manager = jxl::test::MemoryManager();
  auto io = jxl::make_unique<jxl::CodecInOut>(memory_manager);
  io->metadata.m.SetFloat32Samples();
  io->metadata.m.color_encoding = jxl::ColorEncoding::LinearSRGB();
  JXL_TEST_ASSIGN_OR_DIE(jxl::Image3F color,
                         jxl::Image3F::Create(memory_manager, xsize, ysize));
  JXL_TEST_ASSIGN_OR_DIE(jxl::ImageF alpha,
                         jxl::ImageF::Create(memory_manager, xsize, ysize));
  for (size_t y = 0; y < ysize; ++y) {
    float* JXL_RESTRICT row0 = color.PlaneRow(0, y);
    float* JXL_RESTRICT row1 = color.PlaneRow(1, y);
    float* JXL_RESTRICT row2 = color.PlaneRow(2, y);
    float* JXL_RESTRICT row_alpha = alpha.Row(y);
    for (size_t x = 0; x < xsize; ++x) {
      // Alternating signs average to values far from every sample.
      const float sign = ((x + y) & 1) ? -1.0f : 1.0f;
      row0[x] = sign * (0.5f + x / 256.0f);
      row1[x] = sign * (y / 128.0f);
      row2[x] = 1.5f - ((x * 7 + y * 3) & 255) / 128.0f;
      row_alpha[x] = ((x ^ y) & 15) / 16.0f;
    }
  }
  EXPECT_TRUE(
      io->SetFromImage(std::move(color), jxl::ColorEncoding::LinearSRGB()));
  jxl::ExtraChannelInfo info;
  info.type = jxl::ExtraChannel::kAlpha;
  info.bit_depth = io->metadata.m.bit_depth;
  io->metadata.m.extra_channel_info.push_back(info);
  std::vector<jxl::ImageF> extra_channels;
  extra_channels.push_back(std::move(alpha));
  EXPECT_TRUE(io->frames[0].SetExtraChannels(std::move(extra_channels)));

  jxl::CompressParams cparams;
  cparams.SetLossless();
  cparams.speed_tier = jxl::SpeedTier::kThunder;
  cparams.responsive = 0;
  cparams.modular_group_size_shift = group_size_shift;
  std::vector<uint8_t> compressed;
  EXPECT_TRUE(jxl::test::EncodeFile(cparams, io.get(), &compressed));
  return compressed;
}

// Reduced-input previews of float modular images are box averages of the full
// decode. Float samples used to be averaged as the integers that encode them.
TEST(DecodeTest, PreviewDownsamplingModularLosslessFloat) {
  struct Case {
    size_t xsize;
    size_t ysize;
    int group_size_shift;
  };
  // One group is decoded from the global section (FinalizeDecoding); 128x128
  // groups are decoded one by one (DecodeGroup).
  for (const Case& test : {Case{200, 150, -1}, Case{300, 270, 0}}) {
    const size_t xsize = test.xsize;
    const size_t ysize = test.ysize;
    SCOPED_TRACE(testing::Message() << xsize << "x" << ysize);
    const std::vector<uint8_t> compressed =
        CreateLosslessFloatCodestream(xsize, ysize, test.group_size_shift);
    const JxlPixelFormat format = {4, JXL_TYPE_FLOAT, JXL_LITTLE_ENDIAN, 0};
    jxl::extras::JXLDecompressParams full_params;
    full_params.accepted_formats = {format};
    jxl::extras::PackedPixelFile full;
    ASSERT_TRUE(jxl::extras::DecodeImageJXL(compressed.data(),
                                            compressed.size(), full_params,
                                            /*decoded_bytes=*/nullptr, &full));
    for (size_t factor : {2u, 4u, 8u}) {
      SCOPED_TRACE(factor);
      jxl::extras::JXLDecompressParams params;
      params.accepted_formats = {format};
      params.preview_downsampling = factor;
      params.preview_hooks = jxl::GetDecoderPreviewHooks();
      jxl::extras::JXLPreviewBackend backend =
          jxl::extras::JXLPreviewBackend::kNone;
      params.preview_backend = &backend;
      jxl::extras::PackedPixelFile preview;
      ASSERT_TRUE(jxl::extras::DecodeImageJXL(
          compressed.data(), compressed.size(), params,
          /*decoded_bytes=*/nullptr, &preview));
      EXPECT_EQ(jxl::extras::JXLPreviewBackend::kNativeReducedInput, backend);
      ASSERT_EQ(1u, preview.frames.size());
      const jxl::extras::PackedImage& image = preview.frames[0].color;
      ASSERT_EQ(jxl::DivCeil(xsize, factor), image.xsize);
      ASSERT_EQ(jxl::DivCeil(ysize, factor), image.ysize);
      ASSERT_EQ(4u, image.format.num_channels);
      float max_error = 0.0f;
      for (size_t y = 0; y < image.ysize; ++y) {
        for (size_t x = 0; x < image.xsize; ++x) {
          for (size_t c = 0; c < 4; ++c) {
            const float error = std::abs(
                BoxAveragePixel(full.frames[0].color, factor, x, y, c) -
                image.GetPixelValue(y, x, c));
            ASSERT_FALSE(std::isnan(error)) << x << "," << y << " c=" << c;
            max_error = std::max(max_error, error);
          }
        }
      }
      // Float rounding of sums of up to 64 samples of magnitude < 4.
      EXPECT_LT(max_error, 1e-4f);
    }
  }
}

// Lossless binary32 samples of both signs and all magnitudes, with zeros of
// both signs, infinities and subnormals, round-trip bit for bit at every
// effort. The modular encoder codes them as their bit patterns, whose
// differences do not fit an int32: it used to compute residuals (and the
// absolute values of samples while learning trees) with signed overflow,
// which UBSan reports.
TEST(DecodeTest, ModularLosslessBinary32MixedSigns) {
  JxlMemoryManager* memory_manager = jxl::test::MemoryManager();
  constexpr size_t xsize = 67;
  constexpr size_t ysize = 45;
  constexpr float kInf = std::numeric_limits<float>::infinity();
  const float specials[] = {0.0f, -0.0f, kInf, -kInf, 1e-40f, -5e-45f};
  std::vector<float> samples(xsize * ysize * 3);
  for (size_t y = 0; y < ysize; ++y) {
    for (size_t x = 0; x < xsize; ++x) {
      for (size_t c = 0; c < 3; ++c) {
        uint32_t h = static_cast<uint32_t>(x * 0x9E3779B1u) ^
                     static_cast<uint32_t>(y * 0x85EBCA77u) ^
                     static_cast<uint32_t>(c * 0xC2B2AE3Du);
        h ^= h >> 15;
        h *= 0x2C1B3C6Du;
        h ^= h >> 12;
        float value;
        if (h % 16 == 0) {
          value = specials[(h >> 4) % 6];
        } else {
          value = std::ldexp(1.0f + ((h >> 8) & 255) / 256.0f,
                             static_cast<int>((h >> 16) % 121) - 60);
          if (h & 16) value = -value;
        }
        samples[(y * xsize + x) * 3 + c] = value;
      }
    }
  }
  for (jxl::SpeedTier speed :
       {jxl::SpeedTier::kThunder, jxl::SpeedTier::kFalcon,
        jxl::SpeedTier::kCheetah, jxl::SpeedTier::kSquirrel,
        jxl::SpeedTier::kTortoise}) {
    SCOPED_TRACE(static_cast<int>(speed));
    auto io = jxl::make_unique<jxl::CodecInOut>(memory_manager);
    io->metadata.m.SetFloat32Samples();
    io->metadata.m.color_encoding = jxl::ColorEncoding::LinearSRGB();
    JXL_TEST_ASSIGN_OR_DIE(jxl::Image3F color,
                           jxl::Image3F::Create(memory_manager, xsize, ysize));
    for (size_t y = 0; y < ysize; ++y) {
      for (size_t x = 0; x < xsize; ++x) {
        for (size_t c = 0; c < 3; ++c) {
          color.PlaneRow(c, y)[x] = samples[(y * xsize + x) * 3 + c];
        }
      }
    }
    ASSERT_TRUE(
        io->SetFromImage(std::move(color), jxl::ColorEncoding::LinearSRGB()));
    jxl::CompressParams cparams;
    cparams.SetLossless();
    cparams.speed_tier = speed;
    std::vector<uint8_t> compressed;
    ASSERT_TRUE(jxl::test::EncodeFile(cparams, io.get(), &compressed));

    jxl::extras::JXLDecompressParams dparams;
    dparams.accepted_formats = {{3, JXL_TYPE_FLOAT, JXL_NATIVE_ENDIAN, 0}};
    jxl::extras::PackedPixelFile ppf;
    ASSERT_TRUE(jxl::extras::DecodeImageJXL(compressed.data(),
                                            compressed.size(), dparams,
                                            /*decoded_bytes=*/nullptr, &ppf));
    ASSERT_EQ(1u, ppf.frames.size());
    const jxl::extras::PackedImage& image = ppf.frames[0].color;
    ASSERT_EQ(samples.size() * sizeof(float), image.pixels_size);
    EXPECT_EQ(0, memcmp(samples.data(), image.pixels(), image.pixels_size));
  }
}

double MeanOfUint16BE(const std::vector<uint8_t>& pixels) {
  double sum = 0;
  for (size_t i = 0; i + 1 < pixels.size(); i += 2) {
    sum += (pixels[i] << 8) | pixels[i + 1];
  }
  return sum / (pixels.size() / 2) / 65535;
}

// A modular frame pauses only at steps where squeeze residuals are all that is
// missing. Without squeeze its channels arrive whole in the last pass, so it
// has no steps; with squeeze, every step renders the whole frame. All input
// but the last byte is available at once, so the decoder learns which steps
// there are in the call that also decodes the passes past them.
TEST(DecodeTest, ModularProgressionStepsNeedSqueeze) {
  constexpr size_t xsize = 277;
  constexpr size_t ysize = 280;
  const std::vector<uint8_t> pixels =
      jxl::test::GetSomeTestImage(xsize, ysize, 3, 0);
  const JxlPixelFormat format = {3, JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};
  const jxl::PassDefinition kPasses[] = {
      {2, 0, 4}, {4, 0, 4}, {8, 2, 2}, {8, 1, 2}, {8, 0, 1}};
  jxl::ProgressiveMode progressive_mode{kPasses};
  for (int responsive : {0, 1}) {
    SCOPED_TRACE(responsive);
    jxl::TestCodestreamParams params;
    params.cparams.SetLossless();
    params.cparams.responsive = responsive;
    params.cparams.custom_progressive_mode = &progressive_mode;
    // 128x128 groups: in larger ones, all of this image is in the global
    // section, which then ends at the last byte.
    params.cparams.modular_group_size_shift = 0;
    const std::vector<uint8_t> data = jxl::CreateTestJXLCodestream(
        jxl::Bytes(pixels.data(), pixels.size()), xsize, ysize, 3, params);

    JxlDecoderPtr dec = JxlDecoderMake(nullptr);
    ASSERT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderSubscribeEvents(dec.get(),
                                        JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE |
                                            JXL_DEC_FRAME_PROGRESSION));
    ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetProgressiveDetail(
                                   dec.get(), JxlProgressiveDetail::kPasses));
    ASSERT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderSetInput(dec.get(), data.data(), data.size() - 1));
    ASSERT_EQ(JXL_DEC_FRAME, JxlDecoderProcessInput(dec.get()));
    std::vector<uint8_t> output(pixels.size());
    ASSERT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderSetImageOutBuffer(dec.get(), &format, output.data(),
                                          output.size()));
    std::vector<size_t> ratios;
    JxlDecoderStatus status;
    while ((status = JxlDecoderProcessInput(dec.get())) ==
           JXL_DEC_FRAME_PROGRESSION) {
      ratios.push_back(JxlDecoderGetIntendedDownsamplingRatio(dec.get()));
      ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderFlushImage(dec.get()));
      // Each step renders the whole frame, not a black image.
      EXPECT_NEAR(MeanOfUint16BE(pixels), MeanOfUint16BE(output), 0.02);
    }
    EXPECT_EQ(JXL_DEC_NEED_MORE_INPUT, status);
    if (responsive) {
      ASSERT_FALSE(ratios.empty());
      EXPECT_EQ(8u, ratios[0]);
    } else {
      EXPECT_TRUE(ratios.empty()) << ratios.size() << " steps";
    }
  }
}

// Without coalescing, a preview holds the layers of the first displayed frame,
// each of its size divided by the factor (rounded up) and at its crop divided
// by the factor (rounded down), on the canvas divided by the factor. Layers
// used to get buffers of the canvas size, which the decoder refused, crops
// rounded toward zero, and only the first layer was decoded.
TEST(DecodeTest, PreviewDownsamplingNonCoalescedLayers) {
  using jxl::extras::JXLPreviewBackend;
  constexpr size_t xsize = 301;
  constexpr size_t ysize = 203;
  struct Crop {
    int x0, y0;
    size_t xsize, ysize;
  };
  for (const Crop& crop : {Crop{37, 21, 150, 100}, Crop{-3, -5, 101, 67}}) {
    for (uint32_t orientation :
         {JXL_ORIENT_IDENTITY, JXL_ORIENT_ROTATE_90_CW}) {
      SCOPED_TRACE(::testing::Message() << crop.x0 << "," << crop.y0
                                        << " orientation " << orientation);
      const std::vector<uint8_t> compressed = CreateBlendedCropCodestream(
          xsize, ysize, crop.x0, crop.y0, crop.xsize, crop.ysize, orientation);
      jxl::extras::JXLDecompressParams dparams;
      dparams.accepted_formats = {{4, JXL_TYPE_FLOAT, JXL_NATIVE_ENDIAN, 0}};
      dparams.coalescing = false;
      jxl::extras::PackedPixelFile full;
      ASSERT_TRUE(jxl::extras::DecodeImageJXL(
          compressed.data(), compressed.size(), dparams,
          /*decoded_bytes=*/nullptr, &full));
      ASSERT_EQ(2u, full.frames.size());
      // The first displayed frame is made of both layers.
      dparams.first_frame_only = true;
      jxl::extras::PackedPixelFile first;
      ASSERT_TRUE(jxl::extras::DecodeImageJXL(
          compressed.data(), compressed.size(), dparams,
          /*decoded_bytes=*/nullptr, &first));
      EXPECT_EQ(2u, first.frames.size());
      dparams.first_frame_only = false;
      for (size_t factor : {2u, 4u, 8u}) {
        SCOPED_TRACE(factor);
        dparams.preview_downsampling = factor;
        dparams.preview_hooks = jxl::GetDecoderPreviewHooks();
        dparams.preview_allowed_backends =
            PreviewBackendBit(JXLPreviewBackend::kFallbackDownsample);
        jxl::extras::PackedPixelFile preview;
        ASSERT_TRUE(jxl::extras::DecodeImageJXL(
            compressed.data(), compressed.size(), dparams,
            /*decoded_bytes=*/nullptr, &preview));
        EXPECT_EQ(jxl::DivCeil(full.info.xsize, factor), preview.info.xsize);
        EXPECT_EQ(jxl::DivCeil(full.info.ysize, factor), preview.info.ysize);
        ASSERT_EQ(2u, preview.frames.size());
        for (size_t i = 0; i < 2; ++i) {
          SCOPED_TRACE(i);
          const JxlLayerInfo& big = full.frames[i].frame_info.layer_info;
          const JxlLayerInfo& small = preview.frames[i].frame_info.layer_info;
          const auto floor_div = [factor](int32_t a) {
            return static_cast<int32_t>(
                std::floor(a / static_cast<double>(factor)));
          };
          EXPECT_EQ(jxl::DivCeil(big.xsize, factor), small.xsize);
          EXPECT_EQ(jxl::DivCeil(big.ysize, factor), small.ysize);
          EXPECT_EQ(floor_div(big.crop_x0), small.crop_x0);
          EXPECT_EQ(floor_div(big.crop_y0), small.crop_y0);
          const jxl::extras::PackedImage& image = preview.frames[i].color;
          ASSERT_EQ(small.xsize, image.xsize);
          ASSERT_EQ(small.ysize, image.ysize);
          // The boxes of a layer start at its origin in the codestream.
          if (orientation != JXL_ORIENT_IDENTITY) continue;
          double max_error = 0.0;
          for (size_t y = 0; y < image.ysize; ++y) {
            for (size_t x = 0; x < image.xsize; ++x) {
              for (size_t c = 0; c < 4; ++c) {
                max_error = std::max<double>(
                    max_error, std::abs(BoxAveragePixel(full.frames[i].color,
                                                        factor, x, y, c) -
                                        image.GetPixelValue(y, x, c)));
              }
            }
          }
          EXPECT_LT(max_error, 1e-5);
        }
      }
    }
  }
}

// Box averages by `factor` of `pixels`, `xsize` x `ysize` pixels of
// `num_channels` 16-bit big-endian samples, rounded, in the same layout.
std::vector<uint8_t> BoxAverageUint16BE(const std::vector<uint8_t>& pixels,
                                        size_t xsize, size_t ysize,
                                        size_t num_channels, size_t factor) {
  const size_t out_xsize = jxl::DivCeil(xsize, factor);
  const size_t out_ysize = jxl::DivCeil(ysize, factor);
  std::vector<uint8_t> out(out_xsize * out_ysize * num_channels * 2);
  for (size_t y = 0; y < out_ysize; ++y) {
    for (size_t x = 0; x < out_xsize; ++x) {
      for (size_t c = 0; c < num_channels; ++c) {
        uint32_t sum = 0;
        uint32_t count = 0;
        for (size_t fy = y * factor; fy < std::min(ysize, (y + 1) * factor);
             ++fy) {
          for (size_t fx = x * factor; fx < std::min(xsize, (x + 1) * factor);
               ++fx) {
            const size_t i = ((fy * xsize + fx) * num_channels + c) * 2;
            sum += (pixels[i] << 8) | pixels[i + 1];
            ++count;
          }
        }
        const uint32_t average = (sum + count / 2) / count;
        const size_t o = ((y * out_xsize + x) * num_channels + c) * 2;
        out[o] = average >> 8;
        out[o + 1] = average & 0xFF;
      }
    }
  }
  return out;
}

// A lossless image of `num_channels` (3 or 4) with `orientation`, and an
// embedded preview that holds its box average by `preview_factor`, in the
// codestream's orientation (where the decoder's boxes are too).
std::vector<uint8_t> CreateEmbeddedPreviewCodestream(size_t xsize, size_t ysize,
                                                     size_t num_channels,
                                                     size_t preview_factor,
                                                     uint32_t orientation) {
  JxlMemoryManager* memory_manager = jxl::test::MemoryManager();
  auto io = jxl::make_unique<jxl::CodecInOut>(memory_manager);
  EXPECT_TRUE(io->SetSize(xsize, ysize));
  io->metadata.m.SetUintSamples(16);
  if (num_channels == 4) io->metadata.m.SetAlphaBits(16);
  io->metadata.m.color_encoding = jxl::ColorEncoding::SRGB(false);
  io->metadata.m.orientation = orientation;
  const JxlPixelFormat format = {static_cast<uint32_t>(num_channels),
                                 JXL_TYPE_UINT16, JXL_BIG_ENDIAN, 0};
  const std::vector<uint8_t> pixels =
      jxl::test::GetSomeTestImage(xsize, ysize, num_channels, 0);
  EXPECT_TRUE(ConvertFromExternal(jxl::Bytes(pixels.data(), pixels.size()),
                                  xsize, ysize, jxl::ColorEncoding::SRGB(false),
                                  /*bits_per_sample=*/16, format,
                                  /*pool=*/nullptr, &io->Main(),
                                  /*set_alpha=*/num_channels == 4));
  const size_t preview_xsize = jxl::DivCeil(xsize, preview_factor);
  const size_t preview_ysize = jxl::DivCeil(ysize, preview_factor);
  const std::vector<uint8_t> preview_pixels =
      BoxAverageUint16BE(pixels, xsize, ysize, num_channels, preview_factor);
  jxl::ImageBundle preview(memory_manager, &io->metadata.m);
  EXPECT_TRUE(ConvertFromExternal(
      jxl::Bytes(preview_pixels.data(), preview_pixels.size()), preview_xsize,
      preview_ysize, jxl::ColorEncoding::SRGB(false), /*bits_per_sample=*/16,
      format, /*pool=*/nullptr, &preview, /*set_alpha=*/num_channels == 4));
  io->preview_frame = std::move(preview);
  io->metadata.m.have_preview = true;
  EXPECT_TRUE(io->metadata.m.preview_size.Set(preview_xsize, preview_ysize));
  jxl::CompressParams cparams;
  cparams.SetLossless();
  cparams.speed_tier = jxl::SpeedTier::kThunder;
  std::vector<uint8_t> compressed;
  EXPECT_TRUE(jxl::test::EncodeFile(cparams, io.get(), &compressed));
  return compressed;
}

uint16_t Uint16Sample(const jxl::extras::PackedImage& image, size_t x, size_t y,
                      size_t c) {
  uint16_t value;
  memcpy(&value, image.const_pixels(y, x, c), sizeof(value));
  return value;
}

// The embedded preview stands in for the decoder's preview only when it is
// exactly that image's size, and is then output as that image: in display
// orientation, at the requested bit depth, as the only frame. The decoder
// used to report the preview size of the codestream, not of its output, for
// orientations that transpose: such previews were skipped, or returned with
// transposed rows. The bit depth was not applied to the preview, and smaller
// previews were returned too.
TEST(DecodeTest, PreviewDownsamplingEmbeddedPreview) {
  using jxl::extras::JXLPreviewBackend;
  constexpr size_t xsize = 400;
  constexpr size_t ysize = 200;
  constexpr size_t kPreviewFactor = 4;
  for (uint32_t orientation : {JXL_ORIENT_IDENTITY, JXL_ORIENT_ROTATE_90_CW}) {
    const bool transposed = orientation == JXL_ORIENT_ROTATE_90_CW;
    for (uint32_t num_channels : {3u, 4u}) {
      SCOPED_TRACE(::testing::Message() << "orientation " << orientation
                                        << ", channels " << num_channels);
      const std::vector<uint8_t> compressed = CreateEmbeddedPreviewCodestream(
          xsize, ysize, num_channels, kPreviewFactor, orientation);
      // The decoder describes the preview it outputs.
      for (bool keep_orientation : {false, true}) {
        JxlDecoderPtr dec = JxlDecoderMake(nullptr);
        ASSERT_EQ(JXL_DEC_SUCCESS,
                  JxlDecoderSubscribeEvents(dec.get(), JXL_DEC_BASIC_INFO));
        ASSERT_EQ(JXL_DEC_SUCCESS,
                  JxlDecoderSetKeepOrientation(dec.get(),
                                               TO_JXL_BOOL(keep_orientation)));
        ASSERT_EQ(JXL_DEC_SUCCESS,
                  JxlDecoderSetInput(dec.get(), compressed.data(),
                                     compressed.size()));
        ASSERT_EQ(JXL_DEC_BASIC_INFO, JxlDecoderProcessInput(dec.get()));
        JxlBasicInfo info;
        ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBasicInfo(dec.get(), &info));
        const bool swap = transposed && !keep_orientation;
        EXPECT_EQ(swap ? ysize : xsize, info.xsize);
        ASSERT_TRUE(info.have_preview);
        EXPECT_EQ((swap ? ysize : xsize) / kPreviewFactor, info.preview.xsize);
        EXPECT_EQ((swap ? xsize : ysize) / kPreviewFactor, info.preview.ysize);
      }
      const size_t out_xsize = transposed ? ysize : xsize;
      const size_t out_ysize = transposed ? xsize : ysize;
      for (uint32_t bits : {16u, 10u}) {
        SCOPED_TRACE(bits);
        const auto decode = [&](size_t factor, uint32_t allowed_backends,
                                JXLPreviewBackend* backend,
                                jxl::extras::PackedPixelFile* ppf) {
          jxl::extras::JXLDecompressParams dparams;
          dparams.accepted_formats = {
              {num_channels, JXL_TYPE_UINT16, JXL_NATIVE_ENDIAN, 0}};
          if (bits != 16) {
            dparams.output_bitdepth = {JXL_BIT_DEPTH_CUSTOM, bits, 0};
          }
          dparams.preview_downsampling = factor;
          dparams.preview_hooks = jxl::GetDecoderPreviewHooks();
          dparams.preview_allowed_backends = allowed_backends;
          dparams.preview_backend = backend;
          return jxl::extras::DecodeImageJXL(compressed.data(),
                                             compressed.size(), dparams,
                                             /*decoded_bytes=*/nullptr, ppf);
        };
        JXLPreviewBackend backend = JXLPreviewBackend::kNone;
        jxl::extras::PackedPixelFile embedded;
        ASSERT_TRUE(decode(kPreviewFactor, 0, &backend, &embedded));
        EXPECT_EQ(JXLPreviewBackend::kEmbeddedPreview, backend);
        EXPECT_EQ(out_xsize / kPreviewFactor, embedded.info.xsize);
        EXPECT_EQ(out_ysize / kPreviewFactor, embedded.info.ysize);
        EXPECT_FALSE(embedded.info.have_preview);
        EXPECT_EQ(nullptr, embedded.preview_frame.get());
        EXPECT_EQ(bits, embedded.info.bits_per_sample);
        if (num_channels == 4) {
          EXPECT_EQ(bits, embedded.info.alpha_bits);
        }
        ASSERT_EQ(1u, embedded.frames.size());
        EXPECT_TRUE(embedded.frames[0].frame_info.is_last);
        const jxl::extras::PackedImage& image = embedded.frames[0].color;
        ASSERT_EQ(embedded.info.xsize, image.xsize);
        ASSERT_EQ(embedded.info.ysize, image.ysize);
        // The same image, downsampled by the decoder.
        jxl::extras::PackedPixelFile downsampled;
        ASSERT_TRUE(
            decode(kPreviewFactor,
                   PreviewBackendBit(JXLPreviewBackend::kFallbackDownsample),
                   &backend, &downsampled));
        EXPECT_EQ(JXLPreviewBackend::kFallbackDownsample, backend);
        const jxl::extras::PackedImage& reference = downsampled.frames[0].color;
        ASSERT_EQ(reference.xsize, image.xsize);
        ASSERT_EQ(reference.ysize, image.ysize);
        int max_difference = 0;
        uint16_t max_sample = 0;
        for (size_t y = 0; y < image.ysize; ++y) {
          for (size_t x = 0; x < image.xsize; ++x) {
            for (size_t c = 0; c < num_channels; ++c) {
              const uint16_t sample = Uint16Sample(image, x, y, c);
              max_sample = std::max(max_sample, sample);
              max_difference =
                  std::max(max_difference,
                           std::abs(sample - Uint16Sample(reference, x, y, c)));
            }
          }
        }
        // Both round the same box averages.
        EXPECT_LE(max_difference, 1);
        EXPECT_LT(max_sample, 1u << bits);

        // A preview of another size than the requested one is not used.
        for (size_t factor : {2u, 8u}) {
          SCOPED_TRACE(factor);
          jxl::extras::PackedPixelFile other;
          ASSERT_TRUE(decode(factor, 0, &backend, &other));
          EXPECT_NE(JXLPreviewBackend::kEmbeddedPreview, backend);
          EXPECT_EQ(jxl::DivCeil(out_xsize, factor), other.info.xsize);
          EXPECT_EQ(jxl::DivCeil(out_ysize, factor), other.info.ysize);
          ASSERT_EQ(1u, other.frames.size());
          EXPECT_EQ(other.info.xsize, other.frames[0].color.xsize);
        }
      }
      if (num_channels != 4) continue;
      // Alpha outside the color format is an extra channel, which the decoder
      // does not output for the preview: the image is downsampled instead.
      jxl::extras::JXLDecompressParams dparams;
      dparams.accepted_formats = {{3, JXL_TYPE_UINT16, JXL_NATIVE_ENDIAN, 0}};
      dparams.preview_downsampling = kPreviewFactor;
      JXLPreviewBackend backend = JXLPreviewBackend::kNone;
      dparams.preview_backend = &backend;
      jxl::extras::PackedPixelFile ppf;
      ASSERT_TRUE(jxl::extras::DecodeImageJXL(compressed.data(),
                                              compressed.size(), dparams,
                                              /*decoded_bytes=*/nullptr, &ppf));
      EXPECT_EQ(JXLPreviewBackend::kNativeReducedInput, backend);
      ASSERT_EQ(1u, ppf.frames.size());
      ASSERT_EQ(1u, ppf.frames[0].extra_channels.size());
      EXPECT_EQ(out_xsize / kPreviewFactor,
                ppf.frames[0].extra_channels[0].xsize);
      EXPECT_EQ(out_ysize / kPreviewFactor,
                ppf.frames[0].extra_channels[0].ysize);
    }
  }
}

// What CallGeneratePreview received.
struct PreviewApiResult {
  JxlPreviewStatus status = JXL_PREVIEW_INTERNAL_ERROR;
  uint32_t xsize = 0;
  uint32_t ysize = 0;
  size_t stride = 0;
  size_t pixels_size = 0;
  JxlPixelFormat format = {};
  JxlPreviewBackend backend = JXL_PREVIEW_BACKEND_NONE;
  uint32_t downsampling = 0;
  JxlColorEncoding color_encoding = {};
  const uint8_t* pixels_address = nullptr;
  bool pixels_null = false;
  std::vector<uint8_t> pixels;
};

// Calls JxlGeneratePreview on `input` with every output set, to values it must
// overwrite; `setup` sets the inputs.
template <typename Setup>
PreviewApiResult CallGeneratePreview(const std::vector<uint8_t>& input,
                                     Setup setup) {
  PreviewApiResult result;
  result.xsize = result.ysize = result.downsampling = 7;
  result.stride = result.pixels_size = 7;
  result.format.num_channels = 7;
  result.backend = JXL_PREVIEW_BACKEND_DECODER_DOWNSAMPLE;
  result.color_encoding.color_space = JXL_COLOR_SPACE_XYB;
  uint8_t placeholder = 0;
  uint8_t* pixels = &placeholder;
  JxlPreviewOptions options;
  JxlPreviewOptionsInit(&options);
  options.out_xsize = &result.xsize;
  options.out_ysize = &result.ysize;
  options.out_stride = &result.stride;
  options.out_pixels = &pixels;
  options.out_pixels_size = &result.pixels_size;
  options.out_format = &result.format;
  options.out_backend_used = &result.backend;
  options.out_downsampling = &result.downsampling;
  options.out_color_encoding = &result.color_encoding;
  setup(&options);
  result.status = JxlGeneratePreview(input.data(), input.size(), &options);
  result.pixels_address = pixels;
  result.pixels_null = pixels == nullptr;
  if (pixels != nullptr && pixels != &placeholder) {
    result.pixels.assign(pixels, pixels + result.pixels_size);
    if (options.dst == nullptr) free(pixels);
  }
  return result;
}

// After a failure other than a buffer too small, every output is reset.
void ExpectPreviewOutputsReset(const PreviewApiResult& result) {
  EXPECT_EQ(0u, result.xsize);
  EXPECT_EQ(0u, result.ysize);
  EXPECT_EQ(0u, result.stride);
  EXPECT_EQ(0u, result.pixels_size);
  EXPECT_EQ(0u, result.format.num_channels);
  EXPECT_EQ(JXL_PREVIEW_BACKEND_NONE, result.backend);
  EXPECT_EQ(0u, result.downsampling);
  EXPECT_EQ(JXL_COLOR_SPACE_UNKNOWN, result.color_encoding.color_space);
  EXPECT_TRUE(result.pixels_null);
}

void* FailingAlloc(void* /*opaque*/, size_t /*size*/) { return nullptr; }
void* AllocAtMost256KiB(void* /*opaque*/, size_t size) {
  return size > (256 << 10) ? nullptr : malloc(size);
}
void* SystemAlloc(void* /*opaque*/, size_t size) { return malloc(size); }
void SystemFree(void* /*opaque*/, void* address) { free(address); }

// Inputs that are not JPEG XL, truncated or corrupt, and failed allocations
// get their own status, whether or not the options make the preview API probe
// the header first. They used to come back as internal errors, and a decoder
// that could not be allocated was used anyway.
TEST(DecodeTest, PreviewApiStatusMapping) {
  jxl::TestCodestreamParams params;
  params.cparams.speed_tier = jxl::SpeedTier::kLightning;
  const std::vector<uint8_t> valid =
      CreateDCOnlyTestCodestream(/*xsize=*/300, /*ysize=*/300, 3, params);
  std::vector<uint8_t> corrupt = valid;
  for (size_t i = corrupt.size() / 3; i < corrupt.size(); ++i) {
    corrupt[i] ^= 0x5A;
  }
  const std::vector<std::vector<uint8_t>> bad_inputs = {
      std::vector<uint8_t>(64, 0xAB),  // not JPEG XL
      {0xFF},                          // part of a signature
      std::vector<uint8_t>(valid.begin(), valid.begin() + valid.size() / 2),
      corrupt};
  const JxlColorEncoding srgb = jxl::ColorEncoding::SRGB(false).ToExternal();
  // With the default color encoding, the header is probed first.
  const auto with_probe = [](JxlPreviewOptions* o) {
    o->preview_downsampling = 2;
  };
  const auto without_probe = [&](JxlPreviewOptions* o) {
    o->preview_downsampling = 2;
    o->color_encoding = &srgb;
  };
  for (size_t i = 0; i < bad_inputs.size(); ++i) {
    SCOPED_TRACE(i);
    for (const PreviewApiResult& result :
         {CallGeneratePreview(bad_inputs[i], with_probe),
          CallGeneratePreview(bad_inputs[i], without_probe)}) {
      EXPECT_EQ(JXL_PREVIEW_CORRUPT_INPUT, result.status);
      ExpectPreviewOutputsReset(result);
    }
  }
  JxlPreviewInfo info;
  EXPECT_EQ(JXL_PREVIEW_CORRUPT_INPUT,
            JxlGetPreviewInfo(bad_inputs[0].data(), bad_inputs[0].size(),
                              /*query=*/nullptr, &info));

  // The decoder cannot be allocated.
  JxlMemoryManager failing = {nullptr, &FailingAlloc, &SystemFree};
  for (bool probe : {true, false}) {
    SCOPED_TRACE(probe);
    const PreviewApiResult result =
        CallGeneratePreview(valid, [&](JxlPreviewOptions* o) {
          if (probe) {
            with_probe(o);
          } else {
            without_probe(o);
          }
          o->memory_manager = &failing;
        });
    EXPECT_EQ(JXL_PREVIEW_OUT_OF_MEMORY, result.status);
    ExpectPreviewOutputsReset(result);
  }
  JxlPreviewInfoQuery query = {};
  query.memory_manager = &failing;
  EXPECT_EQ(JXL_PREVIEW_OUT_OF_MEMORY,
            JxlGetPreviewInfo(valid.data(), valid.size(), &query, &info));

  // An allocation inside the decoder fails.
  const std::vector<uint8_t> large =
      CreateDCOnlyTestCodestream(/*xsize=*/1024, /*ysize=*/1024, 3, params);
  JxlMemoryManager limited = {nullptr, &AllocAtMost256KiB, &SystemFree};
  EXPECT_EQ(JXL_PREVIEW_OUT_OF_MEMORY,
            CallGeneratePreview(large, [&](JxlPreviewOptions* o) {
              o->preview_downsampling = 1;
              o->memory_manager = &limited;
            }).status);
}

// Factor 1 returns the first frame of an animation, and decodes no more: a
// broken later frame does not matter, as it does not at other factors.
TEST(DecodeTest, PreviewApiFactorOneStopsAfterFirstFrame) {
  const std::vector<uint8_t> animation =
      CreateAnimationCodestream(/*xsize=*/128, /*ysize=*/128,
                                /*progressive_dc=*/-1);
  // Truncated in the second, last, frame.
  const std::vector<uint8_t> truncated(animation.begin(), animation.end() - 1);
  const auto factor_one = [](JxlPreviewOptions* o) {
    o->preview_downsampling = 1;
  };
  const PreviewApiResult reference = CallGeneratePreview(animation, factor_one);
  ASSERT_EQ(JXL_PREVIEW_SUCCESS, reference.status);
  EXPECT_EQ(JXL_PREVIEW_BACKEND_NONE, reference.backend);
  const PreviewApiResult first_frame =
      CallGeneratePreview(truncated, factor_one);
  ASSERT_EQ(JXL_PREVIEW_SUCCESS, first_frame.status);
  EXPECT_EQ(reference.pixels, first_frame.pixels);
  // A factor chosen from a target size larger than the image.
  const PreviewApiResult from_target =
      CallGeneratePreview(truncated, [](JxlPreviewOptions* o) {
        o->target_xsize = 1000;
        o->target_ysize = 1000;
      });
  EXPECT_EQ(JXL_PREVIEW_SUCCESS, from_target.status);
  EXPECT_EQ(1u, from_target.downsampling);
}

// A buffer that is too small, or rows closer than their size (which depends on
// the source's channels), are reported with the layout a retry needs. A stride
// whose buffer size overflows is too small, not a wild write.
TEST(DecodeTest, PreviewApiBufferTooSmallReportsLayout) {
  jxl::TestCodestreamParams params;
  params.cparams.speed_tier = jxl::SpeedTier::kLightning;
  const std::vector<uint8_t> input =
      CreateDCOnlyTestCodestream(/*xsize=*/100, /*ysize=*/60, 3, params);
  uint8_t tiny[16];
  const PreviewApiResult too_small =
      CallGeneratePreview(input, [&](JxlPreviewOptions* o) {
        o->preview_downsampling = 2;
        o->dst = tiny;
        o->dst_size = sizeof(tiny);
      });
  ASSERT_EQ(JXL_PREVIEW_BUFFER_TOO_SMALL, too_small.status);
  EXPECT_EQ(50u, too_small.xsize);
  EXPECT_EQ(30u, too_small.ysize);
  EXPECT_EQ(3u, too_small.format.num_channels);
  EXPECT_EQ(2u, too_small.downsampling);
  EXPECT_NE(JXL_PREVIEW_BACKEND_NONE, too_small.backend);
  EXPECT_EQ(JXL_COLOR_SPACE_RGB, too_small.color_encoding.color_space);
  EXPECT_EQ(150u, too_small.stride);
  EXPECT_EQ(150u * 30, too_small.pixels_size);
  EXPECT_TRUE(too_small.pixels_null);

  std::vector<uint8_t> buffer(too_small.pixels_size);
  const PreviewApiResult retry =
      CallGeneratePreview(input, [&](JxlPreviewOptions* o) {
        o->preview_downsampling = 2;
        o->dst = buffer.data();
        o->dst_size = buffer.size();
      });
  EXPECT_EQ(JXL_PREVIEW_SUCCESS, retry.status);
  EXPECT_EQ(buffer.data(), retry.pixels_address);
  EXPECT_EQ(150u, retry.stride);
  EXPECT_EQ(buffer.size(), retry.pixels_size);

  // The caller assumed two channels; the source has three.
  const PreviewApiResult narrow_stride =
      CallGeneratePreview(input, [&](JxlPreviewOptions* o) {
        o->preview_downsampling = 2;
        o->dst = buffer.data();
        o->dst_size = buffer.size();
        o->dst_stride = 50 * 2;
      });
  EXPECT_EQ(JXL_PREVIEW_BUFFER_TOO_SMALL, narrow_stride.status);
  EXPECT_EQ(150u, narrow_stride.stride);
  EXPECT_EQ(150u * 30, narrow_stride.pixels_size);

  const PreviewApiResult wide_stride =
      CallGeneratePreview(input, [&](JxlPreviewOptions* o) {
        o->preview_downsampling = 2;
        o->dst = buffer.data();
        o->dst_size = buffer.size();
        o->dst_stride = 160;
      });
  EXPECT_EQ(JXL_PREVIEW_BUFFER_TOO_SMALL, wide_stride.status);
  EXPECT_EQ(160u, wide_stride.stride);
  EXPECT_EQ(29u * 160 + 150, wide_stride.pixels_size);

  // (ysize - 1) * stride wraps around to fewer than 29 bytes, so a check in the
  // multiplication domain would accept the buffer.
  const size_t overflowing_stride = std::numeric_limits<size_t>::max() / 29 + 1;
  const PreviewApiResult overflow =
      CallGeneratePreview(input, [&](JxlPreviewOptions* o) {
        o->preview_downsampling = 2;
        o->dst = buffer.data();
        o->dst_size = buffer.size();
        o->dst_stride = overflowing_stride;
      });
  EXPECT_EQ(JXL_PREVIEW_BUFFER_TOO_SMALL, overflow.status);
  EXPECT_EQ(overflowing_stride, overflow.stride);
  EXPECT_EQ(std::numeric_limits<size_t>::max(), overflow.pixels_size);
}

// A factor 2 preview of `input`, with the inputs `setup` sets, fails with
// `status`.
template <typename Setup>
void ExpectPreviewApiFailure(const std::vector<uint8_t>& input,
                             JxlPreviewStatus status, Setup setup) {
  const PreviewApiResult result =
      CallGeneratePreview(input, [&](JxlPreviewOptions* o) {
        o->preview_downsampling = 2;
        setup(o);
      });
  EXPECT_EQ(status, result.status);
  ExpectPreviewOutputsReset(result);
}

// Invalid arguments are refused before decoding, and the backend mask applies
// to factor 1 too.
TEST(DecodeTest, PreviewApiRejectsInvalidArguments) {
  jxl::TestCodestreamParams params;
  params.cparams.speed_tier = jxl::SpeedTier::kLightning;
  const std::vector<uint8_t> input =
      CreateDCOnlyTestCodestream(/*xsize=*/64, /*ysize=*/48, 3, params);
  JxlColorEncoding zeroed;
  memset(&zeroed, 0, sizeof(zeroed));
  JxlColorEncoding unknown = jxl::ColorEncoding::SRGB(false).ToExternal();
  unknown.color_space = JXL_COLOR_SPACE_UNKNOWN;
  JxlColorEncoding xyb = jxl::ColorEncoding::SRGB(false).ToExternal();
  xyb.color_space = JXL_COLOR_SPACE_XYB;
  JxlColorEncoding nan_gamma = jxl::ColorEncoding::SRGB(false).ToExternal();
  nan_gamma.transfer_function = JXL_TRANSFER_FUNCTION_GAMMA;
  nan_gamma.gamma = std::numeric_limits<double>::quiet_NaN();
  JxlColorEncoding nan_white = jxl::ColorEncoding::SRGB(false).ToExternal();
  nan_white.white_point = JXL_WHITE_POINT_CUSTOM;
  nan_white.white_point_xy[0] = std::numeric_limits<double>::quiet_NaN();
  for (const JxlColorEncoding* c :
       {&zeroed, &unknown, &xyb, &nan_gamma, &nan_white}) {
    ExpectPreviewApiFailure(
        input, JXL_PREVIEW_INVALID_ARGUMENT,
        [&](JxlPreviewOptions* o) { o->color_encoding = c; });
  }
  // A memory manager sets both functions or neither.
  JxlMemoryManager alloc_only = {nullptr, &SystemAlloc, nullptr};
  ExpectPreviewApiFailure(
      input, JXL_PREVIEW_INVALID_ARGUMENT,
      [&](JxlPreviewOptions* o) { o->memory_manager = &alloc_only; });
  JxlPreviewInfoQuery query = {};
  query.memory_manager = &alloc_only;
  JxlPreviewInfo info;
  EXPECT_EQ(JXL_PREVIEW_INVALID_ARGUMENT,
            JxlGetPreviewInfo(input.data(), input.size(), &query, &info));
  for (float nits : {std::numeric_limits<float>::denorm_min(),
                     std::numeric_limits<float>::infinity(),
                     std::numeric_limits<float>::quiet_NaN(), -2.0f}) {
    SCOPED_TRACE(nits);
    ExpectPreviewApiFailure(
        input, JXL_PREVIEW_INVALID_ARGUMENT,
        [&](JxlPreviewOptions* o) { o->display_nits = nits; });
  }
  // Unnamed values inside each enum's range (0-3, 0-7): a value outside it
  // is undefined behaviour in C++ (UBSan -fsanitize=enum).
  const JxlPixelFormat formats[] = {
      {5, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0},
      {3, JXL_TYPE_UINT8, static_cast<JxlEndianness>(3), 0},
      {3, static_cast<JxlDataType>(4), JXL_NATIVE_ENDIAN, 0}};
  for (const JxlPixelFormat& format : formats) {
    ExpectPreviewApiFailure(input, JXL_PREVIEW_UNSUPPORTED_FORMAT,
                            [&](JxlPreviewOptions* o) { o->format = format; });
  }
  // Factor 1 is the full decode, a backend of its own.
  const auto with_mask = [&](uint32_t factor, uint32_t mask) {
    return CallGeneratePreview(input, [&](JxlPreviewOptions* o) {
      o->preview_downsampling = factor;
      o->allowed_backends = mask;
    });
  };
  const PreviewApiResult not_full =
      with_mask(1, JXL_PREVIEW_BACKEND_BIT_EMBEDDED_PREVIEW |
                       JXL_PREVIEW_BACKEND_BIT_DECODER_DOWNSAMPLE);
  EXPECT_EQ(JXL_PREVIEW_NO_BACKEND_AVAILABLE, not_full.status);
  ExpectPreviewOutputsReset(not_full);
  const PreviewApiResult full =
      with_mask(1, JXL_PREVIEW_BACKEND_BIT_FULL_DECODE);
  EXPECT_EQ(JXL_PREVIEW_SUCCESS, full.status);
  EXPECT_EQ(JXL_PREVIEW_BACKEND_NONE, full.backend);
  EXPECT_EQ(64u, full.xsize);
  EXPECT_EQ(JXL_PREVIEW_NO_BACKEND_AVAILABLE,
            with_mask(2, JXL_PREVIEW_BACKEND_BIT_FULL_DECODE).status);
}

// A mask of only the embedded preview is served when the embedded preview is
// the requested image (preview_api_test's CTest inputs have none), and leaves
// no backend at another factor.
TEST(DecodeTest, PreviewApiEmbeddedPreviewOnly) {
  constexpr size_t kPreviewFactor = 4;
  const std::vector<uint8_t> compressed = CreateEmbeddedPreviewCodestream(
      /*xsize=*/400, /*ysize=*/200, /*num_channels=*/3, kPreviewFactor,
      JXL_ORIENT_IDENTITY);
  const auto with_factor = [&](uint32_t factor) {
    return CallGeneratePreview(compressed, [&](JxlPreviewOptions* o) {
      o->preview_downsampling = factor;
      o->allowed_backends = JXL_PREVIEW_BACKEND_BIT_EMBEDDED_PREVIEW;
    });
  };
  const PreviewApiResult embedded = with_factor(kPreviewFactor);
  EXPECT_EQ(JXL_PREVIEW_SUCCESS, embedded.status);
  EXPECT_EQ(JXL_PREVIEW_BACKEND_EMBEDDED_PREVIEW, embedded.backend);
  EXPECT_EQ(kPreviewFactor, embedded.downsampling);
  EXPECT_EQ(100u, embedded.xsize);
  EXPECT_EQ(50u, embedded.ysize);
  const PreviewApiResult other = with_factor(2);
  EXPECT_EQ(JXL_PREVIEW_NO_BACKEND_AVAILABLE, other.status);
  ExpectPreviewOutputsReset(other);
}

// The data color profile is the profile of the pixels the decoder outputs, also
// for images that are not XYB encoded: once an output profile is set, that one.
// It used to stay the original profile for them.
TEST(DecodeTest, DataColorProfileFollowsOutputProfileWithoutXyb) {
  jxl::TestCodestreamParams params;
  params.cparams.SetLossless();
  params.cparams.speed_tier = jxl::SpeedTier::kThunder;
  params.color_space = "RGB_D65_SRG_Rel_Lin";
  const std::vector<uint8_t> compressed =
      CreateDCOnlyTestCodestream(/*xsize=*/64, /*ysize=*/48, 3, params);
  JxlDecoderPtr dec = JxlDecoderMake(nullptr);
  ASSERT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSubscribeEvents(
                dec.get(), JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING));
  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetInput(dec.get(), compressed.data(),
                                                compressed.size()));
  ASSERT_EQ(JXL_DEC_BASIC_INFO, JxlDecoderProcessInput(dec.get()));
  JxlBasicInfo info;
  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetBasicInfo(dec.get(), &info));
  ASSERT_TRUE(info.uses_original_profile);
  ASSERT_EQ(JXL_DEC_COLOR_ENCODING, JxlDecoderProcessInput(dec.get()));
  const auto encoding = [&](JxlColorProfileTarget target) {
    JxlColorEncoding c = {};
    EXPECT_EQ(JXL_DEC_SUCCESS,
              JxlDecoderGetColorAsEncodedProfile(dec.get(), target, &c));
    return c;
  };
  EXPECT_EQ(JXL_TRANSFER_FUNCTION_LINEAR,
            encoding(JXL_COLOR_PROFILE_TARGET_DATA).transfer_function);
  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderSetCms(dec.get(), *JxlGetDefaultCms()));
  const JxlColorEncoding srgb = jxl::ColorEncoding::SRGB(false).ToExternal();
  ASSERT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderSetOutputColorProfile(dec.get(), &srgb, nullptr, 0));
  EXPECT_EQ(JXL_TRANSFER_FUNCTION_SRGB,
            encoding(JXL_COLOR_PROFILE_TARGET_DATA).transfer_function);
  EXPECT_EQ(JXL_TRANSFER_FUNCTION_LINEAR,
            encoding(JXL_COLOR_PROFILE_TARGET_ORIGINAL).transfer_function);
  // The ICC profile describes the same pixels.
  size_t data_icc_size = 0;
  size_t original_icc_size = 0;
  ASSERT_EQ(JXL_DEC_SUCCESS,
            JxlDecoderGetICCProfileSize(
                dec.get(), JXL_COLOR_PROFILE_TARGET_DATA, &data_icc_size));
  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetICCProfileSize(
                                 dec.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL,
                                 &original_icc_size));
  std::vector<uint8_t> data_icc(data_icc_size);
  std::vector<uint8_t> original_icc(original_icc_size);
  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetColorAsICCProfile(
                                 dec.get(), JXL_COLOR_PROFILE_TARGET_DATA,
                                 data_icc.data(), data_icc.size()));
  ASSERT_EQ(JXL_DEC_SUCCESS, JxlDecoderGetColorAsICCProfile(
                                 dec.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL,
                                 original_icc.data(), original_icc.size()));
  EXPECT_EQ(jxl::ColorEncoding::SRGB(false).ICC(), data_icc);
  EXPECT_NE(original_icc, data_icc);
}

// The color encoding of the output pixels is reported. Without tone mapping and
// a requested encoding, a source described only by an ICC profile is output in
// that profile (color space unknown), unless it is XYB encoded: the decoder
// then outputs linear sRGB.
TEST(DecodeTest, PreviewApiOutputColorEncoding) {
  for (bool lossless : {true, false}) {
    SCOPED_TRACE(lossless);
    jxl::TestCodestreamParams params;
    params.cparams.speed_tier = jxl::SpeedTier::kLightning;
    if (lossless) params.cparams.SetLossless();
    params.add_icc_profile = true;
    const std::vector<uint8_t> input =
        CreateDCOnlyTestCodestream(/*xsize=*/64, /*ysize=*/48, 3, params);
    const PreviewApiResult untouched =
        CallGeneratePreview(input, [](JxlPreviewOptions* o) {
          o->preview_downsampling = 2;
          o->display_nits = JXL_PREVIEW_NO_TONE_MAPPING;
        });
    ASSERT_EQ(JXL_PREVIEW_SUCCESS, untouched.status);
    if (lossless) {
      EXPECT_EQ(JXL_COLOR_SPACE_UNKNOWN, untouched.color_encoding.color_space);
      uint8_t* icc = nullptr;
      size_t icc_size = 0;
      JxlPreviewInfoQuery query = {};
      query.out_icc = &icc;
      query.out_icc_size = &icc_size;
      JxlPreviewInfo info;
      ASSERT_EQ(JXL_PREVIEW_SUCCESS,
                JxlGetPreviewInfo(input.data(), input.size(), &query, &info));
      EXPECT_EQ(jxl::test::GetIccTestProfile().size(), icc_size);
      free(icc);
    } else {
      EXPECT_EQ(JXL_COLOR_SPACE_RGB, untouched.color_encoding.color_space);
      EXPECT_EQ(JXL_TRANSFER_FUNCTION_LINEAR,
                untouched.color_encoding.transfer_function);
    }
    const PreviewApiResult by_default = CallGeneratePreview(
        input, [](JxlPreviewOptions* o) { o->preview_downsampling = 2; });
    ASSERT_EQ(JXL_PREVIEW_SUCCESS, by_default.status);
    EXPECT_EQ(JXL_COLOR_SPACE_RGB, by_default.color_encoding.color_space);
    EXPECT_EQ(JXL_TRANSFER_FUNCTION_SRGB,
              by_default.color_encoding.transfer_function);
  }
}
