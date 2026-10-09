// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "lib/extras/enc/encode.h"
#include "lib/jxl/base/printf_macros.h"
#include "tools/cmdline.h"
#include "tools/preview_benchmark/benchmark_core.h"

namespace {

using jpegxl::tools::IsPreviewBenchOption;
using jpegxl::tools::ParsePreviewBenchOptionValue;
using jpegxl::tools::PreviewBenchMeasurement;

struct Args {
  jpegxl::tools::PreviewBenchOptions options;
  std::string csv_path;
  std::string json_path;
  std::string save_previews_dir;
  std::vector<std::string> inputs;
};

// Prints the help to `out`: stdout when asked for, stderr after an error.
void PrintUsage(FILE* out, const char* program) {
  fprintf(
      out,
      "Usage: %s [OPTIONS] INPUT...\n"
      "Benchmarks JPEG XL full decode against preview decode.\n\n"
      "Each decode runs in its own preview_bench_worker process, which\n"
      "must be next to this program; the process memory peaks are the\n"
      "worker's.\n\n"
      "Options:\n"
      "  --preview_downsampling N  Preview scale factor (1, 2, 4, or 8; 1 is\n"
      "                            the full decode). Default: 4\n"
      "  --iterations N            Benchmark repetitions per mode. 0 = save "
      "previews\n"
      "                            only (no timing; needs --save-previews).\n"
      "                            Default: 5\n"
      "  --threads N               Decoder threads, the same for both modes.\n"
      "                            0 = the library default, lowered for\n"
      "                            images with few groups. Default: 0\n"
      "  --in_process              Decode in this process instead of in\n"
      "                            preview_bench_worker processes. The\n"
      "                            results are tagged in_process and have no\n"
      "                            process memory peaks.\n"
      "  --csv PATH                Write batch results as CSV.\n"
      "  --json PATH               Write batch results as JSON.\n"
      "  --save-previews DIR       Save the previews to DIR, as PNG if PNG\n"
      "                            support is built in, else as PGM/PPM/PAM.\n"
      "                            With --iterations=0 skips timing entirely.\n"
      "  -h, --help                Show this help.\n",
      program);
}

// Parses the value of the option at argv[*index] (see
// ParsePreviewBenchOptionValue) as cjxl and djxl parse unsigned values: a
// minus sign, an empty value, trailing characters and overflow are errors.
bool ParseUnsignedOption(int argc, const char* argv[], int* index,
                         size_t* value) {
  std::string text;
  return ParsePreviewBenchOptionValue(argc, argv, index, &text) &&
         jpegxl::tools::ParseUnsigned(text.c_str(), value);
}

// Parses the value of the option at argv[*index] as a path, which cannot be
// empty.
bool ParsePathOption(int argc, const char* argv[], int* index,
                     std::string* path) {
  return ParsePreviewBenchOptionValue(argc, argv, index, path) &&
         !path->empty();
}

bool ParseArgs(int argc, const char* argv[], Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      PrintUsage(stdout, argv[0]);
      std::exit(EXIT_SUCCESS);
    } else if (IsPreviewBenchOption(arg, "--preview_downsampling")) {
      size_t& factor = args->options.preview_downsampling;
      if (!ParseUnsignedOption(argc, argv, &i, &factor) ||
          (factor != 1 && factor != 2 && factor != 4 && factor != 8)) {
        fprintf(stderr,
                "Invalid value for --preview_downsampling: must be 1, 2, 4 "
                "or 8\n");
        return false;
      }
    } else if (IsPreviewBenchOption(arg, "--iterations")) {
      if (!ParseUnsignedOption(argc, argv, &i, &args->options.num_reps)) {
        fprintf(stderr, "Invalid value for --iterations\n");
        return false;
      }
    } else if (IsPreviewBenchOption(arg, "--threads")) {
      if (!ParseUnsignedOption(argc, argv, &i, &args->options.num_threads)) {
        fprintf(stderr, "Invalid value for --threads\n");
        return false;
      }
    } else if (arg == "--in_process") {
      args->options.measurement = PreviewBenchMeasurement::kInProcess;
    } else if (IsPreviewBenchOption(arg, "--csv")) {
      if (!ParsePathOption(argc, argv, &i, &args->csv_path)) {
        fprintf(stderr, "Invalid value for --csv\n");
        return false;
      }
    } else if (IsPreviewBenchOption(arg, "--json")) {
      if (!ParsePathOption(argc, argv, &i, &args->json_path)) {
        fprintf(stderr, "Invalid value for --json\n");
        return false;
      }
    } else if (IsPreviewBenchOption(arg, "--save-previews")) {
      if (!ParsePathOption(argc, argv, &i, &args->save_previews_dir)) {
        fprintf(stderr, "Invalid value for --save-previews\n");
        return false;
      }
    } else if (!arg.empty() && arg[0] == '-') {
      fprintf(stderr, "Unknown option: %s\n", arg.c_str());
      return false;
    } else {
      args->inputs.emplace_back(arg);
    }
  }

  if (args->inputs.empty()) {
    fprintf(stderr, "No input files or directories were provided.\n");
    return false;
  }
  if (args->options.num_reps == 0 && args->save_previews_dir.empty()) {
    fprintf(stderr,
            "--iterations=0 times nothing and only saves previews: it needs "
            "--save-previews.\n");
    return false;
  }
  return true;
}

// The extension and encoder a preview with `num_channels` is saved with.
std::unique_ptr<jxl::extras::Encoder> PreviewEncoder(uint32_t num_channels,
                                                     std::string* extension) {
  *extension = ".png";
  std::unique_ptr<jxl::extras::Encoder> encoder =
      jxl::extras::Encoder::FromExtension(*extension);
  if (encoder != nullptr) return encoder;
  // Without PNG support, the netpbm format for the number of channels.
  *extension = num_channels == 1 ? ".pgm" : num_channels == 3 ? ".ppm" : ".pam";
  return jxl::extras::Encoder::FromExtension(*extension);
}

