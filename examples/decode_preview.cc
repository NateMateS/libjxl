// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

// This C++ example decodes a thumbnail of a JPEG XL image: the first frame,
// downsampled by the decoder with JxlDecoderSetImageOutDownsampling, as 8-bit
// sRGB with alpha. The factor (1, 2, 4 or 8) is the largest that keeps the
// shorter edge of the thumbnail at least `min_edge` pixels long.
//
// Decoder-side downsampling costs less than decoding the full image and
// downsampling it: the decoder may render at reduced resolution and skip data
// that does not contribute at that scale. At a factor of 8, a lossy (VarDCT)
// frame can be rendered from its 1/8 resolution DC image alone, so most of the
// file is never decoded.
//
// Usage: decode_preview <min_edge> <in.jxl> <out.pam>

#include <jxl/codestream_header.h>
#include <jxl/color_encoding.h>
#include <jxl/decode.h>
#include <jxl/decode_cxx.h>
#include <jxl/resizable_parallel_runner.h>
#include <jxl/resizable_parallel_runner_cxx.h>
#include <jxl/types.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <vector>

/** Returns the largest factor in {8, 4, 2, 1} that keeps the shorter edge of
 * the downsampled image at least `min_edge` pixels long.
 */
uint32_t ThumbnailFactor(uint32_t xsize, uint32_t ysize, uint32_t min_edge) {
  const uint32_t shorter_edge = xsize < ysize ? xsize : ysize;
  for (uint32_t factor : {8u, 4u, 2u}) {
    if ((shorter_edge + factor - 1) / factor >= min_edge) return factor;
  }
  return 1;
}

/** Decodes a thumbnail of the first frame of a JPEG XL image into 8-bit RGBA
 * pixels, interleaved, line per line from top to bottom.
 */
bool DecodeJpegXlThumbnail(const uint8_t* jxl, size_t size, uint32_t min_edge,
                           std::vector<uint8_t>* pixels, size_t* xsize,
                           size_t* ysize) {
  // Multi-threaded parallel runner.
  JxlResizableParallelRunnerPtr runner =
      JxlResizableParallelRunnerMake(nullptr);

  JxlDecoderPtr dec = JxlDecoderMake(nullptr);
  if (JXL_DEC_SUCCESS !=
      JxlDecoderSubscribeEvents(dec.get(),
                                JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING |
                                    JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE)) {
    fprintf(stderr, "JxlDecoderSubscribeEvents failed\n");
    return false;
  }

  if (JXL_DEC_SUCCESS != JxlDecoderSetParallelRunner(dec.get(),
                                                     JxlResizableParallelRunner,
                                                     runner.get())) {
    fprintf(stderr, "JxlDecoderSetParallelRunner failed\n");
    return false;
  }

  JxlBasicInfo info;
  JxlPixelFormat format = {4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
  uint32_t factor = 1;

  JxlDecoderSetInput(dec.get(), jxl, size);
  JxlDecoderCloseInput(dec.get());

  for (;;) {
    JxlDecoderStatus status = JxlDecoderProcessInput(dec.get());

    if (status == JXL_DEC_ERROR) {
      fprintf(stderr, "Decoder error\n");
      return false;
    } else if (status == JXL_DEC_NEED_MORE_INPUT) {
      fprintf(stderr, "Error, already provided all input\n");
      return false;
    } else if (status == JXL_DEC_BASIC_INFO) {
      if (JXL_DEC_SUCCESS != JxlDecoderGetBasicInfo(dec.get(), &info)) {
        fprintf(stderr, "JxlDecoderGetBasicInfo failed\n");
        return false;
      }
      // xsize and ysize are those of the displayed image, after orientation.
      factor = ThumbnailFactor(info.xsize, info.ysize, min_edge);
      *xsize = (info.xsize + factor - 1) / factor;
      *ysize = (info.ysize + factor - 1) / factor;
      JxlResizableParallelRunnerSetThreads(
          runner.get(),
          JxlResizableParallelRunnerSuggestThreads(info.xsize, info.ysize));
      // Tone map HDR images to an SDR display.
      if (info.intensity_target > 255.0f &&
          JXL_DEC_SUCCESS !=
              JxlDecoderSetDesiredIntensityTarget(dec.get(), 255.0f)) {
        fprintf(stderr, "JxlDecoderSetDesiredIntensityTarget failed\n");
        return false;
      }
    } else if (status == JXL_DEC_COLOR_ENCODING) {
      // Ask for sRGB output. Without a color management system, the decoder
      // can convert to it only from XYB, the color space of most lossy images;
      // other images are output in their own color space, described by
      // JxlDecoderGetColorAsEncodedProfile or JxlDecoderGetColorAsICCProfile.
      if (!info.uses_original_profile) {
        JxlColorEncoding srgb = {};
        srgb.color_space = JXL_COLOR_SPACE_RGB;
        srgb.white_point = JXL_WHITE_POINT_D65;
        srgb.primaries = JXL_PRIMARIES_SRGB;
        srgb.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
        srgb.rendering_intent = JXL_RENDERING_INTENT_RELATIVE;
        if (JXL_DEC_SUCCESS !=
            JxlDecoderSetPreferredColorProfile(dec.get(), &srgb)) {
          fprintf(stderr, "JxlDecoderSetPreferredColorProfile failed\n");
          return false;
        }
      }
    } else if (status == JXL_DEC_FRAME) {
      // Set once the frame header is known, before the output buffer.
      if (JXL_DEC_SUCCESS !=
          JxlDecoderSetImageOutDownsampling(dec.get(), factor)) {
        fprintf(stderr, "JxlDecoderSetImageOutDownsampling failed\n");
        return false;
      }
    } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
      // The buffer size follows the output downsampling.
      size_t buffer_size;
      if (JXL_DEC_SUCCESS !=
          JxlDecoderImageOutBufferSize(dec.get(), &format, &buffer_size)) {
        fprintf(stderr, "JxlDecoderImageOutBufferSize failed\n");
        return false;
      }
      if (buffer_size != *xsize * *ysize * 4) {
        fprintf(stderr, "Invalid out buffer size %d %d\n",
                static_cast<int>(buffer_size),
                static_cast<int>(*xsize * *ysize * 4));
        return false;
      }
      pixels->resize(buffer_size);
      if (JXL_DEC_SUCCESS != JxlDecoderSetImageOutBuffer(dec.get(), &format,
                                                         pixels->data(),
                                                         pixels->size())) {
        fprintf(stderr, "JxlDecoderSetImageOutBuffer failed\n");
        return false;
      }
    } else if (status == JXL_DEC_FULL_IMAGE) {
      // The first frame is enough for a thumbnail, also of an animation.
      return true;
    } else if (status == JXL_DEC_SUCCESS) {
      fprintf(stderr, "No frame decoded\n");
      return false;
    } else {
      fprintf(stderr, "Unknown decoder status\n");
      return false;
    }
  }
}

