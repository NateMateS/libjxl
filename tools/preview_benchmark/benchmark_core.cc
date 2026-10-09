// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "tools/preview_benchmark/benchmark_core.h"

#include <jxl/decode.h>
#include <jxl/resizable_parallel_runner.h>
#include <jxl/thread_parallel_runner.h>
#include <jxl/thread_parallel_runner_cxx.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "lib/extras/time.h"
#include "lib/jxl/base/os_macros.h"
#include "lib/jxl/dec_preview_internal.h"
#include "tools/tracking_memory_manager.h"

#if JXL_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// psapi.h uses the types windows.h defines.
#include <psapi.h>
#elif !JXL_OS_HAIKU
#include <sys/resource.h>
#include <sys/time.h>
#endif

// Out-of-process workers: Windows, Linux and macOS (not iOS, which has no
// process spawning).
#if JXL_OS_WIN || JXL_OS_LINUX || (JXL_OS_MAC && !JXL_OS_IOS)
#define JXL_PREVIEW_BENCH_HAS_WORKER 1
#else
#define JXL_PREVIEW_BENCH_HAS_WORKER 0
#endif

#if JXL_PREVIEW_BENCH_HAS_WORKER && !JXL_OS_WIN
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <ctime>
#if JXL_OS_MAC
#include <crt_externs.h>
#include <mach-o/dyld.h>
#endif
#endif

namespace jpegxl {
namespace tools {

namespace {

template <typename Getter>
PreviewBenchStats ComputeStats(const std::vector<PreviewBenchRun>& runs,
                               Getter getter) {
  PreviewBenchStats stats;
  if (runs.empty()) return stats;
  stats.min = getter(runs.front());
  stats.max = stats.min;
  double total = 0.0;
  for (const PreviewBenchRun& run : runs) {
    const double value = getter(run);
    stats.min = std::min(stats.min, value);
    stats.max = std::max(stats.max, value);
    total += value;
  }
  stats.mean = total / runs.size();
  if (runs.size() > 1) {
    double sq_sum = 0.0;
    for (const PreviewBenchRun& run : runs) {
      const double diff = getter(run) - stats.mean;
      sq_sum += diff * diff;
    }
    stats.stddev = std::sqrt(sq_sum / (runs.size() - 1));
  }
  return stats;
}

std::string ToLower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return text;
}

bool IsJxlPath(const std::filesystem::path& path) {
  return ToLower(path.extension().string()) == ".jxl";
}

double PercentReduction(double baseline, double candidate) {
  if (baseline <= 0.0) return 0.0;
  return 100.0 * (1.0 - candidate / baseline);
}

double Speedup(double baseline, double candidate) {
  if (candidate <= 0.0) return 0.0;
  return baseline / candidate;
}

bool HasWorkingSetPeaks(const PreviewBenchComparison& comparison) {
  return comparison.full.has_process_peak_working_set &&
         comparison.preview.has_process_peak_working_set;
}

bool HasPrivatePeaks(const PreviewBenchComparison& comparison) {
  return comparison.full.has_process_peak_private &&
         comparison.preview.has_process_peak_private;
}

std::string JsonEscape(const std::string& text) {
  std::ostringstream os;
  for (unsigned char c : text) {
    switch (c) {
      case '\\':
        os << "\\\\";
        break;
      case '"':
        os << "\\\"";
        break;
      case '\b':
        os << "\\b";
        break;
      case '\f':
        os << "\\f";
        break;
      case '\n':
        os << "\\n";
        break;
      case '\r':
        os << "\\r";
        break;
      case '\t':
        os << "\\t";
        break;
      default:
        if (c < 0x20) {
          os << "\\u" << std::hex << std::setw(4) << std::setfill('0')
             << static_cast<int>(c) << std::dec << std::setfill(' ');
        } else {
          os << static_cast<char>(c);
        }
        break;
    }
  }
  return os.str();
}

std::string CsvEscape(const std::string& text) {
  if (text.find_first_of(",\"\n\r") == std::string::npos) {
    return text;
  }
  std::string escaped = "\"";
  for (char c : text) {
    if (c == '"') escaped.push_back('"');
    escaped.push_back(c);
  }
  escaped.push_back('"');
  return escaped;
}

// A process memory value for CSV and JSON output: the number, or `missing`
// ("n/a" in CSV, null in JSON) when it was not measured. Byte counts stay
// integers (a double would print 6 significant digits).
template <typename T>
std::string OptionalNumber(bool available, T value, const char* missing) {
  if (!available) return missing;
  std::ostringstream os;
  os << value;
  return os.str();
}

std::string HumanBytes(uint64_t bytes) {
  static const char* kUnits[] = {"B", "KiB", "MiB", "GiB"};
  double value = static_cast<double>(bytes);
  size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < sizeof(kUnits) / sizeof(kUnits[0])) {
    value /= 1024.0;
    ++unit;
  }
  std::ostringstream os;
  os << std::fixed << std::setprecision(unit == 0 ? 0 : 2) << value << ' '
     << kUnits[unit];
  return os.str();
}

// The text output is UTF-8. Non-ASCII characters are written as byte escapes,
// which MSVC, unlike "\u" escapes, does not convert to the code page when it
// compiles without /utf-8.
std::string FormatPctDelta(double pct) {
  std::ostringstream os;
  os << std::fixed << std::setprecision(1);
  if (pct > 0.05) {
    os << "\xE2\x86\x93 " << pct << " %";  // ↓
  } else if (pct < -0.05) {
    os << "\xE2\x86\x91 " << std::abs(pct) << " %";  // ↑
  } else {
    os << "\xE2\x80\x94";  // —
  }
  return os.str();
}

std::string SummaryRange(const PreviewBenchStats& stats) {
  std::ostringstream os;
  os << std::fixed << std::setprecision(2) << stats.mean;
  if (stats.stddev > 0.0) {
    os << " \xC2\xB1 " << stats.stddev;  // ±
  }
  os << " ms";
  if (stats.min != stats.max) {
    os << "  [" << stats.min << " \xE2\x80\x93 " << stats.max << "]";  // –
  }
  return os.str();
}

size_t EffectiveThreadCountOrDefault(size_t requested_threads) {
  return requested_threads == 0
             ? JxlThreadParallelRunnerDefaultNumWorkerThreads()
             : requested_threads;
}

JxlFrameEncoding DetectFrameEncoding(const std::vector<uint8_t>& compressed) {
  JxlDecoder* dec = JxlDecoderCreate(nullptr);
  if (dec == nullptr) return JXL_FRAME_ENCODING_UNKNOWN;
  JxlFrameEncoding result = JXL_FRAME_ENCODING_UNKNOWN;
  if (JXL_DEC_SUCCESS == JxlDecoderSubscribeEvents(dec, JXL_DEC_FRAME)) {
    if (JXL_DEC_SUCCESS ==
        JxlDecoderSetInput(dec, compressed.data(), compressed.size())) {
      for (;;) {
        const JxlDecoderStatus status = JxlDecoderProcessInput(dec);
        if (status == JXL_DEC_FRAME) {
          jxl::GetDecoderPreviewHooks()->get_frame_encoding(dec, &result);
          break;
        }
        if (status == JXL_DEC_SUCCESS || status == JXL_DEC_ERROR ||
            status == JXL_DEC_NEED_MORE_INPUT) {
          break;
        }
      }
    }
  }
  JxlDecoderDestroy(dec);
  return result;
}

void ProbeImageDimensions(const std::vector<uint8_t>& compressed, size_t* xsize,
                          size_t* ysize) {
  if (xsize == nullptr || ysize == nullptr) return;
  *xsize = 0;
  *ysize = 0;
  JxlDecoder* dec = JxlDecoderCreate(nullptr);
  if (dec == nullptr) return;
  if (JXL_DEC_SUCCESS == JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO)) {
    if (JXL_DEC_SUCCESS ==
        JxlDecoderSetInput(dec, compressed.data(), compressed.size())) {
      for (;;) {
        const JxlDecoderStatus status = JxlDecoderProcessInput(dec);
        if (status == JXL_DEC_BASIC_INFO) {
          JxlBasicInfo info = {};
          if (JXL_DEC_SUCCESS == JxlDecoderGetBasicInfo(dec, &info)) {
            *xsize = info.xsize;
            *ysize = info.ysize;
          }
          break;
        }
        if (status == JXL_DEC_SUCCESS || status == JXL_DEC_ERROR ||
            status == JXL_DEC_NEED_MORE_INPUT) {
          break;
        }
      }
    }
  }
  JxlDecoderDestroy(dec);
}

}  // namespace

