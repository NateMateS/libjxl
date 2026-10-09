# JPEG XL Preview Benchmark

This directory contains the tools for exercising and validating the generalized
preview / scaled-decode path: the decoder-side output downsampling of the
public API (`JxlDecoderSetImageOutDownsampling` in
[`jxl/decode.h`](../../lib/include/jxl/decode.h)), and the in-tree one-shot
wrapper above it, [`lib/extras/preview.h`](../../lib/extras/preview.h), which is
not installed. See the headers for the API reference and
[`examples/decode_preview.cc`](../../examples/decode_preview.cc) for a
thumbnailer built on the public API alone.

The decoder reports its render method through the public
`JxlDecoderGetImageOutDownsamplingMethod`. The tools link the static
`jxl-internal` library, whose decoder preview hooks
(`lib/jxl/dec_preview_internal.h`) also restrict that method. A program linking
the shared library has no hooks: it can allow or refuse the decoder's methods
only as a whole, `decoder_downsample`. `djxl --preview_downsampling=N -v`
prints the backend it used (`Preview backend: NAME`, with the names under
[Preview Backends](#preview-backends)).

| Tool | Description | Built with |
|------|-------------|------------|
| `preview_bench` | CLI benchmarker: full vs. preview decode, timing, throughput, memory, and quality | `JPEGXL_ENABLE_BENCHMARK` |
| `preview_bench_worker` | Runs one decode for `preview_bench` and `preview_bench_gui` | `JPEGXL_ENABLE_BENCHMARK` |
| `preview_bench_gui` | Qt GUI for the same benchmark (same core and worker as `preview_bench`) | `JPEGXL_ENABLE_BENCHMARK`, `JPEGXL_ENABLE_VIEWERS`, Qt6 |
| `preview_demo_gui` | Interactive API explorer: decode options, placeholder progression, batch gallery | `JPEGXL_ENABLE_VIEWERS`, Qt6 |
| `preview_api_test` | Headless exerciser for `lib/extras/preview.h` sanity-checking | `BUILD_TESTING` |

All of them also need `JPEGXL_ENABLE_TOOLS` (the default). `JPEGXL_ENABLE_BENCHMARK`
is on by default and `JPEGXL_ENABLE_VIEWERS` is off by default. Only
`preview_bench` and `preview_bench_worker` are installed.

`preview_bench` and `preview_bench_gui` are meant to compare:

- full decode
- preview decode
- preview backend choice
- wall time (mean, stddev, min, max)
- CPU time (mean, stddev, min, max)
- decoded codestream bytes
- tracked decoder allocations
- decoder allocation count
- effective thread count used
- decode throughput (source Mpx/s)
- peak process working set / private bytes
- preview quality against a downsampled full decode reference

## Prerequisites

### Qt6 (GUI only)

`preview_bench_gui` and `preview_demo_gui` require Qt6 Widgets. The CLI
(`preview_bench`), worker (`preview_bench_worker`), and `preview_api_test`
have no Qt dependency and build everywhere.

| Platform | Install |
|----------|---------|
| Debian/Ubuntu | `sudo apt install qt6-base-dev` |
| Fedora/RHEL | `sudo dnf install qt6-qtbase-devel` |
| Arch | `sudo pacman -S qt6-base` |
| macOS (Homebrew) | `brew install qt` |

With `JPEGXL_ENABLE_VIEWERS=ON`, CMake detects Qt6 automatically via
`find_package(Qt6)`. If Qt6 is absent, CMake prints a status message and skips
both GUI targets; `preview_bench`, `preview_bench_worker`, and
`preview_api_test` still build normally.

The simplest way to get a known-good environment is the repository's
[dev container](../../.devcontainer) (VS Code: "Reopen in Container"),
which provisions Qt6 and every other build dependency.

---

## Build

The `preview_benchmark` subdirectory is not a standalone CMake project --
it must be configured as part of the main libjxl build. Run from the
repository root (the directory containing the top-level `CMakeLists.txt`).
`BUILD_TESTING` (on by default) needs the `testdata` submodule: run
`git submodule update --init --recursive` first, or pass
`-DBUILD_TESTING=OFF`, which drops `preview_api_test`.

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DJPEGXL_ENABLE_VIEWERS=ON
cmake --build build --target \
  preview_bench preview_bench_worker preview_bench_gui \
  preview_demo_gui preview_api_test
```

If CMake cannot find Qt6 automatically (e.g. a Homebrew install on macOS
or a non-system path on Linux), point it explicitly:

```bash
# macOS Homebrew:
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DJPEGXL_ENABLE_VIEWERS=ON \
  -DQt6_DIR=$(brew --prefix qt6)/lib/cmake/Qt6
# Custom Linux install: the same, with
#   -DQt6_DIR=/opt/Qt/6.x.y/gcc_64/lib/cmake/Qt6
```

Windows builds are not documented here; consult the upstream
[`BUILDING.md`](../../BUILDING.md) for the project-wide CMake + MSVC
flow. The targets above are platform-portable; only the build-system
invocation differs. On Windows, with CMake 3.21 or later, the DLLs that each
executable needs are copied next to it, and the GUIs are deployed with
`windeployqt`, so that they run from the build tree.

## Run

The examples use `test_images/`, a local directory of `.jxl` files of your
choice; it is not part of the repository (which only has a few small files in
`testdata/jxl/`).

CLI:

```bash
./build/tools/preview_benchmark/preview_bench \
  --preview_downsampling=4 \
  --iterations=10 \
  --json=results.json \
  test_images/
```

Options:

- `INPUT...`: `.jxl` files, or directories searched recursively for `.jxl`
  files. A missing path or any other file ends the run before decoding.
- `--preview_downsampling N`: preview factor (1, 2, 4 or 8; 1 is the full
  decode). Default: 4.
- `--iterations N`: repetitions per mode. Default: 5. With 0, nothing is timed
  and only `--save-previews` runs.
- `--threads N`: decoder threads. Default: 0, the library default
  (`JxlThreadParallelRunnerDefaultNumWorkerThreads`), which preview decodes
  lower for images with few groups (see `effective_num_threads` below).
- `--in_process`: decode in the `preview_bench` process instead of in
  `preview_bench_worker` processes (see below).
- `--csv PATH`, `--json PATH`: write the results.
- `--save-previews DIR`: decode each preview for display and save its first
  frame to `DIR/<name>_ds<N>.png`, or, if `preview_bench` was built without
  PNG support, to `.pgm` (gray), `.ppm` (RGB) or `.pam` (with alpha).

`preview_bench` exits with a non-zero status if any input fails, after
printing its error; inputs that fail are left out of the CSV and JSON output.

Benchmark GUI (accepts file or directory arguments):

```bash
./build/tools/preview_benchmark/preview_bench_gui
./build/tools/preview_benchmark/preview_bench_gui test_images
```

Demo GUI (accepts `.jxl` file arguments; its Gallery tab opens a folder, from
its button or by dropping the folder on the window):

```bash
./build/tools/preview_benchmark/preview_demo_gui
./build/tools/preview_benchmark/preview_demo_gui test_images/*.jxl
```

`preview_bench` and `preview_bench_gui` locate `preview_bench_worker` next to
their own binary, so keep them in the same directory after install/copy.

## Preview Backends

Preview runs report the backend that produced the output:

- `none`: no preview backend was used.
- `embedded_preview`: the codestream's embedded preview image was used.
- `native_dc_only`: the decoder rendered a VarDCT frame from its DC image at
  1/8 resolution, without decoding the AC data.
- `native_progression_flush`: the decoder flushed an intermediate image at a
  progressive decoding step and stopped before full-image completion.
- `native_reduced_input`: the decoder ran the native reduced-resolution path
  for a non-frame-upsampled image.
- `native_fused_upsampling`: the decoder combined encoded frame upsampling with
  the requested preview downsampling instead of upsampling to full display size
  and then downsampling again.
- `fallback_downsample`: the decoder produced full output and downsampled it for
  preview, used when native preview paths are unsafe or disallowed.
- `decoder_downsample`: never reported; as an allowed backend, any of the four
  decoder render methods above (`native_dc_only`, `native_reduced_input`,
  `native_fused_upsampling`, `fallback_downsample`).

## Tests and visual regression

`preview_api_test` is a standalone exerciser for `lib/extras/preview.h`. It is
built with `BUILD_TESTING=ON`, and registered with CTest on
`testdata/jxl/splines.jxl` and `testdata/jxl/pq_gradient.jxl`:

```bash
ctest --test-dir build -R preview_api_test
```

It can also be run on one SDR JXL and (optionally) one HDR JXL:

```bash
./build/tools/preview_benchmark/preview_api_test \
  test_images/flower_modular_responsive.jxl \
  test_images/sunrise-PQ.jxl
```

`dump_previews.sh` regenerates a visual regression set: it drives
`preview_bench --save-previews --iterations=0` over every `.jxl` at the top
level of `test_images/` at downsampling factors 2, 4, 8, writes the previews to
`test_images/previews/{ds2,ds4,ds8}/` and a `summary.csv` (backend chosen per
input and factor) to `test_images/previews/`. It exits with a non-zero status
if any preview could not be saved. Run it from the repository root (its
default paths are relative), with bash 4 or later:

```bash
./tools/preview_benchmark/dump_previews.sh
# override search path for test images or the tool binary:
./tools/preview_benchmark/dump_previews.sh --input-dir path/to/images \
    --tool ./build/tools/preview_benchmark/preview_bench
# write the previews and summary.csv elsewhere:
./tools/preview_benchmark/dump_previews.sh --output-dir /tmp/previews
# include factor 1 (full-resolution):
./tools/preview_benchmark/dump_previews.sh --factors 1,2,4,8
```

## Dispatch model and memory metrics

How the decodes are measured is decided once for the whole run, and every
result carries it: the `Measurement:` line of the text output, the
`measurement` column of the CSV output, and the `measurement` field of the JSON
output.

- `worker` (the default): `preview_bench` runs each decode iteration in a
  `preview_bench_worker` process (on Linux, macOS and Windows). If the worker
  cannot be run (it is missing, or the platform cannot spawn it),
  `preview_bench` fails with a message rather than measuring something else.
  If a worker fails, its exit status or signal and its error are the error of
  the run; the decode is not retried in-process.
- `in_process` (`--in_process`): the decodes run in the `preview_bench`
  process. Its memory is not that of one decode, so the process memory peaks
  are not reported: `n/a` in the text and CSV output, `null` in the JSON
  output. `preview_bench_gui` measures in-process when it cannot run
  `preview_bench_worker`, and shows which mode it uses next to the controls.

The CSV output has the mean and stddev of the timings only, and no
`decoder_total_bytes` or `frame_count`; the text output adds min and max, and
the JSON output has every field below.

Metric portability:

- `decoder_peak_bytes` / `decoder_total_bytes` come from
  `TrackingMemoryManager` and are reliable on all platforms. They reflect
  decoder-internal allocations only.
- `decoder_num_allocations` is the total number of `alloc` calls made through
  `TrackingMemoryManager` during the decode. It is reset between reps.
  High allocation counts indicate allocator pressure; lower is better on
  memory-constrained targets.
- `wall_time_ms` / `cpu_time_ms` are reported with mean, sample standard
  deviation, min, and max across reps. Stddev distinguishes stable from noisy
  measurements; it is 0 when only one rep is run. Process CPU time is coarse,
  so small files show noisy CPU times: use more iterations and larger images
  for CPU comparisons.
- `effective_num_threads` is the actual thread count passed to
  `JxlThreadParallelRunnerMake` for that mode and image. With `--threads=0`,
  preview mode scales it by the number of 256x256 groups of the *source*
  image, which the decoder visits whatever the preview size, so it is lower
  than the system default only for images with few groups.
- `throughput_src_mpx_per_s` is
  `source_xsize × source_ysize / (1000 × wall_time_ms.mean)`, in source
  megapixels per second, for both modes. It normalises timing across images of different
  sizes and is the most useful single number for cross-image comparison.
- `wall_time_ms` / `cpu_time_ms` are measured strictly around the
  `DecodeImageJXL` call. Process-spawn time is **not** included: the child
  samples wall (`jxl::Now()`) and CPU (`GetProcessTimes` /
  `getrusage`, both cumulative) immediately before and after the decode
  call, and the parent forwards those values verbatim.
- `process_peak_working_set_bytes` / `process_peak_private_bytes` are the
  high-water marks that the OS keeps for the worker process, which the worker
  reads when its decode is done. They cover the **entire worker lifetime**,
  not just the decode window, so they include process startup, library
  mapping, the input file, and thread-pool creation in addition to decode
  allocations. Use them as a relative comparison between full and preview
  decode of the same input rather than as an absolute decode-only memory
  figure.
  - Windows: `PeakWorkingSetSize` and `PeakPagefileUsage` (peak private
    commit) from `GetProcessMemoryInfo`.
  - Linux: `VmHWM` from `/proc/self/status` (`ru_maxrss` is not used: Linux
    carries it over the `execve` of a spawned process, so it would include
    the peak of `preview_bench`). Private bytes: `n/a`.
  - macOS: `ru_maxrss` from `getrusage`. Private bytes: `n/a`.
  - Other platforms: no worker; use `--in_process`.

Quality: the preview is compared with the full decode, both decoded again
(outside the timed runs) in codestream orientation (the orientation is not
applied), since the decoder averages boxes aligned to the codestream origin,
and as unclamped 32-bit float, so that neither 8-bit quantization nor clamping
(which the decoder applies after averaging) limits the comparison. These
decodes need 4 times the memory of an 8-bit decode. Each preview pixel of a
`DivCeil(xsize, f) × DivCeil(ysize, f)` preview is compared with the average of
the box `[x*f, min(x*f+f, xsize)) × [y*f, min(y*f+f, ysize))` of the full
decode (`reference: boxes`). A preview of any other size (an embedded preview
frame of another size, if the decoder returned one) is compared with boxes
scaled to its size (`reference: proportional`), which is only an approximation
of how that preview was made.

The comparison reports, over the channels both images have, in nominal 0-1
float units: RMSE, MAE and the maximum absolute difference (`quality_rmse`,
`quality_mae`, `quality_max_abs` and `quality_reference` in the CSV output;
`quality.{rmse, mae, max_abs, reference, compared_samples}` in the JSON
output). For HDR inputs (intensity target above 255 nits), these decodes and
the `--save-previews` images are tone-mapped to 250 nits; the timed decodes
are not.

## Diagnostics

Set `JXL_PREVIEW_DEBUG=1` to log each preview frame header and the backend used
to stderr. Building libjxl with `-DJXL_DEBUG_PREVIEW=1` also logs why the
decoder chose its render method.
