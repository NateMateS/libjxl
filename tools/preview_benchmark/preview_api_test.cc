// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

// Standalone exerciser for the one-shot preview API in lib/extras/preview.h.
// Each test case targets one option of JxlPreviewOptions / JxlGetPreviewInfo
// and prints PASS/FAIL. Exits with non-zero status on any failure.
//
// Usage:
//   preview_api_test <path-to-some.jxl> [<path-to-hdr.jxl>]
//
// The first argument should be any small JXL; the second (optional) should
// be an HDR file (e.g. PQ/HLG) to verify display_nits behavior. If absent,
// HDR-related cases are skipped.

#include <jxl/color_encoding.h>
#include <jxl/decode.h>
#include <jxl/memory_manager.h>
#include <jxl/thread_parallel_runner.h>
#include <jxl/thread_parallel_runner_cxx.h>
#include <jxl/types.h>

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "lib/extras/preview.h"
#include "lib/jxl/base/printf_macros.h"
#include "lib/jxl/dec_preview_internal.h"

namespace {

int g_pass = 0;
int g_fail = 0;
int g_skip = 0;

#define PREVIEW_EXPECT(cond)                                               \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "    [assert] %s:%d: %s\n", __FILE__, __LINE__, \
                   #cond);                                                 \
      return false;                                                        \
    }                                                                      \
  } while (0)

#define PREVIEW_EXPECT_EQ(a, b)                                             \
  do {                                                                      \
    auto _va = (a);                                                         \
    auto _vb = (b);                                                         \
    if (!(_va == _vb)) {                                                    \
      std::fprintf(stderr, "    [assert] %s:%d: %s == %s (%lld vs %lld)\n", \
                   __FILE__, __LINE__, #a, #b, static_cast<long long>(_va), \
                   static_cast<long long>(_vb));                            \
      return false;                                                         \
    }                                                                       \
  } while (0)

void RunCase(const char* name, bool (*fn)()) {
  std::printf("  [ .. ] %s\n", name);
  std::fflush(stdout);
  bool ok = fn();
  if (ok) {
    std::printf("  [ OK ] %s\n", name);
    ++g_pass;
  } else {
    std::printf("  [FAIL] %s\n", name);
    ++g_fail;
  }
  std::fflush(stdout);
}

bool ReadFile(const std::string& path, std::vector<uint8_t>* out) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return false;
  std::streamsize size = f.tellg();
  if (size < 0) return false;
  f.seekg(0);
  out->resize(static_cast<size_t>(size));
  if (size == 0) return true;
  return static_cast<bool>(f.read(reinterpret_cast<char*>(out->data()), size));
}

// --- Custom memory manager that counts allocs/frees -------------------------
//
// Sizes are stashed in a `void* -> size_t` map keyed by the address we
// returned to the library, rather than in a header just before the pointer.
// This means an "untracked free" (a foreign pointer the library passes us by
// mistake) is detected by a clean map miss instead of an unsafe read of
// memory that may not belong to us. We abort hard on that case so the stack
// trace points at the offending free site, in all build modes.

struct CountingMM {
  std::mutex mu;
  std::unordered_map<void*, size_t> live;
  std::atomic<int64_t> alloc_count{0};
  std::atomic<int64_t> free_count{0};
  std::atomic<int64_t> bytes_outstanding{0};
};

void* CountingAlloc(void* opaque, size_t size) {
  auto* mm = static_cast<CountingMM*>(opaque);
  void* p = std::malloc(size);
  if (p == nullptr) return nullptr;
  {
    std::lock_guard<std::mutex> lk(mm->mu);
    mm->live.emplace(p, size);
  }
  mm->alloc_count.fetch_add(1);
  mm->bytes_outstanding.fetch_add(static_cast<int64_t>(size));
  return p;
}

void CountingFree(void* opaque, void* address) {
  if (address == nullptr) return;
  auto* mm = static_cast<CountingMM*>(opaque);
  size_t sz = 0;
  {
    std::lock_guard<std::mutex> lk(mm->mu);
    auto it = mm->live.find(address);
    if (it == mm->live.end()) {
      std::fprintf(stderr,
                   "CountingFree: untracked free of %p (library passed a "
                   "pointer not allocated through this memory_manager)\n",
                   address);
      std::abort();
    }
    sz = it->second;
    mm->live.erase(it);
  }
  mm->free_count.fetch_add(1);
  mm->bytes_outstanding.fetch_sub(static_cast<int64_t>(sz));
  std::free(address);
}

// --- Test corpus ------------------------------------------------------------

std::vector<uint8_t> g_input;      // generic SDR JXL
std::vector<uint8_t> g_input_hdr;  // HDR JXL (optional)
bool g_have_hdr = false;

// --- Cases ------------------------------------------------------------------

bool Case_OptionsInit_Zeroes() {
  JxlPreviewOptions o;
  std::memset(&o, 0xAA, sizeof(o));
  JxlPreviewOptionsInit(&o);
  // Spot-check a few fields.
  PREVIEW_EXPECT_EQ(o.preview_downsampling, 0u);
  PREVIEW_EXPECT_EQ(o.target_xsize, 0u);
  PREVIEW_EXPECT(o.dst == nullptr);
  PREVIEW_EXPECT(o.runner == nullptr);
  PREVIEW_EXPECT(o.cancel == nullptr);
  PREVIEW_EXPECT(o.out_pixels == nullptr);
  PREVIEW_EXPECT(o.out_color_encoding == nullptr);
  return true;
}

bool Case_GetInfo_Basic() {
  JxlPreviewInfo info;
  JxlPreviewStatus st =
      JxlGetPreviewInfo(g_input.data(), g_input.size(), nullptr, &info);
  PREVIEW_EXPECT_EQ(st, JXL_PREVIEW_SUCCESS);
  PREVIEW_EXPECT(info.xsize > 0);
  PREVIEW_EXPECT(info.ysize > 0);
  PREVIEW_EXPECT(info.num_color_channels == 1 || info.num_color_channels == 3);
  PREVIEW_EXPECT_EQ(info.recommended_factor, 1u);  // no target -> factor 1
  return true;
}

