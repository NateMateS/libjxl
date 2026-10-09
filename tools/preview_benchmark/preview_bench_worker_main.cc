// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <jxl/thread_parallel_runner.h>
#include <jxl/thread_parallel_runner_cxx.h>

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "lib/extras/dec/jxl.h"
#include "lib/extras/packed_image.h"
#include "lib/extras/time.h"
#include "tools/cmdline.h"
#include "tools/preview_benchmark/benchmark_core.h"
#include "tools/tracking_memory_manager.h"

namespace {

using jpegxl::tools::AcceptedFormats;
using jpegxl::tools::CurrentCpuTimeMs;
using jpegxl::tools::IsPreviewBenchOption;
using jpegxl::tools::ParsePreviewBenchOptionValue;
using jpegxl::tools::ParseUnsigned;
using jpegxl::tools::PreviewBackendName;
using jpegxl::tools::PreviewBenchMode;

struct Args {
  jpegxl::tools::PreviewBenchOptions options;
  std::string input_path;
  std::string result_path;
  PreviewBenchMode mode = PreviewBenchMode::kFull;
};

bool ParseArgs(int argc, const char* argv[], Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (IsPreviewBenchOption(arg, "--mode")) {
      std::string value;
      if (!ParsePreviewBenchOptionValue(argc, argv, &i, &value)) return false;
      if (value == "full") {
        args->mode = PreviewBenchMode::kFull;
      } else if (value == "preview") {
        args->mode = PreviewBenchMode::kPreview;
      } else {
        return false;
      }
    } else if (IsPreviewBenchOption(arg, "--input")) {
      if (!ParsePreviewBenchOptionValue(argc, argv, &i, &args->input_path)) {
        return false;
      }
    } else if (IsPreviewBenchOption(arg, "--result")) {
      if (!ParsePreviewBenchOptionValue(argc, argv, &i, &args->result_path)) {
        return false;
      }
    } else if (IsPreviewBenchOption(arg, "--threads")) {
      std::string value;
      if (!ParsePreviewBenchOptionValue(argc, argv, &i, &value) ||
          !ParseUnsigned(value.c_str(), &args->options.num_threads)) {
        return false;
      }
    } else if (IsPreviewBenchOption(arg, "--preview_downsampling")) {
      std::string value;
      if (!ParsePreviewBenchOptionValue(argc, argv, &i, &value) ||
          !ParseUnsigned(value.c_str(), &args->options.preview_downsampling)) {
        return false;
      }
    } else if (IsPreviewBenchOption(arg, "--color_space")) {
      if (!ParsePreviewBenchOptionValue(argc, argv, &i,
                                        &args->options.color_space)) {
        return false;
      }
    } else {
      return false;
    }
  }
  return !args->input_path.empty() && !args->result_path.empty();
}

bool WriteResult(const std::string& path, bool ok, const std::string& error,
                 const jpegxl::tools::PreviewBenchRun& run) {
  const std::string sanitized_error = std::string(error.begin(), error.end());
  std::ostringstream out;
  // The timings reach the parent exactly.
  out << std::setprecision(std::numeric_limits<double>::max_digits10);
  out << "ok=" << (ok ? "1" : "0") << '\n';
  out << "error=";
  for (char c : sanitized_error) out << (c == '\n' || c == '\r' ? ' ' : c);
  out << '\n';
  out << "wall_time_ms=" << run.wall_time_ms << '\n';
  out << "cpu_time_ms=" << run.cpu_time_ms << '\n';
  out << "decoder_peak_bytes=" << run.decoder_peak_bytes << '\n';
  out << "decoder_total_bytes=" << run.decoder_total_bytes << '\n';
  out << "decoder_num_allocations=" << run.decoder_num_allocations << '\n';
  out << "decoded_bytes=" << run.decoded_bytes << '\n';
  out << "output_xsize=" << run.output_xsize << '\n';
  out << "output_ysize=" << run.output_ysize << '\n';
  out << "frame_count=" << run.frame_count << '\n';
  out << "num_channels=" << run.num_channels << '\n';
  out << "preview_backend=" << PreviewBackendName(run.preview_backend) << '\n';
  if (run.has_process_peak_working_set) {
    out << "process_peak_working_set_bytes="
        << run.process_peak_working_set_bytes << '\n';
  }
  if (run.has_process_peak_private) {
    out << "process_peak_private_bytes=" << run.process_peak_private_bytes
        << '\n';
  }
  const std::string text = out.str();
  return jpegxl::tools::WritePreviewBenchFile(path, text.data(), text.size());
}

}  // namespace