bool PreviewBenchHasWorker() {
#if JXL_PREVIEW_BENCH_HAS_WORKER
  return true;
#else
  return false;
#endif
}

bool ReadPreviewBenchFile(const std::string& pathname,
                          std::vector<uint8_t>* bytes) {
  std::ifstream file(pathname, std::ios::binary | std::ios::ate);
  if (!file) return false;
  const std::streamoff size = file.tellg();
  if (size < 0) return false;
  bytes->resize(static_cast<size_t>(size));
  file.seekg(0);
  return size == 0 || static_cast<bool>(file.read(
                          reinterpret_cast<char*>(bytes->data()), size));
}

bool WritePreviewBenchFile(const std::string& pathname, const void* data,
                           size_t size) {
  std::ofstream file(pathname, std::ios::binary | std::ios::trunc);
  if (!file) return false;
  file.write(static_cast<const char*>(data),
             static_cast<std::streamsize>(size));
  file.close();
  return !file.fail();
}

ProcessPeakMemory CurrentProcessPeakMemory() {
  ProcessPeakMemory peak;
#if JXL_OS_WIN
  PROCESS_MEMORY_COUNTERS_EX counters = {};
  if (GetProcessMemoryInfo(
          GetCurrentProcess(),
          reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
          sizeof(counters))) {
    peak.has_working_set = true;
    peak.working_set_bytes = counters.PeakWorkingSetSize;
    // Peak commit charge of the process: its private bytes.
    peak.has_private = true;
    peak.private_bytes = counters.PeakPagefileUsage;
  }
#elif JXL_OS_LINUX
  // VmHWM is the high-water mark of the current address space. ru_maxrss is
  // not used: Linux carries it over an execve(), so for a process started with
  // posix_spawn() it includes the peak of the parent that spawned it.
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.rfind("VmHWM:", 0) != 0) continue;
    std::istringstream fields(line.substr(6));
    uint64_t kib = 0;
    std::string unit;
    if (fields >> kib >> unit && unit == "kB") {
      peak.has_working_set = true;
      peak.working_set_bytes = kib * 1024;
    }
    break;
  }
#elif JXL_OS_MAC
  rusage usage = {};
  if (getrusage(RUSAGE_SELF, &usage) == 0 && usage.ru_maxrss > 0) {
    peak.has_working_set = true;
    // In bytes on macOS.
    peak.working_set_bytes = static_cast<uint64_t>(usage.ru_maxrss);
  }
#endif
  return peak;
}

std::vector<JxlPixelFormat> AcceptedFormats(JxlDataType data_type) {
  return {
      {4, data_type, JXL_LITTLE_ENDIAN, 0},
      {3, data_type, JXL_LITTLE_ENDIAN, 0},
      {2, data_type, JXL_LITTLE_ENDIAN, 0},
      {1, data_type, JXL_LITTLE_ENDIAN, 0},
  };
}

double CurrentCpuTimeMs() {
#if JXL_OS_WIN
  FILETIME creation_time;
  FILETIME exit_time;
  FILETIME kernel_time;
  FILETIME user_time;
  if (!GetProcessTimes(GetCurrentProcess(), &creation_time, &exit_time,
                       &kernel_time, &user_time)) {
    return 0.0;
  }
  ULARGE_INTEGER kernel = {};
  ULARGE_INTEGER user = {};
  kernel.LowPart = kernel_time.dwLowDateTime;
  kernel.HighPart = kernel_time.dwHighDateTime;
  user.LowPart = user_time.dwLowDateTime;
  user.HighPart = user_time.dwHighDateTime;
  return static_cast<double>(kernel.QuadPart + user.QuadPart) * 1e-4;
#elif !JXL_OS_HAIKU
  rusage usage = {};
  if (getrusage(RUSAGE_SELF, &usage) != 0) return 0.0;
  const double user_ms =
      usage.ru_utime.tv_sec * 1000.0 + usage.ru_utime.tv_usec / 1000.0;
  const double sys_ms =
      usage.ru_stime.tv_sec * 1000.0 + usage.ru_stime.tv_usec / 1000.0;
  return user_ms + sys_ms;
#else
  return 0.0;
#endif
}

size_t PreviewBenchEffectiveNumThreads(const std::vector<uint8_t>& compressed,
                                       const PreviewBenchOptions& options,
                                       PreviewBenchMode mode) {
  const size_t default_threads =
      EffectiveThreadCountOrDefault(options.num_threads);
  if (options.num_threads != 0 || mode != PreviewBenchMode::kPreview) {
    return default_threads;
  }

  JxlDecoder* dec = JxlDecoderCreate(nullptr);
  if (dec == nullptr) return default_threads;
  if (JXL_DEC_SUCCESS !=
      JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_FRAME)) {
    JxlDecoderDestroy(dec);
    return default_threads;
  }
  if (JXL_DEC_SUCCESS !=
      JxlDecoderSetInput(dec, compressed.data(), compressed.size())) {
    JxlDecoderDestroy(dec);
    return default_threads;
  }
  JxlBasicInfo info = {};
  JxlFrameEncoding frame_encoding = JXL_FRAME_ENCODING_UNKNOWN;
  for (;;) {
    const JxlDecoderStatus status = JxlDecoderProcessInput(dec);
    if (status == JXL_DEC_BASIC_INFO) {
      if (JXL_DEC_SUCCESS != JxlDecoderGetBasicInfo(dec, &info)) {
        JxlDecoderDestroy(dec);
        return default_threads;
      }
      continue;
    }
    if (status == JXL_DEC_FRAME) {
      if (JXL_DEC_SUCCESS != jxl::GetDecoderPreviewHooks()->get_frame_encoding(
                                 dec, &frame_encoding)) {
        frame_encoding = JXL_FRAME_ENCODING_UNKNOWN;
      }
      break;
    }
    if (status == JXL_DEC_SUCCESS || status == JXL_DEC_ERROR ||
        status == JXL_DEC_NEED_MORE_INPUT) {
      break;
    }
  }
  if (info.xsize == 0 || info.ysize == 0) {
    JxlDecoderDestroy(dec);
    return default_threads;
  }

  // Scale threads by source image groups, not output size. The decoder visits
  // every source group regardless of the preview output dimensions; only the
  // amount of work per group is reduced for VarDCT. Using output dimensions
  // would severely under-thread VarDCT previews and still under-thread modular
  // ones due to the pixel_cap denominator.
  const size_t suggested = std::max<size_t>(
      1, JxlResizableParallelRunnerSuggestThreads(info.xsize, info.ysize));
  const size_t approx_groups =
      jxl::DivCeil(static_cast<uint64_t>(info.xsize), uint64_t{256}) *
      jxl::DivCeil(static_cast<uint64_t>(info.ysize), uint64_t{256});
  // Ensure at least 2 threads for multi-group modular images so the group
  // reader and the decoder don't serialize.
  size_t min_threads = 1;
  if (frame_encoding == JXL_FRAME_ENCODING_MODULAR && approx_groups > 1) {
    min_threads = std::min<size_t>(2, default_threads);
  }
  // Cap by group count: there is no benefit spinning up more threads than
  // there are groups to parallelize over.
  const size_t group_cap =
      std::max<size_t>(1, std::min(default_threads, approx_groups));
  JxlDecoderDestroy(dec);
  const size_t threads = std::min(
      default_threads, std::min(group_cap, std::max(suggested, min_threads)));
  return std::max<size_t>(min_threads, threads);
}