bool Case_GetInfo_NullArgs() {
  JxlPreviewInfo info;
  PREVIEW_EXPECT_EQ(JxlGetPreviewInfo(nullptr, 1, nullptr, &info),
                    JXL_PREVIEW_INVALID_ARGUMENT);
  PREVIEW_EXPECT_EQ(JxlGetPreviewInfo(g_input.data(), 0, nullptr, &info),
                    JXL_PREVIEW_INVALID_ARGUMENT);
  PREVIEW_EXPECT_EQ(
      JxlGetPreviewInfo(g_input.data(), g_input.size(), nullptr, nullptr),
      JXL_PREVIEW_INVALID_ARGUMENT);
  return true;
}

bool Case_GetInfo_CorruptInput() {
  std::vector<uint8_t> garbage(64, 0xAB);
  JxlPreviewInfo info;
  PREVIEW_EXPECT_EQ(
      JxlGetPreviewInfo(garbage.data(), garbage.size(), nullptr, &info),
      JXL_PREVIEW_CORRUPT_INPUT);
  return true;
}

bool Case_GetInfo_RecommendedFactor() {
  JxlPreviewInfo full;
  JxlPreviewStatus st =
      JxlGetPreviewInfo(g_input.data(), g_input.size(), nullptr, &full);
  PREVIEW_EXPECT_EQ(st, JXL_PREVIEW_SUCCESS);
  // Ask for 1/4 size: factor should land on 4 (or 8 if image is small).
  JxlPreviewInfoQuery q = {};
  q.target_xsize = full.xsize / 4;
  q.target_ysize = full.ysize / 4;
  JxlPreviewInfo info;
  st = JxlGetPreviewInfo(g_input.data(), g_input.size(), &q, &info);
  PREVIEW_EXPECT_EQ(st, JXL_PREVIEW_SUCCESS);
  PREVIEW_EXPECT(info.recommended_factor == 4u ||
                 info.recommended_factor == 8u);
  PREVIEW_EXPECT(info.recommended_xsize >= q.target_xsize);
  PREVIEW_EXPECT(info.recommended_ysize >= q.target_ysize);
  return true;
}

bool Case_GetInfo_ICC() {
  JxlPreviewInfoQuery q = {};
  uint8_t* icc = nullptr;
  size_t icc_size = 0;
  q.out_icc = &icc;
  q.out_icc_size = &icc_size;
  JxlPreviewInfo info;
  JxlPreviewStatus st =
      JxlGetPreviewInfo(g_input.data(), g_input.size(), &q, &info);
  PREVIEW_EXPECT_EQ(st, JXL_PREVIEW_SUCCESS);
  PREVIEW_EXPECT(icc != nullptr);
  PREVIEW_EXPECT(icc_size > 4);
  // Typical ICC starts with 4-byte big-endian profile size.
  std::free(icc);
  return true;
}

bool Case_GetInfo_ICC_MissingSizePtr() {
  JxlPreviewInfoQuery q = {};
  uint8_t* icc = nullptr;
  q.out_icc = &icc;
  q.out_icc_size = nullptr;  // illegal when out_icc != nullptr
  JxlPreviewInfo info;
  PREVIEW_EXPECT_EQ(
      JxlGetPreviewInfo(g_input.data(), g_input.size(), &q, &info),
      JXL_PREVIEW_INVALID_ARGUMENT);
  return true;
}

bool Case_Generate_Defaults_Allocates() {
  JxlPreviewOptions o;
  JxlPreviewOptionsInit(&o);
  uint32_t xs = 0, ys = 0;
  uint8_t* px = nullptr;
  size_t px_size = 0;
  size_t stride = 0;
  JxlPixelFormat actual = {};
  JxlPreviewBackend backend = JXL_PREVIEW_BACKEND_NONE;
  o.out_xsize = &xs;
  o.out_ysize = &ys;
  o.out_pixels = &px;
  o.out_pixels_size = &px_size;
  o.out_stride = &stride;
  o.out_format = &actual;
  o.out_backend_used = &backend;
  o.preview_downsampling = 4;

  JxlPreviewStatus st = JxlGeneratePreview(g_input.data(), g_input.size(), &o);
  PREVIEW_EXPECT_EQ(st, JXL_PREVIEW_SUCCESS);
  PREVIEW_EXPECT(xs > 0);
  PREVIEW_EXPECT(ys > 0);
  PREVIEW_EXPECT(px != nullptr);
  PREVIEW_EXPECT_EQ(actual.data_type, JXL_TYPE_UINT8);
  PREVIEW_EXPECT(actual.num_channels >= 1 && actual.num_channels <= 4);
  PREVIEW_EXPECT_EQ(stride, static_cast<size_t>(xs) * actual.num_channels);
  PREVIEW_EXPECT_EQ(px_size, static_cast<size_t>(ys) * stride);
  std::free(px);
  return true;
}

bool Case_Generate_NullArgs() {
  PREVIEW_EXPECT_EQ(JxlGeneratePreview(nullptr, 1, nullptr),
                    JXL_PREVIEW_INVALID_ARGUMENT);
  JxlPreviewOptions o;
  JxlPreviewOptionsInit(&o);
  PREVIEW_EXPECT_EQ(
      JxlGeneratePreview(g_input.data(), g_input.size(), &o),
      JXL_PREVIEW_INVALID_ARGUMENT);  // missing out_* required pointers
  return true;
}

bool Case_Generate_BadFactor() {
  JxlPreviewOptions o;
  JxlPreviewOptionsInit(&o);
  uint32_t xs, ys;
  uint8_t* px = nullptr;
  o.out_xsize = &xs;
  o.out_ysize = &ys;
  o.out_pixels = &px;
  o.preview_downsampling = 3;  // not in {0,1,2,4,8}
  PREVIEW_EXPECT_EQ(JxlGeneratePreview(g_input.data(), g_input.size(), &o),
                    JXL_PREVIEW_INVALID_ARGUMENT);
  return true;
}

