// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#ifndef TOOLS_PREVIEW_BENCHMARK_BENCHMARK_CORE_H_
#define TOOLS_PREVIEW_BENCHMARK_BENCHMARK_CORE_H_

#include <jxl/decode.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "lib/extras/dec/jxl.h"
#include "lib/extras/packed_image.h"
#include "lib/jxl/dec_preview_internal.h"

namespace jpegxl {
namespace tools {

enum class PreviewBenchMode { kFull, kPreview };

// Where the decode runs are measured, decided once for a whole benchmark.
// kWorker runs every decode in a preview_bench_worker process, which also
// reports the process memory high-water marks. kInProcess runs the decodes in
// the calling process, where process memory cannot be attributed to a decode
// and is not reported.
enum class PreviewBenchMeasurement { kWorker, kInProcess };

struct PreviewBenchOptions {
  size_t preview_downsampling = 4;
  size_t num_reps = 5;
  size_t num_threads = 0;
  std::string color_space = "RGB_D65_SRG_Rel_SRG";
  PreviewBenchMeasurement measurement = PreviewBenchMeasurement::kWorker;
};

struct PreviewBenchRun {
  bool ok = false;
  std::string error;
  double wall_time_ms = 0.0;
  double cpu_time_ms = 0.0;
  uint64_t decoder_peak_bytes = 0;
  uint64_t decoder_total_bytes = 0;
  uint64_t decoder_num_allocations = 0;
  // High-water marks of the worker process, where the OS reports them.
  bool has_process_peak_working_set = false;
  uint64_t process_peak_working_set_bytes = 0;
  bool has_process_peak_private = false;
  uint64_t process_peak_private_bytes = 0;
  size_t decoded_bytes = 0;
  size_t output_xsize = 0;
  size_t output_ysize = 0;
  size_t frame_count = 0;
  uint32_t num_channels = 0;
  jxl::extras::JXLPreviewBackend preview_backend =
      jxl::extras::JXLPreviewBackend::kNone;
};

struct PreviewBenchStats {
  double mean = 0.0;
  double stddev = 0.0;
  double min = 0.0;
  double max = 0.0;
};

struct PreviewBenchSummary {
  bool ok = false;
  std::string error;
  PreviewBenchMode mode = PreviewBenchMode::kFull;
  PreviewBenchMeasurement measurement = PreviewBenchMeasurement::kWorker;
  std::string input_path;
  size_t input_bytes = 0;
  size_t output_xsize = 0;
  size_t output_ysize = 0;
  size_t frame_count = 0;
  uint32_t num_channels = 0;
  size_t decoded_bytes = 0;
  JxlFrameEncoding frame_encoding = JXL_FRAME_ENCODING_UNKNOWN;
  jxl::extras::JXLPreviewBackend preview_backend =
      jxl::extras::JXLPreviewBackend::kNone;
  PreviewBenchStats wall_time_ms;
  PreviewBenchStats cpu_time_ms;
  size_t source_xsize = 0;
  size_t source_ysize = 0;
  uint64_t decoder_peak_bytes = 0;
  uint64_t decoder_total_bytes = 0;
  uint64_t decoder_num_allocations = 0;
  // Maximum over the runs; available only if every run reported it.
  bool has_process_peak_working_set = false;
  uint64_t process_peak_working_set_bytes = 0;
  bool has_process_peak_private = false;
  uint64_t process_peak_private_bytes = 0;
  size_t effective_num_threads = 0;
  double throughput_mpx_per_s = 0.0;
  std::vector<PreviewBenchRun> runs;
};

struct PreviewBenchQuality {
  bool available = false;
  std::string note;
  // How each preview pixel is compared with the full decode: "boxes" (the
  // average of the f x f box the decoder averages, clipped to the image) or
  // "proportional" (the preview is not the image size divided by f, rounded
  // up, as an embedded preview frame of another size would be, so the box is
  // scaled to the preview size).
  std::string reference;
  size_t compared_samples = 0;
  double rmse = 0.0;
  double mae = 0.0;
  double max_abs = 0.0;
};

struct PreviewBenchComparison {
  std::string input_path;
  PreviewBenchSummary full;
  PreviewBenchSummary preview;
  PreviewBenchQuality quality;
  double wall_time_reduction_pct = 0.0;
  double cpu_time_reduction_pct = 0.0;
  double decoded_bytes_reduction_pct = 0.0;
  double decoder_peak_reduction_pct = 0.0;
  double decoder_num_allocations_reduction_pct = 0.0;
  // Meaningful only if both summaries have the corresponding peak.
  double process_peak_working_set_reduction_pct = 0.0;
  double process_peak_private_reduction_pct = 0.0;
  double wall_time_speedup = 0.0;
  double cpu_time_speedup = 0.0;
};

struct PreviewBenchImage {
  jxl::extras::PackedPixelFile ppf;
  size_t decoded_bytes = 0;
  jxl::extras::JXLPreviewBackend preview_backend =
      jxl::extras::JXLPreviewBackend::kNone;
};

// Process memory high-water marks of the calling process, as far as the OS
// reports them: peak working set (resident set) on Windows, Linux and macOS,
// peak private (committed) bytes on Windows only.
struct ProcessPeakMemory {
  bool has_working_set = false;
  uint64_t working_set_bytes = 0;
  bool has_private = false;
  uint64_t private_bytes = 0;
};

const char* PreviewBenchModeName(PreviewBenchMode mode);
const char* PreviewBenchMeasurementName(PreviewBenchMeasurement measurement);
const char* PreviewBackendName(jxl::extras::JXLPreviewBackend backend);
const char* FrameEncodingName(JxlFrameEncoding encoding);

// Returns true if out-of-process workers are supported on this platform
// (Linux, macOS, Windows).
bool PreviewBenchHasWorker();

// Returns true if preview_bench_worker can be run: the platform supports it
// and the executable is next to the calling program. Otherwise, *error says
// why.
bool PreviewBenchWorkerAvailable(std::string* error);

ProcessPeakMemory CurrentProcessPeakMemory();

// Whole-file reads and writes, which fail cleanly when the file cannot be
// opened (ReadFile and WriteFile of tools/file_io.h crash then).
bool ReadPreviewBenchFile(const std::string& pathname,
                          std::vector<uint8_t>* bytes);
bool WritePreviewBenchFile(const std::string& pathname, const void* data,
                           size_t size);

// Pixel formats with 1 to 4 channels of `data_type`.
std::vector<JxlPixelFormat> AcceptedFormats(
    JxlDataType data_type = JXL_TYPE_UINT8);
double CurrentCpuTimeMs();

bool ExpandJxlInputs(const std::vector<std::string>& roots,
                     std::vector<std::string>* inputs, std::string* error);

// The decoder thread count for `compressed`, the same in both modes:
// options.num_threads, or if that is 0 the library default, lowered for images
// with few groups.
size_t PreviewBenchEffectiveNumThreads(const std::vector<uint8_t>& compressed,
                                       const PreviewBenchOptions& options);

// Command-line parsing shared by preview_bench and preview_bench_worker, whose
// options are passed as "--name VALUE" or "--name=VALUE".
// Returns whether `arg` is the option `name` ("--threads" or "--threads=4" for
// "--threads", but not "--threadsX").
bool IsPreviewBenchOption(const std::string& arg, const std::string& name);
// Reads the value of the option at argv[*index]: what follows its '=', or else
// the next argument, which *index then points to. Returns false if there is no
// next argument.
bool ParsePreviewBenchOptionValue(int argc, const char* argv[], int* index,
                                  std::string* value);

// Decodes `pathname` as displayed (oriented), in 8-bit sRGB.
bool DecodeJxlForPreviewBenchmark(const std::string& pathname,
                                  const PreviewBenchOptions& options,
                                  PreviewBenchMode mode,
                                  PreviewBenchImage* image, std::string* error);

bool BenchmarkJxlFile(const std::string& pathname,
                      const PreviewBenchOptions& options, PreviewBenchMode mode,
                      PreviewBenchSummary* summary);

bool BenchmarkJxlComparison(const std::string& pathname,
                            const PreviewBenchOptions& options,
                            PreviewBenchComparison* comparison);

std::string FormatComparisonText(const PreviewBenchComparison& comparison);
std::string FormatSummaryText(const PreviewBenchSummary& summary);

bool WriteComparisonsCsv(const std::vector<PreviewBenchComparison>& comparisons,
                         const std::string& pathname, std::string* error);
bool WriteComparisonsJson(
    const std::vector<PreviewBenchComparison>& comparisons,
    const std::string& pathname, std::string* error);

}  // namespace tools
}  // namespace jpegxl

#endif  // TOOLS_PREVIEW_BENCHMARK_BENCHMARK_CORE_H_