namespace {

// Process memory is not measured in-process: the calling process holds more
// than the decode (inputs, earlier results), and no high-water mark can be
// attributed to a single decode.
PreviewBenchRun RunDecodeInProcess(const std::vector<uint8_t>& compressed,
                                   const PreviewBenchOptions& options,
                                   PreviewBenchMode mode,
                                   const JxlThreadParallelRunnerPtr& runner) {
  PreviewBenchRun run;
  TrackingMemoryManager memory_manager;
  // ppf is scoped so its destructor runs *before* memory_manager.Reset().
  // Today PackedPixelFile's storage uses the system allocator, but if a
  // future change routes any of it through dparams.memory_manager, this
  // ordering ensures Reset() sees a clean ledger and ppf's frees still
  // hit a live tracking map.
  {
    jxl::extras::PackedPixelFile ppf;
    jxl::extras::JXLDecompressParams dparams;
    dparams.accepted_formats = AcceptedFormats();
    dparams.color_space = options.color_space;
    dparams.runner = JxlThreadParallelRunner;
    dparams.runner_opaque = runner.get();
    dparams.memory_manager = memory_manager.get();
    dparams.unpremultiply_alpha = true;
    dparams.use_image_callback = false;
    if (mode == PreviewBenchMode::kPreview) {
      dparams.preview_downsampling = options.preview_downsampling;
      dparams.preview_backend = &run.preview_backend;
      dparams.preview_hooks = jxl::GetDecoderPreviewHooks();
    }

    const double before_cpu_ms = CurrentCpuTimeMs();
    const double before_wall_s = jxl::Now();

    if (!jxl::extras::DecodeImageJXL(compressed.data(), compressed.size(),
                                     dparams, &run.decoded_bytes, &ppf)) {
      run.error = "DecodeImageJXL failed";
      return run;
    }

    const double after_wall_s = jxl::Now();
    const double after_cpu_ms = CurrentCpuTimeMs();

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
    run.error = "Decoder leaked tracked allocations";
    return run;
  }
  run.ok = true;
  return run;
}

bool ParseSizeTValue(const std::string& text, size_t* value) {
  if (value == nullptr) return false;
  std::istringstream is(text);
  uint64_t parsed = 0;
  is >> parsed;
  if (!is || !is.eof()) return false;
  *value = static_cast<size_t>(parsed);
  return true;
}

bool ParseUint64Value(const std::string& text, uint64_t* value) {
  if (value == nullptr) return false;
  std::istringstream is(text);
  is >> *value;
  return static_cast<bool>(is) && is.eof();
}

bool ParseDoubleValue(const std::string& text, double* value) {
  if (value == nullptr) return false;
  std::istringstream is(text);
  is >> *value;
  return static_cast<bool>(is) && is.eof();
}

bool ParseBackendValue(const std::string& text,
                       jxl::extras::JXLPreviewBackend* backend) {
  using jxl::extras::JXLPreviewBackend;
  for (JXLPreviewBackend candidate :
       {JXLPreviewBackend::kNone, JXLPreviewBackend::kEmbeddedPreview,
        JXLPreviewBackend::kNativeDcOnly,
        JXLPreviewBackend::kNativeProgressionFlush,
        JXLPreviewBackend::kFallbackDownsample,
        JXLPreviewBackend::kNativeReducedInput,
        JXLPreviewBackend::kNativeFusedUpsampling,
        JXLPreviewBackend::kDecoderDownsample}) {
    if (text == PreviewBackendName(candidate)) {
      *backend = candidate;
      return true;
    }
  }
  return false;
}

#if JXL_PREVIEW_BENCH_HAS_WORKER
std::filesystem::path WorkerExecutablePath() {
#if JXL_OS_WIN
  wchar_t module_path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, module_path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) return {};
  return std::filesystem::path(module_path).parent_path() /
         L"preview_bench_worker.exe";
#elif JXL_OS_LINUX
  char buf[4096];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return {};
  buf[n] = '\0';
  return std::filesystem::path(buf).parent_path() / "preview_bench_worker";
#elif JXL_OS_MAC
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> buf(size);
  if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
  return std::filesystem::path(buf.data()).parent_path() /
         "preview_bench_worker";
#else
  return {};
#endif
}

#if JXL_OS_WIN
std::wstring QuoteCommandArgW(const std::wstring& arg) {
  std::wstring quoted = L"\"";
  size_t backslashes = 0;
  for (wchar_t c : arg) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    if (c == L'"') {
      quoted.append(backslashes * 2 + 1, L'\\');
      quoted.push_back(L'"');
      backslashes = 0;
      continue;
    }
    quoted.append(backslashes, L'\\');
    backslashes = 0;
    quoted.push_back(c);
  }
  quoted.append(backslashes * 2, L'\\');
  quoted.push_back(L'"');
  return quoted;
}

// Narrow strings here are in the encoding std::filesystem uses for narrow
// paths, the one the worker's narrow argv is decoded with (UTF-8 with the
// tools' manifest), so converting through std::filesystem::path passes any
// character through; byte-wise widening only works for ASCII.
std::wstring ToWide(const std::string& text) {
  return std::filesystem::path(text).wstring();
}
#endif  // JXL_OS_WIN