int main(int argc, const char* argv[]) {
  Args args;
  jpegxl::tools::PreviewBenchRun run;
  if (!ParseArgs(argc, argv, &args)) {
    WriteResult(args.result_path, false, "Invalid arguments", run);
    return EXIT_FAILURE;
  }

  std::vector<uint8_t> compressed;
  if (!jpegxl::tools::ReadPreviewBenchFile(args.input_path, &compressed)) {
    WriteResult(args.result_path, false, "Failed to read input file", run);
    return EXIT_FAILURE;
  }

  auto runner = JxlThreadParallelRunnerMake(
      nullptr,
      jpegxl::tools::PreviewBenchEffectiveNumThreads(compressed, args.options));
  if (!runner) {
    WriteResult(args.result_path, false, "Failed to create thread runner", run);
    return EXIT_FAILURE;
  }

  jpegxl::tools::TrackingMemoryManager memory_manager;
  // ppf is scoped so its destructor runs *before* memory_manager.Reset().
  // Today PackedPixelFile's storage uses the system allocator, but if a
  // future change routes any of it through dparams.memory_manager, this
  // ordering ensures Reset() sees a clean ledger and ppf's frees still
  // hit a live tracking map.
  {
    jxl::extras::PackedPixelFile ppf;
    jxl::extras::JXLDecompressParams dparams;
    dparams.accepted_formats = AcceptedFormats();
    dparams.color_space = args.options.color_space;
    dparams.runner = JxlThreadParallelRunner;
    dparams.runner_opaque = runner.get();
    dparams.memory_manager = memory_manager.get();
    dparams.unpremultiply_alpha = true;
    dparams.use_image_callback = false;
    if (args.mode == PreviewBenchMode::kPreview) {
      dparams.preview_downsampling = args.options.preview_downsampling;
      dparams.preview_backend = &run.preview_backend;
      dparams.preview_hooks = jxl::GetDecoderPreviewHooks();
    }

    const double before_cpu_ms = CurrentCpuTimeMs();
    const double before_wall_s = jxl::Now();
    if (!jxl::extras::DecodeImageJXL(compressed.data(), compressed.size(),
                                     dparams, &run.decoded_bytes, &ppf)) {
      WriteResult(args.result_path, false, "DecodeImageJXL failed", run);
      return EXIT_FAILURE;
    }
    const double after_wall_s = jxl::Now();
    const double after_cpu_ms = CurrentCpuTimeMs();

    run.ok = true;
    run.wall_time_ms = (after_wall_s - before_wall_s) * 1000.0;
    run.cpu_time_ms = std::max(0.0, after_cpu_ms - before_cpu_ms);
    run.decoder_peak_bytes = memory_manager.max_bytes_in_use;
    run.decoder_total_bytes = memory_manager.total_bytes_allocated;
    run.decoder_num_allocations = memory_manager.total_allocations;
    run.output_xsize = ppf.info.xsize;
    run.output_ysize = ppf.info.ysize;
    run.frame_count = ppf.frames.size();
    run.num_channels =
        ppf.frames.empty() ? 0 : ppf.frames.front().color.format.num_channels;
  }

  if (!memory_manager.Reset()) {
    WriteResult(args.result_path, false, "Decoder leaked tracked allocations",
                run);
    return EXIT_FAILURE;
  }

  // The OS high-water marks of this process, which has done nothing but this
  // decode.
  const jpegxl::tools::ProcessPeakMemory peak =
      jpegxl::tools::CurrentProcessPeakMemory();
  run.has_process_peak_working_set = peak.has_working_set;
  run.process_peak_working_set_bytes = peak.working_set_bytes;
  run.has_process_peak_private = peak.has_private;
  run.process_peak_private_bytes = peak.private_bytes;

  if (!WriteResult(args.result_path, true, "", run)) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