// Decodes the preview of `input` for display and saves its first frame to
// `dir`. Prints an "[ OK ]" line to stdout or a "[FAIL]" line to stderr.
bool SavePreview(const std::string& input,
                 const jpegxl::tools::PreviewBenchOptions& options,
                 const std::filesystem::path& dir) {
  const size_t factor = options.preview_downsampling;
  const std::string name = std::filesystem::path(input).filename().string();
  const auto fail = [&](const char* stage, const std::string& error) {
    fprintf(stderr, "[FAIL] ds%" PRIuS " %s: %s: %s\n", factor, name.c_str(),
            stage, error.c_str());
    return false;
  };

  jpegxl::tools::PreviewBenchImage image;
  std::string error;
  if (!jpegxl::tools::DecodeJxlForPreviewBenchmark(
          input, options, jpegxl::tools::PreviewBenchMode::kPreview, &image,
          &error)) {
    return fail("decode", error);
  }
  jxl::extras::PackedPixelFile& ppf = image.ppf;
  if (ppf.frames.empty()) return fail("decode", "no frame was decoded");
  // Only the first frame is saved.
  ppf.frames.erase(ppf.frames.begin() + 1, ppf.frames.end());
  ppf.info.have_animation = JXL_FALSE;
  const jxl::extras::PackedImage& color = ppf.frames.front().color;

  std::string extension;
  const std::unique_ptr<jxl::extras::Encoder> encoder =
      PreviewEncoder(color.format.num_channels, &extension);
  if (encoder == nullptr) {
    return fail("encode", "no encoder for " + extension);
  }
  jxl::extras::EncodedImage encoded;
  if (!encoder->Encode(ppf, &encoded, nullptr) || encoded.bitstreams.empty()) {
    return fail("encode", extension.substr(1) + " encoding failed");
  }

  const std::filesystem::path out_path =
      dir / (std::filesystem::path(input).stem().string() + "_ds" +
             std::to_string(factor) + extension);
  const std::vector<uint8_t>& bytes = encoded.bitstreams[0];
  if (!jpegxl::tools::WritePreviewBenchFile(out_path.string(), bytes.data(),
                                            bytes.size())) {
    return fail("write", "cannot write " + out_path.string());
  }
  fprintf(stdout,
          "[ OK ] ds%" PRIuS " %s -> %s  (%" PRIuS "x%" PRIuS
          ", ch=%u, backend=%s)\n",
          factor, name.c_str(), out_path.string().c_str(), color.xsize,
          color.ysize, color.format.num_channels,
          jpegxl::tools::PreviewBackendName(image.preview_backend));
  fflush(stdout);
  return true;
}

}  // namespace

int main(int argc, const char* argv[]) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    PrintUsage(stderr, argv[0]);
    return EXIT_FAILURE;
  }

  std::vector<std::string> inputs;
  std::string error;
  if (!jpegxl::tools::ExpandJxlInputs(args.inputs, &inputs, &error)) {
    fprintf(stderr, "%s\n", error.c_str());
    return EXIT_FAILURE;
  }

  // When --iterations=0, skip timing entirely and only save previews.
  const bool bench_enabled = (args.options.num_reps > 0);
  if (bench_enabled &&
      args.options.measurement == PreviewBenchMeasurement::kWorker &&
      !jpegxl::tools::PreviewBenchWorkerAvailable(&error)) {
    fprintf(stderr,
            "Cannot measure in preview_bench_worker processes: %s.\n"
            "Build preview_bench_worker next to preview_bench, or pass "
            "--in_process to decode in this process (without process memory "
            "peaks).\n",
            error.c_str());
    return EXIT_FAILURE;
  }

  std::vector<jpegxl::tools::PreviewBenchComparison> comparisons;
  bool ok = true;
  if (bench_enabled) {
    comparisons.reserve(inputs.size());
    for (const std::string& input : inputs) {
      jpegxl::tools::PreviewBenchComparison comparison;
      if (!jpegxl::tools::BenchmarkJxlComparison(input, args.options,
                                                 &comparison)) {
        for (const jpegxl::tools::PreviewBenchSummary* summary :
             {&comparison.full, &comparison.preview}) {
          if (!summary->error.empty()) {
            fprintf(stderr, "Benchmark failed for %s (%s decode): %s\n",
                    input.c_str(),
                    jpegxl::tools::PreviewBenchModeName(summary->mode),
                    summary->error.c_str());
          }
        }
        ok = false;
        continue;
      }
      comparisons.emplace_back(std::move(comparison));
      fprintf(stdout, "%s\n\n",
              jpegxl::tools::FormatComparisonText(comparisons.back()).c_str());
    }
  }

  if (!args.csv_path.empty() &&
      !jpegxl::tools::WriteComparisonsCsv(comparisons, args.csv_path, &error)) {
    fprintf(stderr, "%s\n", error.c_str());
    ok = false;
  }
  if (!args.json_path.empty() && !jpegxl::tools::WriteComparisonsJson(
                                     comparisons, args.json_path, &error)) {
    fprintf(stderr, "%s\n", error.c_str());
    ok = false;
  }

  if (!args.save_previews_dir.empty()) {
    const std::filesystem::path dir(args.save_previews_dir);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
      fprintf(stderr, "Cannot create %s: %s\n", args.save_previews_dir.c_str(),
              ec.message().c_str());
      return EXIT_FAILURE;
    }
    for (const std::string& input : inputs) {
      if (!SavePreview(input, args.options, dir)) ok = false;
    }
  }

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