// Empty, with *ec set, if there is no usable temporary directory (for example
// TMPDIR names a missing directory).
std::filesystem::path WorkerResultPath(std::error_code* ec) {
  const auto temp_dir = std::filesystem::temp_directory_path(*ec);
  if (*ec) return {};
  std::ostringstream name;
#if JXL_OS_WIN
  const auto pid = static_cast<uint64_t>(GetCurrentProcessId());
  const auto stamp = static_cast<uint64_t>(GetTickCount64());
#else
  const auto pid = static_cast<uint64_t>(getpid());
  struct timespec ts = {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  const auto stamp = static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
                     static_cast<uint64_t>(ts.tv_nsec);
#endif
  name << "preview_bench_worker_" << pid << "_" << stamp << ".txt";
  return temp_dir / name.str();
}

// Reads the worker's result file into *run. The run is replaced only if the
// whole file parses; run->ok is the worker's verdict.
bool ParseWorkerResultFile(const std::filesystem::path& path,
                           PreviewBenchRun* run, std::string* error) {
  std::vector<uint8_t> bytes;
  if (!ReadPreviewBenchFile(path.string(), &bytes)) {
    *error = "it wrote no result file";
    return false;
  }
  std::istringstream lines(std::string(bytes.begin(), bytes.end()));
  std::string line;
  std::map<std::string, std::string> values;
  while (std::getline(lines, line)) {
    const size_t equals = line.find('=');
    if (equals == std::string::npos) continue;
    values.emplace(line.substr(0, equals), line.substr(equals + 1));
  }
  const auto get = [&](const char* key) -> const std::string* {
    auto it = values.find(key);
    return it == values.end() ? nullptr : &it->second;
  };
  const auto bad = [&](const char* key) {
    *error = std::string("its result file has a missing or invalid ") + key;
    return false;
  };

  PreviewBenchRun parsed;
  const std::string* ok_value = get("ok");
  if (ok_value == nullptr || (*ok_value != "0" && *ok_value != "1")) {
    return bad("ok");
  }
  if (const std::string* value = get("error")) parsed.error = *value;
  if (*ok_value == "0") {
    *run = parsed;
    return true;
  }

  const auto get_double = [&](const char* key, double* out) {
    const std::string* value = get(key);
    return value != nullptr && ParseDoubleValue(*value, out);
  };
  const auto get_uint64 = [&](const char* key, uint64_t* out) {
    const std::string* value = get(key);
    return value != nullptr && ParseUint64Value(*value, out);
  };
  const auto get_size = [&](const char* key, size_t* out) {
    const std::string* value = get(key);
    return value != nullptr && ParseSizeTValue(*value, out);
  };
  if (!get_double("wall_time_ms", &parsed.wall_time_ms)) {
    return bad("wall_time_ms");
  }
  if (!get_double("cpu_time_ms", &parsed.cpu_time_ms)) {
    return bad("cpu_time_ms");
  }
  if (!get_uint64("decoder_peak_bytes", &parsed.decoder_peak_bytes)) {
    return bad("decoder_peak_bytes");
  }
  if (!get_uint64("decoder_total_bytes", &parsed.decoder_total_bytes)) {
    return bad("decoder_total_bytes");
  }
  if (!get_uint64("decoder_num_allocations", &parsed.decoder_num_allocations)) {
    return bad("decoder_num_allocations");
  }
  if (!get_size("decoded_bytes", &parsed.decoded_bytes)) {
    return bad("decoded_bytes");
  }
  if (!get_size("output_xsize", &parsed.output_xsize)) {
    return bad("output_xsize");
  }
  if (!get_size("output_ysize", &parsed.output_ysize)) {
    return bad("output_ysize");
  }
  if (!get_size("frame_count", &parsed.frame_count)) {
    return bad("frame_count");
  }
  size_t num_channels = 0;
  if (!get_size("num_channels", &num_channels)) return bad("num_channels");
  parsed.num_channels = static_cast<uint32_t>(num_channels);
  const std::string* backend = get("preview_backend");
  if (backend == nullptr ||
      !ParseBackendValue(*backend, &parsed.preview_backend)) {
    return bad("preview_backend");
  }
  // The peaks are written only where the OS reports them.
  if (get("process_peak_working_set_bytes") != nullptr) {
    if (!get_uint64("process_peak_working_set_bytes",
                    &parsed.process_peak_working_set_bytes)) {
      return bad("process_peak_working_set_bytes");
    }
    parsed.has_process_peak_working_set = true;
  }
  if (get("process_peak_private_bytes") != nullptr) {
    if (!get_uint64("process_peak_private_bytes",
                    &parsed.process_peak_private_bytes)) {
      return bad("process_peak_private_bytes");
    }
    parsed.has_process_peak_private = true;
  }
  parsed.ok = true;
  *run = parsed;
  return true;
}

// Runs one decode in a preview_bench_worker process. Any failure of the worker
// is the run's error; nothing is retried in this process.
PreviewBenchRun RunDecodeInWorker(const std::string& pathname,
                                  const PreviewBenchOptions& options,
                                  PreviewBenchMode mode) {
  PreviewBenchRun run;
  const std::filesystem::path worker = WorkerExecutablePath();
  std::error_code ec;
  if (worker.empty() || !std::filesystem::exists(worker, ec)) {
    run.error = "preview_bench_worker was not found";
    return run;
  }

  const std::filesystem::path result_path = WorkerResultPath(&ec);
  if (ec) {
    run.error =
        "No temporary directory for the worker's result: " + ec.message();
    return run;
  }
  // How the worker ended, for error messages.
  std::string ending;
  bool exited_normally = false;

#if JXL_OS_WIN
  std::wostringstream command;
  command << QuoteCommandArgW(worker.wstring()) << L" --mode "
          << PreviewBenchModeName(mode) << L" --input "
          << QuoteCommandArgW(ToWide(pathname)) << L" --result "
          << QuoteCommandArgW(result_path.wstring()) << L" --threads "
          << options.num_threads << L" --preview_downsampling "
          << options.preview_downsampling << L" --color_space "
          << QuoteCommandArgW(ToWide(options.color_space));
  std::wstring command_line = command.str();
  std::vector<wchar_t> mutable_command(command_line.begin(),
                                       command_line.end());
  mutable_command.push_back(L'\0');

  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
    run.error = "Failed to launch preview_bench_worker (Windows error " +
                std::to_string(GetLastError()) + ")";
    return run;
  }
  const DWORD wait = WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 0;
  const bool have_exit_code =
      wait == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  if (!have_exit_code) {
    run.error = "Waiting on preview_bench_worker failed";
    std::filesystem::remove(result_path, ec);
    return run;
  }
  if (exit_code == 0) {
    exited_normally = true;
  } else if (exit_code >= 0xC0000000u) {
    // An NTSTATUS error: a crash (0xC0000005), but also a failure to start,
    // such as a missing DLL (0xC0000135).
    std::ostringstream os;
    os << "terminated with NTSTATUS 0x" << std::hex << std::uppercase
       << exit_code;
    ending = os.str();
  } else {
    ending = "exited with status " + std::to_string(exit_code);
  }
#else  // POSIX (Linux / macOS)
  const std::string worker_str = worker.string();
  const std::string result_str = result_path.string();
  const std::string mode_str = PreviewBenchModeName(mode);
  const std::string threads_str = std::to_string(options.num_threads);
  const std::string downsample_str =
      std::to_string(options.preview_downsampling);

  std::vector<std::string> args = {worker_str,         "--mode",
                                   mode_str,           "--input",
                                   pathname,           "--result",
                                   result_str,         "--threads",
                                   threads_str,        "--preview_downsampling",
                                   downsample_str,     "--color_space",
                                   options.color_space};
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (auto& s : args) argv.push_back(s.data());
  argv.push_back(nullptr);

#if JXL_OS_MAC
  char** envp = *_NSGetEnviron();
#else
  char** envp = ::environ;
#endif

  pid_t pid = -1;
  const int spawn_rc = posix_spawn(&pid, worker_str.c_str(), nullptr, nullptr,
                                   argv.data(), envp);
  if (spawn_rc != 0) {
    run.error = std::string("Failed to launch preview_bench_worker: ") +
                std::strerror(spawn_rc);
    return run;
  }
  int wait_status = 0;
  pid_t waited = -1;
  do {
    waited = waitpid(pid, &wait_status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited != pid) {
    run.error = "Waiting on preview_bench_worker failed";
    std::filesystem::remove(result_path, ec);
    return run;
  }
  if (WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0) {
    exited_normally = true;
  } else if (WIFEXITED(wait_status)) {
    ending = "exited with status " + std::to_string(WEXITSTATUS(wait_status));
  } else if (WIFSIGNALED(wait_status)) {
    const int signal_number = WTERMSIG(wait_status);
    const char* signal_name = strsignal(signal_number);
    ending = "was killed by signal " + std::to_string(signal_number) +
             (signal_name != nullptr ? std::string(" (") + signal_name + ")"
                                     : std::string());
  } else {
    ending = "ended abnormally";
  }
#endif

  PreviewBenchRun parsed;
  std::string parse_error;
  const bool have_result =
      ParseWorkerResultFile(result_path, &parsed, &parse_error);
  std::filesystem::remove(result_path, ec);
  if (!exited_normally) {
    run.error = "preview_bench_worker " + ending;
    if (have_result && !parsed.error.empty()) run.error += ": " + parsed.error;
    return run;
  }
  if (!have_result) {
    run.error = "preview_bench_worker exited normally, but " + parse_error;
    return run;
  }
  if (!parsed.ok) {
    run.error =
        "preview_bench_worker: " +
        (parsed.error.empty() ? std::string("decode failed") : parsed.error);
    return run;
  }
  return parsed;
}
#endif  // JXL_PREVIEW_BENCH_HAS_WORKER

PreviewBenchRun RunDecode(const std::string& pathname,
                          const std::vector<uint8_t>& compressed,
                          const PreviewBenchOptions& options,
                          PreviewBenchMode mode,
                          const JxlThreadParallelRunnerPtr& runner) {
  if (options.measurement == PreviewBenchMeasurement::kInProcess) {
    return RunDecodeInProcess(compressed, options, mode, runner);
  }
#if JXL_PREVIEW_BENCH_HAS_WORKER
  return RunDecodeInWorker(pathname, options, mode);
#else
  PreviewBenchRun run;
  run.error =
      "preview_bench_worker is not supported on this platform; measure "
      "in-process instead";
  return run;
#endif
}

// Compares the first frame of the preview with the full decode. With
// `factor` > 0 and a preview of the full size divided by `factor` (rounded
// up), each preview pixel is compared with the average of the box the decoder
// averages: [x*f, min(x*f+f, xsize)) x [y*f, min(y*f+f, ysize)). Both images
// must be in codestream orientation, since the decoder aligns the boxes to the
// codestream origin. Other preview sizes (as an embedded preview frame of
// another size would have) are compared with proportionally scaled boxes.
PreviewBenchQuality ComputeQuality(
    const jxl::extras::PackedPixelFile& full_ppf,
    const jxl::extras::PackedPixelFile& preview_ppf, size_t factor) {
  PreviewBenchQuality quality;
  if (full_ppf.frames.empty() || preview_ppf.frames.empty()) {
    quality.note = "No frame data available for quality comparison";
    return quality;
  }

  const jxl::extras::PackedImage& full = full_ppf.frames.front().color;
  const jxl::extras::PackedImage& preview = preview_ppf.frames.front().color;
  const size_t channels =
      std::min<size_t>(full.format.num_channels, preview.format.num_channels);
  if (channels == 0 || preview.xsize == 0 || preview.ysize == 0 ||
      full.xsize == 0 || full.ysize == 0) {
    quality.note = "No comparable channels available";
    return quality;
  }

  const bool boxes = factor > 0 &&
                     preview.xsize == jxl::DivCeil(full.xsize, factor) &&
                     preview.ysize == jxl::DivCeil(full.ysize, factor);
  quality.reference = boxes ? "boxes" : "proportional";
  // Source range [*begin, *end) of preview row or column `i`.
  const auto range = [&](size_t i, size_t preview_size, size_t full_size,
                         size_t* begin, size_t* end) {
    if (boxes) {
      *begin = i * factor;
      *end = std::min(full_size, *begin + factor);
    } else {
      *begin = i * full_size / preview_size;
      *end = std::min(
          full_size,
          std::max(*begin + 1,
                   ((i + 1) * full_size + preview_size - 1) / preview_size));
    }
  };

  double abs_sum = 0.0;
  double sq_sum = 0.0;
  double max_abs = 0.0;
  for (size_t y = 0; y < preview.ysize; ++y) {
    size_t y0 = 0;
    size_t y1 = 0;
    range(y, preview.ysize, full.ysize, &y0, &y1);
    for (size_t x = 0; x < preview.xsize; ++x) {
      size_t x0 = 0;
      size_t x1 = 0;
      range(x, preview.xsize, full.xsize, &x0, &x1);
      const size_t count = (x1 - x0) * (y1 - y0);
      for (size_t c = 0; c < channels; ++c) {
        double reference = 0.0;
        for (size_t iy = y0; iy < y1; ++iy) {
          for (size_t ix = x0; ix < x1; ++ix) {
            reference += full.GetPixelValue(iy, ix, c);
          }
        }
        reference /= count;
        const double actual = preview.GetPixelValue(y, x, c);
        const double diff = std::abs(actual - reference);
        abs_sum += diff;
        sq_sum += diff * diff;
        max_abs = std::max(max_abs, diff);
        ++quality.compared_samples;
      }
    }
  }

  if (quality.compared_samples == 0) {
    quality.note = "No pixels were compared";
    return quality;
  }
  quality.available = true;
  quality.mae = abs_sum / quality.compared_samples;
  quality.rmse = std::sqrt(sq_sum / quality.compared_samples);
  quality.max_abs = max_abs;
  return quality;
}

std::string SummaryJson(const PreviewBenchSummary& summary) {
  std::ostringstream os;
  os << "{"
     << "\"ok\":" << (summary.ok ? "true" : "false") << ","
     << "\"mode\":\"" << PreviewBenchModeName(summary.mode) << "\","
     << "\"measurement\":\"" << PreviewBenchMeasurementName(summary.measurement)
     << "\","
     << "\"error\":\"" << JsonEscape(summary.error) << "\","
     << "\"input_bytes\":" << summary.input_bytes << ","
     << "\"output_xsize\":" << summary.output_xsize << ","
     << "\"output_ysize\":" << summary.output_ysize << ","
     << "\"frame_count\":" << summary.frame_count << ","
     << "\"num_channels\":" << summary.num_channels << ","
     << "\"decoded_bytes\":" << summary.decoded_bytes << ","
     << "\"preview_backend\":\"" << PreviewBackendName(summary.preview_backend)
     << "\","
     << "\"wall_time_ms\":{\"mean\":" << summary.wall_time_ms.mean
     << ",\"stddev\":" << summary.wall_time_ms.stddev
     << ",\"min\":" << summary.wall_time_ms.min
     << ",\"max\":" << summary.wall_time_ms.max << "},"
     << "\"cpu_time_ms\":{\"mean\":" << summary.cpu_time_ms.mean
     << ",\"stddev\":" << summary.cpu_time_ms.stddev
     << ",\"min\":" << summary.cpu_time_ms.min
     << ",\"max\":" << summary.cpu_time_ms.max << "},"
     << "\"decoder_peak_bytes\":" << summary.decoder_peak_bytes << ","
     << "\"decoder_total_bytes\":" << summary.decoder_total_bytes << ","
     << "\"decoder_num_allocations\":" << summary.decoder_num_allocations << ","
     << "\"effective_num_threads\":" << summary.effective_num_threads << ","
     << "\"throughput_src_mpx_per_s\":" << summary.throughput_mpx_per_s << ","
     << "\"process_peak_working_set_bytes\":"
     << OptionalNumber(summary.has_process_peak_working_set,
                       summary.process_peak_working_set_bytes, "null")
     << ","
     << "\"process_peak_private_bytes\":"
     << OptionalNumber(summary.has_process_peak_private,
                       summary.process_peak_private_bytes, "null")
     << "}";
  return os.str();
}

enum class DecodePurpose {
  // Oriented 8-bit output.
  kDisplay,
  // Output in codestream orientation, where the decoder's boxes start at the
  // origin, as unclamped 32-bit float, so that the quality metric is limited
  // neither by 8-bit quantization nor by clamping (which the decoder applies
  // after averaging). Uses 4 times the memory of kDisplay.
  kQuality,
};

bool DecodeForBenchmark(const std::string& pathname,
                        const PreviewBenchOptions& options,
                        PreviewBenchMode mode, DecodePurpose purpose,
                        PreviewBenchImage* image, std::string* error) {
  if (image == nullptr) {
    if (error) *error = "image output pointer was null";
    return false;
  }
  std::vector<uint8_t> compressed;
  if (!ReadPreviewBenchFile(pathname, &compressed)) {
    if (error) *error = "Failed to read input file";
    return false;
  }

  auto runner = JxlThreadParallelRunnerMake(
      nullptr, PreviewBenchEffectiveNumThreads(compressed, options, mode));
  if (!runner) {
    if (error) *error = "Failed to create the thread runner";
    return false;
  }

  // Probe the image's intensity_target to detect HDR content.
  // For HDR images (intensity_target > 255 nits), requesting a sRGB output
  // colorspace with relative colorimetric intent without tone mapping will
  // black-crush the image: SDR-level content (e.g., 100 nits in a 1000-nit
  // image) gets mapped to ~2.5 % of the display range (~24/255). Setting
  // display_nits to a typical SDR monitor target triggers the library's
  // built-in Rec. 2390 HDR-to-SDR tone mapping before the sRGB conversion.
  float intensity_target = 255.0f;
  {
    JxlDecoder* probe = JxlDecoderCreate(nullptr);
    if (probe != nullptr) {
      if (JXL_DEC_SUCCESS ==
          JxlDecoderSubscribeEvents(probe, JXL_DEC_BASIC_INFO)) {
        if (JXL_DEC_SUCCESS ==
            JxlDecoderSetInput(probe, compressed.data(), compressed.size())) {
          if (JxlDecoderProcessInput(probe) == JXL_DEC_BASIC_INFO) {
            JxlBasicInfo info = {};
            if (JXL_DEC_SUCCESS == JxlDecoderGetBasicInfo(probe, &info)) {
              intensity_target = info.intensity_target;
            }
          }
        }
      }
      JxlDecoderDestroy(probe);
    }
  }

  const bool for_quality = purpose == DecodePurpose::kQuality;
  jxl::extras::JXLDecompressParams dparams;
  dparams.accepted_formats =
      AcceptedFormats(for_quality ? JXL_TYPE_FLOAT : JXL_TYPE_UINT8);
  dparams.color_space = options.color_space;
  dparams.runner = JxlThreadParallelRunner;
  dparams.runner_opaque = runner.get();
  dparams.unpremultiply_alpha = true;
  dparams.use_image_callback = false;
  dparams.keep_orientation = for_quality;
  // Only the first displayed frame is used (and is all a preview decodes).
  dparams.first_frame_only = true;
  if (mode == PreviewBenchMode::kPreview) {
    dparams.preview_downsampling = options.preview_downsampling;
    dparams.preview_backend = &image->preview_backend;
    dparams.preview_hooks = jxl::GetDecoderPreviewHooks();
  }
  // For HDR images, tone-map to a typical SDR display target so the output
  // is visible without black-crush. 250 nits covers most desktop monitors.
  // The benchmark decode path is intentionally left without tone mapping since
  // it measures raw decode throughput for a specific parameter set.
  static constexpr float kSdrDisplayNits = 250.0f;
  if (intensity_target > 255.0f) {
    dparams.display_nits = kSdrDisplayNits;
  }

  if (!jxl::extras::DecodeImageJXL(compressed.data(), compressed.size(),
                                   dparams, &image->decoded_bytes,
                                   &image->ppf)) {
    if (error) *error = "DecodeImageJXL failed";
    return false;
  }
  return true;
}

}  // namespace