bool Case_Generate_CallerBuffer_TightStride() {
  JxlPreviewInfo info;
  JxlGetPreviewInfo(g_input.data(), g_input.size(), nullptr, &info);
  const uint32_t factor = 4;
  const uint32_t xs_exp = (info.xsize + factor - 1) / factor;
  const uint32_t ys_exp = (info.ysize + factor - 1) / factor;
  // Allocate enough for the worst case (4 channels). The actual format may
  // produce fewer channels; we verify via out_format.
  std::vector<uint8_t> buf(static_cast<size_t>(xs_exp) * ys_exp * 4u, 0xCC);

  JxlPreviewOptions o;
  JxlPreviewOptionsInit(&o);
  uint32_t xs, ys;
  uint8_t* px = nullptr;
  size_t stride = 0;
  JxlPixelFormat actual = {};
  o.out_xsize = &xs;
  o.out_ysize = &ys;
  o.out_pixels = &px;
  o.out_stride = &stride;
  o.out_format = &actual;
  o.preview_downsampling = factor;
  o.dst = buf.data();
  o.dst_size = buf.size();

  PREVIEW_EXPECT_EQ(JxlGeneratePreview(g_input.data(), g_input.size(), &o),
                    JXL_PREVIEW_SUCCESS);
  PREVIEW_EXPECT_EQ(xs, xs_exp);
  PREVIEW_EXPECT_EQ(ys, ys_exp);
  PREVIEW_EXPECT(px == buf.data());
  PREVIEW_EXPECT_EQ(stride, static_cast<size_t>(xs_exp) * actual.num_channels);
  return true;
}

bool Case_Generate_CallerBuffer_PaddedStride() {
  JxlPreviewInfo info;
  JxlGetPreviewInfo(g_input.data(), g_input.size(), nullptr, &info);
  const uint32_t factor = 4;
  const uint32_t xs_exp = (info.xsize + factor - 1) / factor;
  const uint32_t ys_exp = (info.ysize + factor - 1) / factor;
  // Conservative worst-case (4 channels) row size for stride math.
  const size_t row_bytes_max = static_cast<size_t>(xs_exp) * 4u;
  const size_t padded_stride = ((row_bytes_max + 63) / 64) * 64 + 128;
  std::vector<uint8_t> buf(padded_stride * ys_exp, 0xCC);

  JxlPreviewOptions o;
  JxlPreviewOptionsInit(&o);
  uint32_t xs, ys;
  uint8_t* px = nullptr;
  size_t stride = 0;
  JxlPixelFormat actual = {};
  o.out_xsize = &xs;
  o.out_ysize = &ys;
  o.out_pixels = &px;
  o.out_stride = &stride;
  o.out_format = &actual;
  o.preview_downsampling = factor;
  o.dst = buf.data();
  o.dst_size = buf.size();
  o.dst_stride = padded_stride;

  PREVIEW_EXPECT_EQ(JxlGeneratePreview(g_input.data(), g_input.size(), &o),
                    JXL_PREVIEW_SUCCESS);
  PREVIEW_EXPECT_EQ(stride, padded_stride);
  const size_t actual_row_bytes =
      static_cast<size_t>(xs_exp) * actual.num_channels;
  // Verify padding bytes between rows are untouched.
  for (uint32_t y = 0; y < ys_exp; ++y) {
    for (size_t b = actual_row_bytes; b < padded_stride; ++b) {
      if (buf[y * padded_stride + b] != 0xCC) {
        std::fprintf(stderr,
                     "    padding clobbered at row=%u byte=%" PRIuS
                     " (got 0x%02X)\n",
                     y, b, buf[y * padded_stride + b]);
        return false;
      }
    }
  }
  return true;
}

bool Case_Generate_BufferTooSmall() {
  JxlPreviewOptions o;
  JxlPreviewOptionsInit(&o);
  uint32_t xs, ys;
  uint8_t* px = nullptr;
  o.out_xsize = &xs;
  o.out_ysize = &ys;
  o.out_pixels = &px;
  o.preview_downsampling = 4;
  uint8_t tiny[16];
  o.dst = tiny;
  o.dst_size = sizeof(tiny);
  PREVIEW_EXPECT_EQ(JxlGeneratePreview(g_input.data(), g_input.size(), &o),
                    JXL_PREVIEW_BUFFER_TOO_SMALL);
  return true;
}

bool Case_Generate_TargetSize_AutoFactor() {
  JxlPreviewInfo info;
  JxlGetPreviewInfo(g_input.data(), g_input.size(), nullptr, &info);

  JxlPreviewOptions o;
  JxlPreviewOptionsInit(&o);
  uint32_t xs, ys;
  uint8_t* px = nullptr;
  o.out_xsize = &xs;
  o.out_ysize = &ys;
  o.out_pixels = &px;
  o.target_xsize = info.xsize / 4;
  o.target_ysize = info.ysize / 4;
  // preview_downsampling left at 0 -> auto

  PREVIEW_EXPECT_EQ(JxlGeneratePreview(g_input.data(), g_input.size(), &o),
                    JXL_PREVIEW_SUCCESS);
  PREVIEW_EXPECT(xs >= o.target_xsize);
  PREVIEW_EXPECT(ys >= o.target_ysize);
  std::free(px);
  return true;
}

bool Case_Generate_MemoryManager() {
  CountingMM mm_state;
  JxlMemoryManager mm{&mm_state, &CountingAlloc, &CountingFree};

  JxlPreviewOptions o;
  JxlPreviewOptionsInit(&o);
  uint32_t xs, ys;
  uint8_t* px = nullptr;
  size_t px_size = 0;
  o.out_xsize = &xs;
  o.out_ysize = &ys;
  o.out_pixels = &px;
  o.out_pixels_size = &px_size;
  o.preview_downsampling = 4;
  o.memory_manager = &mm;

  PREVIEW_EXPECT_EQ(JxlGeneratePreview(g_input.data(), g_input.size(), &o),
                    JXL_PREVIEW_SUCCESS);
  PREVIEW_EXPECT(px != nullptr);
  PREVIEW_EXPECT(mm_state.alloc_count.load() > 0);
  // Output buffer must come from mm.
  CountingFree(&mm_state, px);
  PREVIEW_EXPECT_EQ(mm_state.bytes_outstanding.load(), 0);
  return true;
}