/** Writes 8-bit RGBA pixels as a PAM file.
 */
bool WritePAM(const char* filename, const uint8_t* pixels, size_t xsize,
              size_t ysize) {
  FILE* file = fopen(filename, "wb");
  if (!file) {
    fprintf(stderr, "Could not open %s for writing", filename);
    return false;
  }
  fprintf(file,
          "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE "
          "RGB_ALPHA\nENDHDR\n",
          static_cast<int>(xsize), static_cast<int>(ysize));
  const size_t size = xsize * ysize * 4;
  const bool ok = fwrite(pixels, 1, size, file) == size;
  if (fclose(file) != 0 || !ok) {
    fprintf(stderr, "Could not write %s", filename);
    return false;
  }
  return true;
}

bool LoadFile(const char* filename, std::vector<uint8_t>* out) {
  FILE* file = fopen(filename, "rb");
  if (!file) {
    return false;
  }

  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return false;
  }

  long size = ftell(file);  // NOLINT
  // Avoid invalid file or directory.
  if (size >= LONG_MAX || size < 0) {
    fclose(file);
    return false;
  }

  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return false;
  }

  out->resize(size);
  size_t readsize = fread(out->data(), 1, size, file);
  if (fclose(file) != 0) {
    return false;
  }

  return readsize == static_cast<size_t>(size);
}

int main(int argc, char* argv[]) {
  if (argc != 4) {
    fprintf(stderr,
            "Usage: %s <min_edge> <in.jxl> <out.pam>\n"
            "Where:\n"
            "  min_edge = minimum length of the shorter thumbnail edge\n"
            "  in.jxl = input JPEG XL image filename\n"
            "  out.pam = output thumbnail filename\n"
            "Output files will be overwritten.\n",
            argv[0]);
    return 1;
  }

  const int min_edge = atoi(argv[1]);
  const char* jxl_filename = argv[2];
  const char* pam_filename = argv[3];
  if (min_edge <= 0) {
    fprintf(stderr, "min_edge must be positive\n");
    return 1;
  }

  std::vector<uint8_t> jxl;
  if (!LoadFile(jxl_filename, &jxl)) {
    fprintf(stderr, "couldn't load %s\n", jxl_filename);
    return 1;
  }

  std::vector<uint8_t> pixels;
  size_t xsize = 0;
  size_t ysize = 0;
  if (!DecodeJpegXlThumbnail(jxl.data(), jxl.size(),
                             static_cast<uint32_t>(min_edge), &pixels, &xsize,
                             &ysize)) {
    fprintf(stderr, "Error while decoding the jxl file\n");
    return 1;
  }
  if (!WritePAM(pam_filename, pixels.data(), xsize, ysize)) {
    fprintf(stderr, "Error while writing the PAM image file\n");
    return 1;
  }
  printf("Successfully wrote %s (%dx%d)\n", pam_filename,
         static_cast<int>(xsize), static_cast<int>(ysize));
  return 0;
}