bool PreviewBenchWorkerAvailable(std::string* error) {
#if JXL_PREVIEW_BENCH_HAS_WORKER
  const std::filesystem::path worker = WorkerExecutablePath();
  if (worker.empty()) {
    if (error) *error = "the location of this program could not be determined";
    return false;
  }
  std::error_code ec;
  if (!std::filesystem::is_regular_file(worker, ec)) {
    if (error) *error = worker.string() + " was not found";
    return false;
  }
  return true;
#else
  if (error) {
    *error = "out-of-process workers are not supported on this platform";
  }
  return false;
#endif
}

const char* PreviewBenchModeName(PreviewBenchMode mode) {
  switch (mode) {
    case PreviewBenchMode::kFull:
      return "full";
    case PreviewBenchMode::kPreview:
      return "preview";
  }
  return "unknown";
}

const char* PreviewBenchMeasurementName(PreviewBenchMeasurement measurement) {
  switch (measurement) {
    case PreviewBenchMeasurement::kWorker:
      return "worker";
    case PreviewBenchMeasurement::kInProcess:
      return "in_process";
  }
  return "unknown";
}

const char* PreviewBackendName(jxl::extras::JXLPreviewBackend backend) {
  switch (backend) {
    case jxl::extras::JXLPreviewBackend::kEmbeddedPreview:
      return "embedded_preview";
    case jxl::extras::JXLPreviewBackend::kNativeDcOnly:
      return "native_dc_only";
    case jxl::extras::JXLPreviewBackend::kNativeProgressionFlush:
      return "native_progression_flush";
    case jxl::extras::JXLPreviewBackend::kFallbackDownsample:
      return "fallback_downsample";
    case jxl::extras::JXLPreviewBackend::kNativeReducedInput:
      return "native_reduced_input";
    case jxl::extras::JXLPreviewBackend::kNativeFusedUpsampling:
      return "native_fused_upsampling";
    case jxl::extras::JXLPreviewBackend::kDecoderDownsample:
      return "decoder_downsample";
    case jxl::extras::JXLPreviewBackend::kNone:
    default:
      return "none";
  }
}