bool Case_Generate_ParallelRunner() {
  auto runner = JxlThreadParallelRunnerMake(nullptr, 4);
  JxlPreviewOptions o;
  JxlPreviewOptionsInit(&o);
  uint32_t xs, ys;
  uint8_t* px = nullptr;
  o.out_xsize = &xs;
  o.out_ysize = &ys;
  o.out_pixels = &px;
  o.preview_downsampling = 2;
  o.runner = JxlThreadParallelRunner;
  o.runner_opaque = runner.get();

  PREVIEW_EXPECT_EQ(JxlGeneratePreview(g_input.data(), g_input.size(), &o),
                    JXL_PREVIEW_SUCCESS);
  PREVIEW_EXPECT(px != nullptr);
  std::free(px);
  return true;
}

bool Case_Generate_Cancel_Immediate() {
  // Pre-set cancel; decode should bail immediately with CANCELLED.
  int cancel_flag = 1;
  JxlPreviewOptions o;
  JxlPreviewOptionsInit(&o);
  uint32_t xs, ys;
  uint8_t* px = nullptr;
  o.out_xsize = &xs;
  o.out_ysize = &ys;
  o.out_pixels = &px;
  o.preview_downsampling = 1;
  o.cancel = &cancel_flag;
  PREVIEW_EXPECT_EQ(JxlGeneratePreview(g_input.data(), g_input.size(), &o),
                    JXL_PREVIEW_CANCELLED);
  PREVIEW_EXPECT(px == nullptr);
  return true;
}

// Reads the basic info of `data` with the public decoder API.
bool ProbeBasicInfo(const std::vector<uint8_t>& data, JxlBasicInfo* info) {
  JxlDecoder* dec = JxlDecoderCreate(nullptr);
  if (dec == nullptr) return false;
  bool ok =
      JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO) == JXL_DEC_SUCCESS &&
      JxlDecoderSetInput(dec, data.data(), data.size()) == JXL_DEC_SUCCESS;
  if (ok) {
    JxlDecoderCloseInput(dec);
    ok = JxlDecoderProcessInput(dec) == JXL_DEC_BASIC_INFO &&
         JxlDecoderGetBasicInfo(dec, info) == JXL_DEC_SUCCESS;
  }
  JxlDecoderDestroy(dec);
  return ok;
}

// Generates a preview of g_input at `factor` with only `allowed_backends`.
JxlPreviewStatus GenerateWithBackends(uint32_t factor,
                                      uint32_t allowed_backends,
                                      JxlPreviewBackend* backend) {
  JxlPreviewOptions o;
  JxlPreviewOptionsInit(&o);
  uint32_t xs = 0;
  uint32_t ys = 0;
  uint8_t* px = nullptr;
  *backend = JXL_PREVIEW_BACKEND_NONE;
  o.out_xsize = &xs;
  o.out_ysize = &ys;
  o.out_pixels = &px;
  o.out_backend_used = backend;
  o.preview_downsampling = factor;
  o.allowed_backends = allowed_backends;
  const JxlPreviewStatus st =
      JxlGeneratePreview(g_input.data(), g_input.size(), &o);
  std::free(px);
  return st;
}

bool Case_Generate_AllowedBackends_Restrict() {
  // Allow only EMBEDDED_PREVIEW. The decoder returns an embedded preview frame
  // when it is exactly the requested preview, the image divided by the factor
  // (rounded up), and the image has no extra channels besides alpha (as
  // lib/extras/dec/jxl.cc decides); otherwise the restriction leaves no
  // backend. The CTest inputs have no embedded preview; decode_test's
  // PreviewApiEmbeddedPreviewOnly covers the success branch.
  const uint32_t factor = 8;
  JxlBasicInfo info;
  PREVIEW_EXPECT(ProbeBasicInfo(g_input, &info));
  const uint32_t other_extra_channels =
      info.num_extra_channels - (info.alpha_bits > 0 ? 1 : 0);
  const bool embedded_serves =
      info.have_preview && other_extra_channels == 0 &&
      info.preview.xsize == (info.xsize + factor - 1) / factor &&
      info.preview.ysize == (info.ysize + factor - 1) / factor;
  std::printf("    input %s an embedded preview of the requested size\n",
              embedded_serves ? "has" : "does not have");
  JxlPreviewBackend backend;
  const JxlPreviewStatus st = GenerateWithBackends(
      factor, JXL_PREVIEW_BACKEND_BIT_EMBEDDED_PREVIEW, &backend);
  if (embedded_serves) {
    PREVIEW_EXPECT_EQ(st, JXL_PREVIEW_SUCCESS);
    PREVIEW_EXPECT_EQ(backend, JXL_PREVIEW_BACKEND_EMBEDDED_PREVIEW);
  } else {
    PREVIEW_EXPECT_EQ(st, JXL_PREVIEW_NO_BACKEND_AVAILABLE);
  }
  return true;
}

// Whether `backend` is one of the decoder's render methods.
bool IsDecoderMethod(JxlPreviewBackend backend) {
  return backend == JXL_PREVIEW_BACKEND_NATIVE_DC_ONLY ||
         backend == JXL_PREVIEW_BACKEND_FALLBACK_DOWNSAMPLE ||
         backend == JXL_PREVIEW_BACKEND_NATIVE_REDUCED_INPUT ||
         backend == JXL_PREVIEW_BACKEND_NATIVE_FUSED_UPSAMPLING;
}

bool Case_Generate_AllowedBackends_NoBackend() {
  // Without preview_hooks, the decoder's render methods cannot be selected
  // one by one, so a mask that allows only one of them (and no embedded
  // preview) leaves no backend that can serve the request.
  JxlPreviewBackend backend;
  PREVIEW_EXPECT_EQ(
      GenerateWithBackends(8, JXL_PREVIEW_BACKEND_BIT_NATIVE_DC_ONLY, &backend),
      JXL_PREVIEW_NO_BACKEND_AVAILABLE);
  PREVIEW_EXPECT_EQ(backend, JXL_PREVIEW_BACKEND_NONE);
  // The same request served by the decoder's downsampling as a whole, which
  // reports the method it used.
  PREVIEW_EXPECT_EQ(
      GenerateWithBackends(8, JXL_PREVIEW_BACKEND_BIT_DECODER_DOWNSAMPLE,
                           &backend),
      JXL_PREVIEW_SUCCESS);
  PREVIEW_EXPECT(IsDecoderMethod(backend));
  return true;
}

