// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "lib/jxl/ac_strategy.h"

#include <jxl/memory_manager.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <hwy/base.h>  // HWY_ALIGN_MAX
#include <hwy/tests/hwy_gtest.h>
#include <vector>

#include "lib/jxl/base/random.h"
#include "lib/jxl/coeff_order_fwd.h"
#include "lib/jxl/dec_transforms_testonly.h"
#include "lib/jxl/enc_transforms.h"
#include "lib/jxl/memory_manager_internal.h"
#include "lib/jxl/simd_util.h"
#include "lib/jxl/test_memory_manager.h"
#include "lib/jxl/test_utils.h"
#include "lib/jxl/testing.h"

namespace jxl {
namespace {

// Test that DCT -> IDCT is a noop.
class AcStrategyRoundtrip : public ::hwy::TestWithParamTargetAndT<int> {
 protected:
  void Run() {
    JxlMemoryManager* memory_manager = test::MemoryManager();
    const AcStrategyType type = static_cast<AcStrategyType>(GetParam());
    const AcStrategy acs = AcStrategy::FromRawStrategy(type);
    const size_t dct_scratch_size =
        3 * (MaxVectorSize() / sizeof(float)) * AcStrategy::kMaxBlockDim;

    size_t mem_bytes =
        (4 * AcStrategy::kMaxCoeffArea + dct_scratch_size) * sizeof(float);
    JXL_TEST_ASSIGN_OR_DIE(AlignedMemory mem,
                           AlignedMemory::Create(memory_manager, mem_bytes));
    float* coeffs = mem.address<float>();
    float* idct = coeffs + AcStrategy::kMaxCoeffArea;
    float* input = idct + AcStrategy::kMaxCoeffArea;
    float* scratch_space = input + AcStrategy::kMaxCoeffArea;

    Rng rng(static_cast<uint64_t>(type) * 65537 + 13);

    for (size_t j = 0; j < 64; j++) {
      size_t i = (acs.log2_covered_blocks()
                      ? rng.UniformU(0, 64u << acs.log2_covered_blocks())
                      : j);
      std::fill_n(input, AcStrategy::kMaxCoeffArea, 0);
      input[i] = 0.2f;
      TransformFromPixels(type, input, acs.covered_blocks_x() * 8, coeffs,
                          scratch_space);
      ASSERT_NEAR(coeffs[0], 0.2 / (64 << acs.log2_covered_blocks()), 1e-6)
          << " i = " << i;
      TransformToPixels(type, coeffs, idct, acs.covered_blocks_x() * 8,
                        scratch_space);
      for (size_t k = 0; k < 64u << acs.log2_covered_blocks(); k++) {
        ASSERT_NEAR(idct[k], k == i ? 0.2f : 0, 2e-6)
            << "k = " << k << " i = " << i << " acs " << static_cast<int>(type);
      }
    }
    // Test DC.
    std::fill_n(idct, AcStrategy::kMaxCoeffArea, 0);
    for (size_t y = 0; y < acs.covered_blocks_y(); y++) {
      for (size_t x = 0; x < acs.covered_blocks_x(); x++) {
        float* dc = idct + AcStrategy::kMaxCoeffArea;
        std::fill_n(dc, AcStrategy::kMaxCoeffArea, 0);
        dc[y * acs.covered_blocks_x() * 8 + x] = 0.2;
        LowestFrequenciesFromDC(type, dc, acs.covered_blocks_x() * 8, coeffs,
                                scratch_space);
        DCFromLowestFrequencies(type, coeffs, idct, acs.covered_blocks_x() * 8,
                                scratch_space);
        std::fill_n(dc, AcStrategy::kMaxCoeffArea, 0);
        dc[y * acs.covered_blocks_x() * 8 + x] = 0.2;
        for (size_t j = 0; j < 64u << acs.log2_covered_blocks(); j++) {
          ASSERT_NEAR(idct[j], dc[j], 1e-6)
              << "j = " << j << " x = " << x << " y = " << y << " acs "
              << static_cast<int>(type);
        }
      }
    }
  }
};

HWY_TARGET_INSTANTIATE_TEST_SUITE_P_T(
    AcStrategyRoundtrip,
    ::testing::Range(0, static_cast<int>(AcStrategy::kNumValidStrategies)));

TEST_P(AcStrategyRoundtrip, Test) { Run(); }

// Test that DC(2x2) -> DCT coefficients -> IDCT -> downsampled IDCT is a noop.
class AcStrategyRoundtripDownsample
    : public ::hwy::TestWithParamTargetAndT<int> {
 protected:
  void Run() {
    JxlMemoryManager* memory_manager = test::MemoryManager();
    const AcStrategyType type = static_cast<AcStrategyType>(GetParam());
    const AcStrategy acs = AcStrategy::FromRawStrategy(type);
    const size_t dct_scratch_size =
        3 * (MaxVectorSize() / sizeof(float)) * AcStrategy::kMaxBlockDim;

    size_t mem_bytes =
        (4 * AcStrategy::kMaxCoeffArea + dct_scratch_size) * sizeof(float);
    JXL_TEST_ASSIGN_OR_DIE(AlignedMemory mem,
                           AlignedMemory::Create(memory_manager, mem_bytes));
    float* coeffs = mem.address<float>();
    float* idct = coeffs + AcStrategy::kMaxCoeffArea;
    float* dc = idct + AcStrategy::kMaxCoeffArea;
    float* scratch_space = dc + AcStrategy::kMaxCoeffArea;

    std::fill_n(coeffs, AcStrategy::kMaxCoeffArea, 0.0f);
    Rng rng(static_cast<uint64_t>(type) * 65537 + 13);

    for (size_t y = 0; y < acs.covered_blocks_y(); y++) {
      for (size_t x = 0; x < acs.covered_blocks_x(); x++) {
        if (x > 4 || y > 4) {
          if (rng.Bernoulli(0.9f)) continue;
        }
        std::fill_n(dc, AcStrategy::kMaxCoeffArea, 0);
        dc[y * acs.covered_blocks_x() * 8 + x] = 0.2f;
        LowestFrequenciesFromDC(type, dc, acs.covered_blocks_x() * 8, coeffs,
                                scratch_space);
        TransformToPixels(type, coeffs, idct, acs.covered_blocks_x() * 8,
                          scratch_space);
        std::fill_n(coeffs, AcStrategy::kMaxCoeffArea, 0.0f);
        std::fill_n(dc, AcStrategy::kMaxCoeffArea, 0);
        dc[y * acs.covered_blocks_x() * 8 + x] = 0.2f;
        // Downsample
        for (size_t dy = 0; dy < acs.covered_blocks_y(); dy++) {
          for (size_t dx = 0; dx < acs.covered_blocks_x(); dx++) {
            float sum = 0;
            for (size_t iy = 0; iy < 8; iy++) {
              for (size_t ix = 0; ix < 8; ix++) {
                sum += idct[(dy * 8 + iy) * 8 * acs.covered_blocks_x() +
                            dx * 8 + ix];
              }
            }
            sum /= 64.0f;
            ASSERT_NEAR(sum, dc[dy * 8 * acs.covered_blocks_x() + dx], 1e-6)
                << "acs " << static_cast<int>(type);
          }
        }
      }
    }
  }
};

HWY_TARGET_INSTANTIATE_TEST_SUITE_P_T(
    AcStrategyRoundtripDownsample,
    ::testing::Range(0, static_cast<int>(AcStrategy::kNumValidStrategies)));

TEST_P(AcStrategyRoundtripDownsample, Test) { Run(); }

// Test that IDCT(block with zeros in the non-topleft corner) -> downsampled
// IDCT is the same as IDCT -> DC(2x2) of the same block.
class AcStrategyDownsample : public ::hwy::TestWithParamTargetAndT<int> {
 protected:
  void Run() {
    JxlMemoryManager* memory_manager = test::MemoryManager();
    const AcStrategyType type = static_cast<AcStrategyType>(GetParam());
    const AcStrategy acs = AcStrategy::FromRawStrategy(type);
    const size_t dct_scratch_size =
        3 * (MaxVectorSize() / sizeof(float)) * AcStrategy::kMaxBlockDim;
    size_t cx = acs.covered_blocks_y();
    size_t cy = acs.covered_blocks_x();
    CoefficientLayout(&cy, &cx);

    size_t mem_bytes =
        (4 * AcStrategy::kMaxCoeffArea + dct_scratch_size) * sizeof(float);
    JXL_TEST_ASSIGN_OR_DIE(AlignedMemory mem,
                           AlignedMemory::Create(memory_manager, mem_bytes));
    float* const idct = mem.address<float>();
    float* idct_acs_downsampled = idct + AcStrategy::kMaxCoeffArea;
    float* const coeffs = idct + AcStrategy::kMaxCoeffArea;
    float* scratch_space = coeffs + AcStrategy::kMaxCoeffArea;

    Rng rng(static_cast<uint64_t>(type) * 65537 + 13);

    for (size_t y = 0; y < cy; y++) {
      for (size_t x = 0; x < cx; x++) {
        if (x > 4 || y > 4) {
          if (rng.Bernoulli(0.9f)) continue;
        }
        std::fill_n(coeffs, AcStrategy::kMaxCoeffArea, 0);
        coeffs[y * cx * 8 + x] = 0.2f;
        TransformToPixels(type, coeffs, idct, acs.covered_blocks_x() * 8,
                          scratch_space);
        std::fill_n(coeffs, AcStrategy::kMaxCoeffArea, 0);
        coeffs[y * cx * 8 + x] = 0.2f;
        DCFromLowestFrequencies(type, coeffs, idct_acs_downsampled,
                                acs.covered_blocks_x() * 8, scratch_space);
        // Downsample
        for (size_t dy = 0; dy < acs.covered_blocks_y(); dy++) {
          for (size_t dx = 0; dx < acs.covered_blocks_x(); dx++) {
            float sum = 0;
            for (size_t iy = 0; iy < 8; iy++) {
              for (size_t ix = 0; ix < 8; ix++) {
                sum += idct[(dy * 8 + iy) * 8 * acs.covered_blocks_x() +
                            dx * 8 + ix];
              }
            }
            sum /= 64;
            ASSERT_NEAR(
                sum, idct_acs_downsampled[dy * 8 * acs.covered_blocks_x() + dx],
                2e-6)
                << " acs " << static_cast<int>(type);
          }
        }
      }
    }
  }
};

HWY_TARGET_INSTANTIATE_TEST_SUITE_P_T(
    AcStrategyDownsample,
    ::testing::Range(0, static_cast<int>(AcStrategy::kNumValidStrategies)));

TEST_P(AcStrategyDownsample, Test) { Run(); }

// Test that the reduced-resolution IDCT of a block whose content only has
// frequencies representable at the reduced size is the box average of the
// block. The coefficients come from the forward transform, so the test does
// not assume a coefficient layout.
class AcStrategyReducedPixels : public ::hwy::TestWithParamTargetAndT<int> {
 protected:
  void Run() {
    JxlMemoryManager* memory_manager = test::MemoryManager();
    const AcStrategyType type = static_cast<AcStrategyType>(GetParam());
    const AcStrategy acs = AcStrategy::FromRawStrategy(type);
    const size_t dct_scratch_size =
        3 * (MaxVectorSize() / sizeof(float)) * AcStrategy::kMaxBlockDim;
    const size_t xsize = acs.covered_blocks_x() * kBlockDim;
    const size_t ysize = acs.covered_blocks_y() * kBlockDim;

    size_t mem_bytes =
        (5 * AcStrategy::kMaxCoeffArea + dct_scratch_size) * sizeof(float);
    JXL_TEST_ASSIGN_OR_DIE(AlignedMemory mem,
                           AlignedMemory::Create(memory_manager, mem_bytes));
    float* pixels = mem.address<float>();
    float* coeffs = pixels + AcStrategy::kMaxCoeffArea;
    float* reduced = coeffs + AcStrategy::kMaxCoeffArea;
    float* scratch_space = reduced + AcStrategy::kMaxCoeffArea;

    const auto basis = [](size_t n, size_t k, size_t i) {
      constexpr double kPi = 3.14159265358979323846;
      return std::cos(kPi * static_cast<double>((2 * i + 1) * k) /
                      static_cast<double>(2 * n));
    };
    Rng rng(static_cast<uint64_t>(type) * 65537 + 17);
    for (size_t factor : {2, 4, 8}) {
      const size_t rx = xsize / factor;
      const size_t ry = ysize / factor;
      // pixels = sum over ky < ry, kx < rx of amp * cos(ky, y) * cos(kx, x),
      // computed separably.
      std::vector<double> amp(ry * rx);
      for (double& a : amp) {
        a = rng.UniformF(-1.0f, 1.0f) / std::sqrt(static_cast<double>(rx * ry));
      }
      std::vector<double> rows(ry * xsize, 0.0);
      for (size_t ky = 0; ky < ry; ky++) {
        for (size_t kx = 0; kx < rx; kx++) {
          for (size_t x = 0; x < xsize; x++) {
            rows[ky * xsize + x] += amp[ky * rx + kx] * basis(xsize, kx, x);
          }
        }
      }
      for (size_t y = 0; y < ysize; y++) {
        for (size_t x = 0; x < xsize; x++) {
          double v = 0;
          for (size_t ky = 0; ky < ry; ky++) {
            v += basis(ysize, ky, y) * rows[ky * xsize + x];
          }
          pixels[y * xsize + x] = static_cast<float>(v);
        }
      }
      TransformFromPixels(type, pixels, xsize, coeffs, scratch_space);
      if (!TransformToReducedPixels(type, coeffs, factor, reduced, rx,
                                    scratch_space)) {
        ASSERT_FALSE(acs.IsMultiblock() || type == AcStrategyType::DCT)
            << "acs " << static_cast<int>(type) << " factor " << factor;
        continue;
      }
      for (size_t y = 0; y < ry; y++) {
        for (size_t x = 0; x < rx; x++) {
          double sum = 0;
          for (size_t iy = 0; iy < factor; iy++) {
            for (size_t ix = 0; ix < factor; ix++) {
              sum += pixels[(y * factor + iy) * xsize + x * factor + ix];
            }
          }
          ASSERT_NEAR(reduced[y * rx + x], sum / (factor * factor), 1e-4)
              << "acs " << static_cast<int>(type) << " factor " << factor
              << " x " << x << " y " << y;
        }
      }
    }
  }
};

HWY_TARGET_INSTANTIATE_TEST_SUITE_P_T(
    AcStrategyReducedPixels,
    ::testing::Range(0, static_cast<int>(AcStrategy::kNumValidStrategies)));

TEST_P(AcStrategyReducedPixels, Test) { Run(); }

class AcStrategyTargetTest : public ::hwy::TestWithParamTarget {};
HWY_TARGET_INSTANTIATE_TEST_SUITE_P(AcStrategyTargetTest);

TEST_P(AcStrategyTargetTest, RoundtripAFVDCT) {
  HWY_ALIGN_MAX float idct[16];
  for (size_t i = 0; i < 16; i++) {
    HWY_ALIGN_MAX float pixels[16] = {};
    pixels[i] = 1;
    HWY_ALIGN_MAX float coeffs[16] = {};

    AFVDCT4x4(pixels, coeffs);
    AFVIDCT4x4(coeffs, idct);
    for (size_t j = 0; j < 16; j++) {
      EXPECT_NEAR(idct[j], pixels[j], 1e-6);
    }
  }
}

TEST_P(AcStrategyTargetTest, BenchmarkAFV) {
  JxlMemoryManager* memory_manager = test::MemoryManager();
  const AcStrategyType type = AcStrategyType::AFV0;
  HWY_ALIGN_MAX float pixels[64] = {1};
  HWY_ALIGN_MAX float coeffs[64] = {};
  const size_t dct_scratch_size =
      3 * (MaxVectorSize() / sizeof(float)) * AcStrategy::kMaxBlockDim;
  size_t mem_bytes = (64 + dct_scratch_size) * sizeof(float);
  JXL_TEST_ASSIGN_OR_DIE(AlignedMemory mem,
                         AlignedMemory::Create(memory_manager, mem_bytes));
  float* scratch_space = mem.address<float>();
  for (size_t i = 0; i < 1 << 14; i++) {
    TransformToPixels(type, coeffs, pixels, 8, scratch_space);
    TransformFromPixels(type, pixels, 8, coeffs, scratch_space);
  }
  EXPECT_NEAR(pixels[0], 0.0, 1E-6);
}

TEST_P(AcStrategyTargetTest, BenchmarkAFVDCT) {
  HWY_ALIGN_MAX float pixels[64] = {1};
  HWY_ALIGN_MAX float coeffs[64] = {};
  for (size_t i = 0; i < 1 << 14; i++) {
    AFVDCT4x4(pixels, coeffs);
    AFVIDCT4x4(coeffs, pixels);
  }
  EXPECT_NEAR(pixels[0], 1.0, 1E-6);
}

}  // namespace
}  // namespace jxl