const char* FrameEncodingName(JxlFrameEncoding encoding) {
  switch (encoding) {
    case JXL_FRAME_ENCODING_VAR_DCT:
      return "VarDCT";
    case JXL_FRAME_ENCODING_MODULAR:
      return "Modular";
    case JXL_FRAME_ENCODING_UNKNOWN:
    default:
      return "Unknown";
  }
}

bool ExpandJxlInputs(const std::vector<std::string>& roots,
                     std::vector<std::string>* inputs, std::string* error) {
  if (inputs == nullptr) {
    if (error) *error = "inputs output pointer was null";
    return false;
  }

  std::vector<std::string> collected;
  for (const std::string& root : roots) {
    std::error_code ec;
    const std::filesystem::path path(root);
    if (!std::filesystem::exists(path, ec)) {
      if (error) *error = "Input does not exist: " + root;
      return false;
    }
    if (std::filesystem::is_regular_file(path, ec)) {
      if (!IsJxlPath(path)) {
        if (error) *error = "Input is not a .jxl file: " + root;
        return false;
      }
      collected.push_back(std::filesystem::absolute(path, ec).string());
      continue;
    }
    if (!std::filesystem::is_directory(path, ec)) {
      if (error) *error = "Unsupported input type: " + root;
      return false;
    }
    for (std::filesystem::recursive_directory_iterator it(path, ec), end;
         it != end; it.increment(ec)) {
      if (ec) break;
      if (!it->is_regular_file(ec)) continue;
      if (IsJxlPath(it->path())) {
        collected.push_back(std::filesystem::absolute(it->path(), ec).string());
      }
    }
    if (ec) {
      if (error) *error = "Failed to walk directory: " + root;
      return false;
    }
  }

  std::sort(collected.begin(), collected.end());
  collected.erase(std::unique(collected.begin(), collected.end()),
                  collected.end());
  *inputs = std::move(collected);
  if (inputs->empty()) {
    if (error) *error = "No .jxl files found in the provided inputs";
    return false;
  }
  return true;
}

bool DecodeJxlForPreviewBenchmark(const std::string& pathname,
                                  const PreviewBenchOptions& options,
                                  PreviewBenchMode mode,
                                  PreviewBenchImage* image,
                                  std::string* error) {
  return DecodeForBenchmark(pathname, options, mode, DecodePurpose::kDisplay,
                            image, error);
}

bool BenchmarkJxlFile(const std::string& pathname,
                      const PreviewBenchOptions& options, PreviewBenchMode mode,
                      PreviewBenchSummary* summary) {
  if (summary == nullptr) return false;
  *summary = PreviewBenchSummary();
  summary->mode = mode;
  summary->measurement = options.measurement;
  summary->input_path = pathname;

  std::vector<uint8_t> compressed;
  if (!ReadPreviewBenchFile(pathname, &compressed)) {
    summary->error = "Failed to read input file";
    return false;
  }
  summary->input_bytes = compressed.size();
  summary->frame_encoding = DetectFrameEncoding(compressed);
  ProbeImageDimensions(compressed, &summary->source_xsize,
                       &summary->source_ysize);

  const size_t effective_num_threads =
      PreviewBenchEffectiveNumThreads(compressed, options, mode);
  auto runner = JxlThreadParallelRunnerMake(nullptr, effective_num_threads);
  if (!runner) {
    summary->error = "Failed to create the thread runner";
    return false;
  }

  const size_t num_reps = std::max(size_t{1}, options.num_reps);
  for (size_t rep = 0; rep < num_reps; ++rep) {
    PreviewBenchRun run =
        RunDecode(pathname, compressed, options, mode, runner);
    if (!run.ok) {
      summary->error = run.error;
      return false;
    }
    summary->runs.emplace_back(std::move(run));
  }

  const PreviewBenchRun& first = summary->runs.front();
  summary->output_xsize = first.output_xsize;
  summary->output_ysize = first.output_ysize;
  summary->frame_count = first.frame_count;
  summary->num_channels = first.num_channels;
  summary->decoded_bytes = first.decoded_bytes;
  summary->preview_backend = first.preview_backend;
  summary->wall_time_ms =
      ComputeStats(summary->runs,
                   [](const PreviewBenchRun& run) { return run.wall_time_ms; });
  summary->cpu_time_ms =
      ComputeStats(summary->runs,
                   [](const PreviewBenchRun& run) { return run.cpu_time_ms; });
  summary->has_process_peak_working_set = true;
  summary->has_process_peak_private = true;
  for (const PreviewBenchRun& run : summary->runs) {
    summary->decoder_peak_bytes =
        std::max(summary->decoder_peak_bytes, run.decoder_peak_bytes);
    summary->decoder_total_bytes =
        std::max(summary->decoder_total_bytes, run.decoder_total_bytes);
    summary->has_process_peak_working_set &= run.has_process_peak_working_set;
    summary->process_peak_working_set_bytes =
        std::max(summary->process_peak_working_set_bytes,
                 run.process_peak_working_set_bytes);
    summary->has_process_peak_private &= run.has_process_peak_private;
    summary->process_peak_private_bytes = std::max(
        summary->process_peak_private_bytes, run.process_peak_private_bytes);
    summary->decoder_num_allocations =
        std::max(summary->decoder_num_allocations, run.decoder_num_allocations);
  }
  if (!summary->has_process_peak_working_set) {
    summary->process_peak_working_set_bytes = 0;
  }
  if (!summary->has_process_peak_private) {
    summary->process_peak_private_bytes = 0;
  }
  summary->effective_num_threads = effective_num_threads;
  summary->throughput_mpx_per_s =
      summary->wall_time_ms.mean > 0.0 && summary->source_xsize > 0
          ? static_cast<double>(summary->source_xsize) *
                static_cast<double>(summary->source_ysize) /
                (summary->wall_time_ms.mean * 1000.0)
          : 0.0;
  summary->ok = true;
  return true;
}