bool Case_Generate_AllowedBackends_All() {
  JxlPreviewOptions o;
  JxlPreviewOptionsInit(&o);
  uint32_t xs, ys;
  uint8_t* px = nullptr;
  o.out_xsize = &xs;
  o.out_ysize = &ys;
  o.out_pixels = &px;
  o.preview_downsampling = 4;
  // Every backend bit, which allows what the default mask 0 allows.
  o.allowed_backends = JXL_PREVIEW_BACKEND_BIT_FULL_DECODE |
                       JXL_PREVIEW_BACKEND_BIT_EMBEDDED_PREVIEW |
                       JXL_PREVIEW_BACKEND_BIT_NATIVE_DC_ONLY |
                       JXL_PREVIEW_BACKEND_BIT_NATIVE_PROGRESSION_FLUSH |
                       JXL_PREVIEW_BACKEND_BIT_FALLBACK_DOWNSAMPLE |
                       JXL_PREVIEW_BACKEND_BIT_NATIVE_REDUCED_INPUT |
                       JXL_PREVIEW_BACKEND_BIT_NATIVE_FUSED_UPSAMPLING |
                       JXL_PREVIEW_BACKEND_BIT_DECODER_DOWNSAMPLE;
  PREVIEW_EXPECT_EQ(JxlGeneratePreview(g_input.data(), g_input.size(), &o),
                    JXL_PREVIEW_SUCCESS);
  std::free(px);
  return true;
}

bool Case_Generate_ColorEncoding_sRGB() {
  JxlColorEncoding ce;
  std::memset(&ce, 0, sizeof(ce));
  ce.color_space = JXL_COLOR_SPACE_RGB;
  ce.white_point = JXL_WHITE_POINT_D65;
  ce.primaries = JXL_PRIMARIES_SRGB;
  ce.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
  ce.rendering_intent = JXL_RENDERING_INTENT_RELATIVE;

  JxlPreviewOptions o;
  JxlPreviewOptionsInit(&o);
  uint32_t xs, ys;
  uint8_t* px = nullptr;
  o.out_xsize = &xs;
  o.out_ysize = &ys;
  o.out_pixels = &px;
  o.preview_downsampling = 4;
  o.color_encoding = &ce;
  JxlColorEncoding out_ce;
  std::memset(&out_ce, 0, sizeof(out_ce));
  o.out_color_encoding = &out_ce;
  PREVIEW_EXPECT_EQ(JxlGeneratePreview(g_input.data(), g_input.size(), &o),
                    JXL_PREVIEW_SUCCESS);
  std::free(px);
  // The output is in the requested encoding.
  PREVIEW_EXPECT_EQ(out_ce.color_space, JXL_COLOR_SPACE_RGB);
  PREVIEW_EXPECT_EQ(out_ce.white_point, JXL_WHITE_POINT_D65);
  PREVIEW_EXPECT_EQ(out_ce.primaries, JXL_PRIMARIES_SRGB);
  PREVIEW_EXPECT_EQ(out_ce.transfer_function, JXL_TRANSFER_FUNCTION_SRGB);
  return true;
}

bool Case_Generate_DisplayNits_HDR() {
  if (!g_have_hdr) {
    ++g_skip;
    std::printf("    (skipped: no HDR input provided)\n");
    return true;
  }
  // Decode same HDR input three ways, verify outputs differ pairwise.
  auto decode = [&](float nits, std::vector<uint8_t>* out, uint32_t* xs,
                    uint32_t* ys) {
    JxlPreviewOptions o;
    JxlPreviewOptionsInit(&o);
    uint8_t* px = nullptr;
    size_t pxs = 0;
    o.out_xsize = xs;
    o.out_ysize = ys;
    o.out_pixels = &px;
    o.out_pixels_size = &pxs;
    o.preview_downsampling = 4;
    o.display_nits = nits;
    JxlPreviewStatus st =
        JxlGeneratePreview(g_input_hdr.data(), g_input_hdr.size(), &o);
    if (st != JXL_PREVIEW_SUCCESS) return false;
    out->assign(px, px + pxs);
    std::free(px);
    return true;
  };
  std::vector<uint8_t> sdr, hdr1000, no_tm;
  uint32_t xs, ys;
  PREVIEW_EXPECT(decode(0.0f, &sdr, &xs, &ys));  // auto SDR (250)
  PREVIEW_EXPECT(decode(1000.0f, &hdr1000, &xs, &ys));
  PREVIEW_EXPECT(decode(JXL_PREVIEW_NO_TONE_MAPPING, &no_tm, &xs, &ys));
  PREVIEW_EXPECT(sdr.size() == hdr1000.size() && sdr.size() == no_tm.size());
  // The three outputs should not be byte-identical (the tone-map differs).
  PREVIEW_EXPECT(sdr != hdr1000);
  PREVIEW_EXPECT(sdr != no_tm);
  return true;
}

bool Case_Generate_DisplayNits_Negative() {
  JxlPreviewOptions o;
  JxlPreviewOptionsInit(&o);
  uint32_t xs, ys;
  uint8_t* px = nullptr;
  o.out_xsize = &xs;
  o.out_ysize = &ys;
  o.out_pixels = &px;
  o.preview_downsampling = 4;
  o.display_nits = -42.0f;  // not -1.0; should be invalid
  PREVIEW_EXPECT_EQ(JxlGeneratePreview(g_input.data(), g_input.size(), &o),
                    JXL_PREVIEW_INVALID_ARGUMENT);
  return true;
}