bool BenchmarkJxlComparison(const std::string& pathname,
                            const PreviewBenchOptions& options,
                            PreviewBenchComparison* comparison) {
  if (comparison == nullptr) return false;
  *comparison = PreviewBenchComparison();
  comparison->input_path = pathname;

  const bool full_ok = BenchmarkJxlFile(
      pathname, options, PreviewBenchMode::kFull, &comparison->full);
  const bool preview_ok = BenchmarkJxlFile(
      pathname, options, PreviewBenchMode::kPreview, &comparison->preview);
  if (!full_ok || !preview_ok) {
    return false;
  }

  comparison->wall_time_reduction_pct =
      PercentReduction(comparison->full.wall_time_ms.mean,
                       comparison->preview.wall_time_ms.mean);
  comparison->cpu_time_reduction_pct = PercentReduction(
      comparison->full.cpu_time_ms.mean, comparison->preview.cpu_time_ms.mean);
  comparison->decoded_bytes_reduction_pct =
      PercentReduction(static_cast<double>(comparison->full.decoded_bytes),
                       static_cast<double>(comparison->preview.decoded_bytes));
  comparison->decoder_peak_reduction_pct = PercentReduction(
      static_cast<double>(comparison->full.decoder_peak_bytes),
      static_cast<double>(comparison->preview.decoder_peak_bytes));
  comparison->decoder_num_allocations_reduction_pct = PercentReduction(
      static_cast<double>(comparison->full.decoder_num_allocations),
      static_cast<double>(comparison->preview.decoder_num_allocations));
  if (HasWorkingSetPeaks(*comparison)) {
    comparison->process_peak_working_set_reduction_pct = PercentReduction(
        static_cast<double>(comparison->full.process_peak_working_set_bytes),
        static_cast<double>(
            comparison->preview.process_peak_working_set_bytes));
  }
  if (HasPrivatePeaks(*comparison)) {
    comparison->process_peak_private_reduction_pct = PercentReduction(
        static_cast<double>(comparison->full.process_peak_private_bytes),
        static_cast<double>(comparison->preview.process_peak_private_bytes));
  }
  comparison->wall_time_speedup =
      Speedup(comparison->full.wall_time_ms.mean,
              comparison->preview.wall_time_ms.mean);
  comparison->cpu_time_speedup = Speedup(comparison->full.cpu_time_ms.mean,
                                         comparison->preview.cpu_time_ms.mean);

  PreviewBenchImage full_image;
  PreviewBenchImage preview_image;
  std::string quality_error;
  if (DecodeForBenchmark(pathname, options, PreviewBenchMode::kFull,
                         DecodePurpose::kQuality, &full_image,
                         &quality_error) &&
      DecodeForBenchmark(pathname, options, PreviewBenchMode::kPreview,
                         DecodePurpose::kQuality, &preview_image,
                         &quality_error)) {
    comparison->quality = ComputeQuality(full_image.ppf, preview_image.ppf,
                                         options.preview_downsampling);
  } else {
    comparison->quality.note = quality_error;
  }
  return true;
}

std::string FormatSummaryText(const PreviewBenchSummary& summary) {
  const int kW = 16;
  std::ostringstream os;

  // Section header
  os << "  "
     << (summary.mode == PreviewBenchMode::kPreview ? "Preview" : "Full");
  os << "  (" << FrameEncodingName(summary.frame_encoding);
  if (summary.source_xsize > 0) {
    os << ", " << summary.source_xsize << "\xC3\x97"
       << summary.source_ysize;  // ×
    if (summary.output_xsize != summary.source_xsize ||
        summary.output_ysize != summary.source_ysize) {
      os << " \xE2\x86\x92 " << summary.output_xsize << "\xC3\x97"  // →
         << summary.output_ysize;
    }
  } else {
    os << ", " << summary.output_xsize << "\xC3\x97" << summary.output_ysize;
  }
  if (summary.mode == PreviewBenchMode::kPreview) {
    os << ", " << PreviewBackendName(summary.preview_backend);
  }
  os << ")\n";

  auto Line = [&](const char* label, const std::string& value) {
    os << "    " << std::left << std::setw(kW) << label << value << "\n";
  };

  Line("Wall time", SummaryRange(summary.wall_time_ms));
  Line("CPU time", SummaryRange(summary.cpu_time_ms));
  {
    std::ostringstream v;
    v << std::fixed << std::setprecision(2) << summary.throughput_mpx_per_s
      << " src-Mpx/s";
    Line("Throughput", v.str());
  }
  Line("Threads", std::to_string(summary.effective_num_threads));
  Line("Decoded", HumanBytes(summary.decoded_bytes));
  Line("Allocs", std::to_string(summary.decoder_num_allocations));
  Line("Decoder peak", HumanBytes(summary.decoder_peak_bytes));
  Line("WS peak", summary.has_process_peak_working_set
                      ? HumanBytes(summary.process_peak_working_set_bytes)
                      : "n/a");
  Line("Private peak", summary.has_process_peak_private
                           ? HumanBytes(summary.process_peak_private_bytes)
                           : "n/a");
  return os.str();
}

std::string FormatComparisonText(const PreviewBenchComparison& comparison) {
  const int kW = 16;
  std::ostringstream os;
  os << "=== " << comparison.input_path << " ===\n";
  os << "  Measurement: "
     << PreviewBenchMeasurementName(comparison.full.measurement)
     << (comparison.full.measurement == PreviewBenchMeasurement::kWorker
             ? " (one worker process per decode; process peaks are the "
               "worker's high-water marks)"
             : " (decodes in this process; process memory not measured)")
     << "\n\n";
  os << FormatSummaryText(comparison.full) << "\n";
  os << FormatSummaryText(comparison.preview) << "\n";

  auto Line = [&](const char* label, const std::string& value) {
    os << "    " << std::left << std::setw(kW) << label << value << "\n";
  };

  os << "  Comparison\n";
  {
    std::ostringstream v;
    v << std::fixed << std::setprecision(2) << comparison.wall_time_speedup
      << "\xC3\x97  (" << FormatPctDelta(comparison.wall_time_reduction_pct)
      << ")";
    Line("Wall speedup", v.str());
  }
  {
    std::ostringstream v;
    v << std::fixed << std::setprecision(2) << comparison.cpu_time_speedup
      << "\xC3\x97  (" << FormatPctDelta(comparison.cpu_time_reduction_pct)
      << ")";
    Line("CPU speedup", v.str());
  }
  Line("Decoded bytes", FormatPctDelta(comparison.decoded_bytes_reduction_pct));
  Line("Decoder peak", FormatPctDelta(comparison.decoder_peak_reduction_pct));
  Line("Allocs",
       FormatPctDelta(comparison.decoder_num_allocations_reduction_pct));
  Line("WS peak",
       HasWorkingSetPeaks(comparison)
           ? FormatPctDelta(comparison.process_peak_working_set_reduction_pct)
           : "n/a");
  Line("Private peak",
       HasPrivatePeaks(comparison)
           ? FormatPctDelta(comparison.process_peak_private_reduction_pct)
           : "n/a");

  if (comparison.quality.available) {
    os << "\n  Quality\n";
    auto QLine = [&](const char* label, double value) {
      std::ostringstream v;
      v << std::fixed << std::setprecision(5) << value;
      Line(label, v.str());
    };
    Line("Reference", comparison.quality.reference == "boxes"
                          ? "box average of the full decode"
                          : "proportional boxes (preview is not the image "
                            "size divided by the factor)");
    QLine("RMSE", comparison.quality.rmse);
    QLine("MAE", comparison.quality.mae);
    QLine("Max|\xCE\x94|", comparison.quality.max_abs);
  } else if (!comparison.quality.note.empty()) {
    os << "\n  Quality\n";
    Line("Note", comparison.quality.note);
  }
  return os.str();
}