bool Case_Generate_CorruptInput() {
  std::vector<uint8_t> garbage(64, 0xAB);
  JxlPreviewOptions o;
  JxlPreviewOptionsInit(&o);
  uint32_t xs, ys;
  uint8_t* px = nullptr;
  o.out_xsize = &xs;
  o.out_ysize = &ys;
  o.out_pixels = &px;
  o.preview_downsampling = 4;
  // Not a JPEG XL signature.
  PREVIEW_EXPECT_EQ(JxlGeneratePreview(garbage.data(), garbage.size(), &o),
                    JXL_PREVIEW_CORRUPT_INPUT);
  PREVIEW_EXPECT(px == nullptr);
  // Too short for a signature.
  PREVIEW_EXPECT_EQ(JxlGeneratePreview(g_input.data(), 1, &o),
                    JXL_PREVIEW_CORRUPT_INPUT);
  // A valid signature, truncated: the decoder's error.
  PREVIEW_EXPECT_EQ(JxlGeneratePreview(g_input.data(), g_input.size() / 2, &o),
                    JXL_PREVIEW_CORRUPT_INPUT);
  PREVIEW_EXPECT(px == nullptr);
  return true;
}

// A non-zero allowed_backends mask allows factor 1, the full decode, only
// with JXL_PREVIEW_BACKEND_BIT_FULL_DECODE, whether the factor is explicit or
// picked from target sizes.
bool Case_Generate_FullDecodeBit() {
  JxlPreviewInfo info;
  PREVIEW_EXPECT_EQ(
      JxlGetPreviewInfo(g_input.data(), g_input.size(), nullptr, &info),
      JXL_PREVIEW_SUCCESS);
  for (bool explicit_factor : {true, false}) {
    for (bool full_decode_bit : {false, true}) {
      JxlPreviewOptions o;
      JxlPreviewOptionsInit(&o);
      uint32_t xs = 0;
      uint32_t ys = 0;
      uint8_t* px = nullptr;
      JxlPreviewBackend backend = JXL_PREVIEW_BACKEND_EMBEDDED_PREVIEW;
      uint32_t factor = 0;
      o.out_xsize = &xs;
      o.out_ysize = &ys;
      o.out_pixels = &px;
      o.out_backend_used = &backend;
      o.out_downsampling = &factor;
      if (explicit_factor) {
        o.preview_downsampling = 1;
      } else {
        o.target_xsize = info.xsize;
        o.target_ysize = info.ysize;
      }
      o.allowed_backends =
          JXL_PREVIEW_BACKEND_BIT_EMBEDDED_PREVIEW |
          JXL_PREVIEW_BACKEND_BIT_DECODER_DOWNSAMPLE |
          (full_decode_bit ? JXL_PREVIEW_BACKEND_BIT_FULL_DECODE : 0u);
      const JxlPreviewStatus st =
          JxlGeneratePreview(g_input.data(), g_input.size(), &o);
      if (!full_decode_bit) {
        PREVIEW_EXPECT_EQ(st, JXL_PREVIEW_NO_BACKEND_AVAILABLE);
        PREVIEW_EXPECT(px == nullptr);
        continue;
      }
      PREVIEW_EXPECT_EQ(st, JXL_PREVIEW_SUCCESS);
      std::free(px);
      PREVIEW_EXPECT_EQ(backend, JXL_PREVIEW_BACKEND_NONE);
      PREVIEW_EXPECT_EQ(factor, 1u);
      PREVIEW_EXPECT_EQ(xs, info.xsize);
      PREVIEW_EXPECT_EQ(ys, info.ysize);
    }
  }
  return true;
}

// A dst_stride below the row size is a buffer too small, which reports the
// stride and size the buffer needs.
bool Case_Generate_StrideTooSmall() {
  JxlPreviewInfo info;
  PREVIEW_EXPECT_EQ(
      JxlGetPreviewInfo(g_input.data(), g_input.size(), nullptr, &info),
      JXL_PREVIEW_SUCCESS);
  const uint32_t factor = 4;
  std::vector<uint8_t> buf(static_cast<size_t>(info.xsize) * info.ysize * 4);
  JxlPreviewOptions o;
  JxlPreviewOptionsInit(&o);
  uint32_t xs = 0;
  uint32_t ys = 0;
  uint8_t* px = nullptr;
  size_t stride = 0;
  size_t px_size = 0;
  JxlPixelFormat actual = {};
  o.out_xsize = &xs;
  o.out_ysize = &ys;
  o.out_pixels = &px;
  o.out_stride = &stride;
  o.out_pixels_size = &px_size;
  o.out_format = &actual;
  o.preview_downsampling = factor;
  o.format = {0, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
  o.dst = buf.data();
  o.dst_size = buf.size();
  o.dst_stride = 1;
  PREVIEW_EXPECT_EQ(JxlGeneratePreview(g_input.data(), g_input.size(), &o),
                    JXL_PREVIEW_BUFFER_TOO_SMALL);
  PREVIEW_EXPECT(px == nullptr);
  PREVIEW_EXPECT_EQ(xs, (info.xsize + factor - 1) / factor);
  PREVIEW_EXPECT_EQ(ys, (info.ysize + factor - 1) / factor);
  PREVIEW_EXPECT(actual.num_channels >= 1 && actual.num_channels <= 4);
  const size_t row_bytes = static_cast<size_t>(xs) * actual.num_channels;
  PREVIEW_EXPECT_EQ(stride, row_bytes);
  PREVIEW_EXPECT_EQ(px_size, row_bytes * ys);
  return true;
}

// A memory manager needs both alloc and free.
bool Case_Generate_HalfMemoryManager() {
  CountingMM mm_state;
  JxlMemoryManager mm{&mm_state, &CountingAlloc, nullptr};
  JxlPreviewOptions o;
  JxlPreviewOptionsInit(&o);
  uint32_t xs, ys;
  uint8_t* px = nullptr;
  o.out_xsize = &xs;
  o.out_ysize = &ys;
  o.out_pixels = &px;
  o.preview_downsampling = 4;
  o.memory_manager = &mm;
  PREVIEW_EXPECT_EQ(JxlGeneratePreview(g_input.data(), g_input.size(), &o),
                    JXL_PREVIEW_INVALID_ARGUMENT);
  PREVIEW_EXPECT(px == nullptr);
  PREVIEW_EXPECT_EQ(mm_state.alloc_count.load(), 0);
  return true;
}

// Every non-NULL output is reset, also when the call fails.
bool Case_Generate_OutputsResetOnFailure() {
  JxlPreviewOptions o;
  JxlPreviewOptionsInit(&o);
  uint32_t xs = 77;
  uint32_t ys = 77;
  uint8_t dummy = 0;
  uint8_t* px = &dummy;
  size_t stride = 77;
  size_t px_size = 77;
  JxlPixelFormat actual = {3, JXL_TYPE_FLOAT, JXL_BIG_ENDIAN, 64};
  JxlPreviewBackend backend = JXL_PREVIEW_BACKEND_FALLBACK_DOWNSAMPLE;
  uint32_t factor = 77;
  JxlColorEncoding out_ce;
  std::memset(&out_ce, 0, sizeof(out_ce));
  out_ce.color_space = JXL_COLOR_SPACE_RGB;
  o.out_xsize = &xs;
  o.out_ysize = &ys;
  o.out_pixels = &px;
  o.out_stride = &stride;
  o.out_pixels_size = &px_size;
  o.out_format = &actual;
  o.out_backend_used = &backend;
  o.out_downsampling = &factor;
  o.out_color_encoding = &out_ce;
  o.preview_downsampling = 3;  // invalid
  PREVIEW_EXPECT_EQ(JxlGeneratePreview(g_input.data(), g_input.size(), &o),
                    JXL_PREVIEW_INVALID_ARGUMENT);
  PREVIEW_EXPECT(px == nullptr);
  PREVIEW_EXPECT_EQ(xs, 0u);
  PREVIEW_EXPECT_EQ(ys, 0u);
  PREVIEW_EXPECT_EQ(stride, 0u);
  PREVIEW_EXPECT_EQ(px_size, 0u);
  const JxlPixelFormat zero_format = {};
  PREVIEW_EXPECT(std::memcmp(&actual, &zero_format, sizeof(actual)) == 0);
  PREVIEW_EXPECT_EQ(backend, JXL_PREVIEW_BACKEND_NONE);
  PREVIEW_EXPECT_EQ(factor, 0u);
  PREVIEW_EXPECT_EQ(out_ce.color_space, JXL_COLOR_SPACE_UNKNOWN);
  return true;
}

bool Case_Generate_Float32_Output() {
  JxlPreviewOptions o;
  JxlPreviewOptionsInit(&o);
  uint32_t xs, ys;
  uint8_t* px = nullptr;
  size_t stride = 0;
  JxlPixelFormat actual = {};
  o.out_xsize = &xs;
  o.out_ysize = &ys;
  o.out_pixels = &px;
  o.out_stride = &stride;
  o.out_format = &actual;
  o.preview_downsampling = 2;
  o.format = {4, JXL_TYPE_FLOAT, JXL_NATIVE_ENDIAN, 0};
  PREVIEW_EXPECT_EQ(JxlGeneratePreview(g_input.data(), g_input.size(), &o),
                    JXL_PREVIEW_SUCCESS);
  PREVIEW_EXPECT_EQ(actual.data_type, JXL_TYPE_FLOAT);
  PREVIEW_EXPECT_EQ(stride, static_cast<size_t>(xs) * actual.num_channels * 4u);
  std::free(px);
  return true;
}

bool Case_Generate_Float16_Output() {
  for (uint32_t factor : {2u, 8u}) {
    JxlPreviewOptions o;
    JxlPreviewOptionsInit(&o);
    uint32_t xs, ys;
    uint8_t* px = nullptr;
    size_t stride = 0;
    JxlPixelFormat actual = {};
    o.out_xsize = &xs;
    o.out_ysize = &ys;
    o.out_pixels = &px;
    o.out_stride = &stride;
    o.out_format = &actual;
    o.preview_downsampling = factor;
    o.format = {4, JXL_TYPE_FLOAT16, JXL_NATIVE_ENDIAN, 0};
    PREVIEW_EXPECT_EQ(JxlGeneratePreview(g_input.data(), g_input.size(), &o),
                      JXL_PREVIEW_SUCCESS);
    PREVIEW_EXPECT_EQ(actual.data_type, JXL_TYPE_FLOAT16);
    PREVIEW_EXPECT_EQ(stride,
                      static_cast<size_t>(xs) * actual.num_channels * 2u);
    std::free(px);
  }
  return true;
}

// format.align is ignored: the output, allocated or in a caller buffer, holds
// the same rows as with align 0.
bool Case_Generate_AlignIgnored() {
  auto decode = [](size_t align, uint8_t* dst, size_t dst_size,
                   size_t dst_stride, std::vector<uint8_t>* rows,
                   size_t* row_bytes) {
    JxlPreviewOptions o;
    JxlPreviewOptionsInit(&o);
    uint32_t xs, ys;
    uint8_t* px = nullptr;
    size_t stride = 0;
    JxlPixelFormat actual = {};
    o.out_xsize = &xs;
    o.out_ysize = &ys;
    o.out_pixels = &px;
    o.out_stride = &stride;
    o.out_format = &actual;
    o.preview_downsampling = 4;
    o.format = {4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, align};
    o.dst = dst;
    o.dst_size = dst_size;
    o.dst_stride = dst_stride;
    if (JxlGeneratePreview(g_input.data(), g_input.size(), &o) !=
            JXL_PREVIEW_SUCCESS ||
        actual.align != 0) {
      return false;
    }
    *row_bytes = static_cast<size_t>(xs) * actual.num_channels;
    rows->clear();
    for (uint32_t y = 0; y < ys; ++y) {
      rows->insert(rows->end(), px + y * stride, px + y * stride + *row_bytes);
    }
    if (dst == nullptr) std::free(px);
    return true;
  };
  std::vector<uint8_t> reference;
  std::vector<uint8_t> rows;
  size_t row_bytes = 0;
  PREVIEW_EXPECT(decode(0, nullptr, 0, 0, &reference, &row_bytes));
  // 61 is coprime with any row size the decoder might pad to.
  PREVIEW_EXPECT(decode(61, nullptr, 0, 0, &rows, &row_bytes));
  PREVIEW_EXPECT(rows == reference);
  const size_t stride = row_bytes + 13;
  std::vector<uint8_t> buf(stride * (reference.size() / row_bytes), 0xCC);
  PREVIEW_EXPECT(decode(61, buf.data(), buf.size(), stride, &rows, &row_bytes));
  PREVIEW_EXPECT(rows == reference);
  return true;
}

// The decoder's render method is reported with and without the preview hooks
// alike.
bool Case_Generate_ReportRenderMethod() {
  JxlPreviewBackend backends[2] = {JXL_PREVIEW_BACKEND_NONE,
                                   JXL_PREVIEW_BACKEND_NONE};
  for (bool with_hooks : {false, true}) {
    JxlPreviewOptions o;
    JxlPreviewOptionsInit(&o);
    uint32_t xs, ys;
    uint8_t* px = nullptr;
    JxlPreviewBackend backend = JXL_PREVIEW_BACKEND_NONE;
    o.out_xsize = &xs;
    o.out_ysize = &ys;
    o.out_pixels = &px;
    o.out_backend_used = &backend;
    o.preview_downsampling = 8;
    if (with_hooks) o.preview_hooks = jxl::GetDecoderPreviewHooks();
    PREVIEW_EXPECT_EQ(JxlGeneratePreview(g_input.data(), g_input.size(), &o),
                      JXL_PREVIEW_SUCCESS);
    std::free(px);
    std::printf("    backend %s hooks: %d\n", with_hooks ? "with" : "without",
                static_cast<int>(backend));
    PREVIEW_EXPECT(backend != JXL_PREVIEW_BACKEND_NONE);
    PREVIEW_EXPECT(backend != JXL_PREVIEW_BACKEND_DECODER_DOWNSAMPLE);
    backends[with_hooks ? 1 : 0] = backend;
  }
  PREVIEW_EXPECT_EQ(backends[0], backends[1]);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <some.jxl> [<hdr.jxl>]\n", argv[0]);
    return 2;
  }
  if (!ReadFile(argv[1], &g_input) || g_input.empty()) {
    std::fprintf(stderr, "failed to read %s\n", argv[1]);
    return 2;
  }
  if (argc >= 3) {
    if (!ReadFile(argv[2], &g_input_hdr) || g_input_hdr.empty()) {
      std::fprintf(stderr, "failed to read HDR input %s\n", argv[2]);
      return 2;
    }
    g_have_hdr = true;
  }

  std::printf("preview_api_test: input=%s (%" PRIuS " bytes)%s\n", argv[1],
              g_input.size(), g_have_hdr ? ", HDR input provided" : "");

  RunCase("OptionsInit zeroes struct", Case_OptionsInit_Zeroes);
  RunCase("GetInfo basic header probe", Case_GetInfo_Basic);
  RunCase("GetInfo rejects null/zero args", Case_GetInfo_NullArgs);
  RunCase("GetInfo rejects corrupt input", Case_GetInfo_CorruptInput);
  RunCase("GetInfo recommended_factor", Case_GetInfo_RecommendedFactor);
  RunCase("GetInfo retrieves ICC", Case_GetInfo_ICC);
  RunCase("GetInfo missing icc_size_ptr -> INVALID_ARGUMENT",
          Case_GetInfo_ICC_MissingSizePtr);
  RunCase("Generate defaults allocate RGBA8 buffer",
          Case_Generate_Defaults_Allocates);
  RunCase("Generate rejects null/missing out args", Case_Generate_NullArgs);
  RunCase("Generate rejects bad downsampling factor", Case_Generate_BadFactor);
  RunCase("Generate writes into caller buffer (tight stride)",
          Case_Generate_CallerBuffer_TightStride);
  RunCase("Generate honors padded dst_stride",
          Case_Generate_CallerBuffer_PaddedStride);
  RunCase("Generate returns BUFFER_TOO_SMALL", Case_Generate_BufferTooSmall);
  RunCase("Generate auto-factor from target size",
          Case_Generate_TargetSize_AutoFactor);
  RunCase("Generate uses custom memory manager", Case_Generate_MemoryManager);
  RunCase("Generate uses parallel runner", Case_Generate_ParallelRunner);
  RunCase("Generate honors pre-set cancel flag",
          Case_Generate_Cancel_Immediate);
  RunCase("Generate honors allowed_backends restriction",
          Case_Generate_AllowedBackends_Restrict);
  RunCase("Generate maps an unservable backend mask to NO_BACKEND_AVAILABLE",
          Case_Generate_AllowedBackends_NoBackend);
  RunCase("Generate accepts a mask of every backend bit",
          Case_Generate_AllowedBackends_All);
  RunCase("Generate accepts JxlColorEncoding (sRGB)",
          Case_Generate_ColorEncoding_sRGB);
  RunCase("Generate display_nits {auto, 1000, no-tone-map}",
          Case_Generate_DisplayNits_HDR);
  RunCase("Generate rejects negative display_nits",
          Case_Generate_DisplayNits_Negative);
  RunCase("Generate maps corrupt input to CORRUPT_INPUT",
          Case_Generate_CorruptInput);
  RunCase("Generate needs BIT_FULL_DECODE in a mask for factor 1",
          Case_Generate_FullDecodeBit);
  RunCase("Generate reports the needed stride for a too small dst_stride",
          Case_Generate_StrideTooSmall);
  RunCase("Generate rejects a half-set memory manager",
          Case_Generate_HalfMemoryManager);
  RunCase("Generate resets every output on failure",
          Case_Generate_OutputsResetOnFailure);
  RunCase("Generate emits FLOAT32 output", Case_Generate_Float32_Output);
  RunCase("Generate emits FLOAT16 output", Case_Generate_Float16_Output);
  RunCase("Generate ignores format.align", Case_Generate_AlignIgnored);
  RunCase("Generate reports render method with and without hooks",
          Case_Generate_ReportRenderMethod);

  std::printf("\n== preview_api_test summary: %d passed, %d failed", g_pass,
              g_fail);
  if (g_skip > 0) std::printf(", %d skipped", g_skip);
  std::printf(" ==\n");
  return g_fail == 0 ? 0 : 1;
}