bool WriteComparisonsCsv(const std::vector<PreviewBenchComparison>& comparisons,
                         const std::string& pathname, std::string* error) {
  std::ostringstream csv;
  csv << "input_path,measurement,preview_backend,"
         "full_output_xsize,full_output_ysize,"
         "preview_output_xsize,preview_output_ysize,"
         "full_wall_ms_mean,full_wall_ms_stddev,"
         "preview_wall_ms_mean,preview_wall_ms_stddev,"
         "wall_reduction_pct,wall_speedup,"
         "full_cpu_ms_mean,full_cpu_ms_stddev,"
         "preview_cpu_ms_mean,preview_cpu_ms_stddev,"
         "cpu_reduction_pct,cpu_speedup,"
         "full_effective_threads,preview_effective_threads,"
         "full_src_mpx_per_s,preview_src_mpx_per_s,"
         "full_decoded_bytes,preview_decoded_bytes,decoded_bytes_reduction_pct,"
         "full_decoder_peak_bytes,preview_decoder_peak_bytes,"
         "decoder_peak_reduction_pct,"
         "full_decoder_num_allocations,preview_decoder_num_allocations,"
         "full_process_peak_working_set_bytes,"
         "preview_process_peak_working_set_bytes,"
         "process_peak_working_set_reduction_pct,"
         "full_process_peak_private_bytes,"
         "preview_process_peak_private_bytes,"
         "process_peak_private_reduction_pct,"
         "decoder_num_allocations_reduction_pct,"
         "quality_reference,quality_rmse,quality_mae,quality_max_abs\n";
  for (const PreviewBenchComparison& comparison : comparisons) {
    const bool ws = HasWorkingSetPeaks(comparison);
    const bool priv = HasPrivatePeaks(comparison);
    csv << CsvEscape(comparison.input_path) << ','
        << PreviewBenchMeasurementName(comparison.full.measurement) << ','
        << PreviewBackendName(comparison.preview.preview_backend) << ','
        << comparison.full.output_xsize << ',' << comparison.full.output_ysize
        << ',' << comparison.preview.output_xsize << ','
        << comparison.preview.output_ysize << ','
        << comparison.full.wall_time_ms.mean << ','
        << comparison.full.wall_time_ms.stddev << ','
        << comparison.preview.wall_time_ms.mean << ','
        << comparison.preview.wall_time_ms.stddev << ','
        << comparison.wall_time_reduction_pct << ','
        << comparison.wall_time_speedup << ','
        << comparison.full.cpu_time_ms.mean << ','
        << comparison.full.cpu_time_ms.stddev << ','
        << comparison.preview.cpu_time_ms.mean << ','
        << comparison.preview.cpu_time_ms.stddev << ','
        << comparison.cpu_time_reduction_pct << ','
        << comparison.cpu_time_speedup << ','
        << comparison.full.effective_num_threads << ','
        << comparison.preview.effective_num_threads << ','
        << comparison.full.throughput_mpx_per_s << ','
        << comparison.preview.throughput_mpx_per_s << ','
        << comparison.full.decoded_bytes << ','
        << comparison.preview.decoded_bytes << ','
        << comparison.decoded_bytes_reduction_pct << ','
        << comparison.full.decoder_peak_bytes << ','
        << comparison.preview.decoder_peak_bytes << ','
        << comparison.decoder_peak_reduction_pct << ','
        << comparison.full.decoder_num_allocations << ','
        << comparison.preview.decoder_num_allocations << ','
        << OptionalNumber(comparison.full.has_process_peak_working_set,
                          comparison.full.process_peak_working_set_bytes, "n/a")
        << ','
        << OptionalNumber(comparison.preview.has_process_peak_working_set,
                          comparison.preview.process_peak_working_set_bytes,
                          "n/a")
        << ','
        << OptionalNumber(ws, comparison.process_peak_working_set_reduction_pct,
                          "n/a")
        << ','
        << OptionalNumber(comparison.full.has_process_peak_private,
                          comparison.full.process_peak_private_bytes, "n/a")
        << ','
        << OptionalNumber(comparison.preview.has_process_peak_private,
                          comparison.preview.process_peak_private_bytes, "n/a")
        << ','
        << OptionalNumber(priv, comparison.process_peak_private_reduction_pct,
                          "n/a")
        << ',' << comparison.decoder_num_allocations_reduction_pct << ','
        << (comparison.quality.available ? comparison.quality.reference
                                         : std::string("n/a"))
        << ',' << comparison.quality.rmse << ',' << comparison.quality.mae
        << ',' << comparison.quality.max_abs << '\n';
  }
  const std::string text = csv.str();
  if (!WritePreviewBenchFile(pathname, text.data(), text.size())) {
    if (error) *error = "Failed to write CSV output";
    return false;
  }
  return true;
}

bool WriteComparisonsJson(
    const std::vector<PreviewBenchComparison>& comparisons,
    const std::string& pathname, std::string* error) {
  std::ostringstream json;
  json << "{\n  \"comparisons\": [\n";
  for (size_t i = 0; i < comparisons.size(); ++i) {
    const PreviewBenchComparison& comparison = comparisons[i];
    json << "    {\n"
         << "      \"input_path\": \"" << JsonEscape(comparison.input_path)
         << "\",\n"
         << "      \"measurement\": \""
         << PreviewBenchMeasurementName(comparison.full.measurement) << "\",\n"
         << "      \"full\": " << SummaryJson(comparison.full) << ",\n"
         << "      \"preview\": " << SummaryJson(comparison.preview) << ",\n"
         << "      \"quality\": {"
         << "\"available\":"
         << (comparison.quality.available ? "true" : "false") << ","
         << "\"note\":\"" << JsonEscape(comparison.quality.note) << "\","
         << "\"reference\":"
         << (comparison.quality.available
                 ? "\"" + comparison.quality.reference + "\""
                 : std::string("null"))
         << ","
         << "\"compared_samples\":" << comparison.quality.compared_samples
         << ","
         << "\"rmse\":" << comparison.quality.rmse << ","
         << "\"mae\":" << comparison.quality.mae << ","
         << "\"max_abs\":" << comparison.quality.max_abs << "},\n"
         << "      \"reduction\": {"
         << "\"wall_time_pct\":" << comparison.wall_time_reduction_pct << ","
         << "\"cpu_time_pct\":" << comparison.cpu_time_reduction_pct << ","
         << "\"decoded_bytes_pct\":" << comparison.decoded_bytes_reduction_pct
         << ","
         << "\"decoder_peak_pct\":" << comparison.decoder_peak_reduction_pct
         << ","
         << "\"decoder_num_allocations_pct\":"
         << comparison.decoder_num_allocations_reduction_pct << ","
         << "\"process_peak_working_set_pct\":"
         << OptionalNumber(HasWorkingSetPeaks(comparison),
                           comparison.process_peak_working_set_reduction_pct,
                           "null")
         << ","
         << "\"process_peak_private_pct\":"
         << OptionalNumber(HasPrivatePeaks(comparison),
                           comparison.process_peak_private_reduction_pct,
                           "null")
         << ","
         << "\"wall_speedup\":" << comparison.wall_time_speedup << ","
         << "\"cpu_speedup\":" << comparison.cpu_time_speedup << "}\n"
         << "    }";
    if (i + 1 != comparisons.size()) json << ',';
    json << '\n';
  }
  json << "  ]\n}\n";
  const std::string text = json.str();
  if (!WritePreviewBenchFile(pathname, text.data(), text.size())) {
    if (error) *error = "Failed to write JSON output";
    return false;
  }
  return true;
}

}  // namespace tools
}  // namespace jpegxl
