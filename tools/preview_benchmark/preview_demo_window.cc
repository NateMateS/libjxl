// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "tools/preview_benchmark/preview_demo_window.h"

#include <jxl/color_encoding.h>
#include <jxl/resizable_parallel_runner.h>
#include <jxl/resizable_parallel_runner_cxx.h>
#include <jxl/types.h>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QKeySequence>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QStringList>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTextCursor>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace jpegxl {
namespace tools {

// --------------------------------------------------------------------------
// Counting memory manager (used when the "track allocations" checkbox is on).
// --------------------------------------------------------------------------
//
// libjxl's JxlMemoryManager hands the caller's `opaque` to every alloc/free.
// We use it to thread per-decode statistics through without a global.
//
// Sizes are stashed in a `void* -> size_t` map keyed by the address we
// returned to the library, rather than in a header just before the pointer.
// This means an "untracked free" (a foreign pointer the library passes us by
// mistake) is detected by a clean map miss instead of an unsafe read of
// memory that may not belong to us. We abort hard on that case so the stack
// trace points at the offending free site, in all build modes.
namespace {

struct CountingMM {
  std::mutex mu;
  std::unordered_map<void*, size_t> live;  // addr -> size
  size_t alloc_count = 0;
  size_t free_count = 0;
  size_t total_bytes = 0;
  size_t live_bytes = 0;
  size_t peak_bytes = 0;
};

void* CountingAlloc(void* opaque, size_t size) {
  auto* mm = static_cast<CountingMM*>(opaque);
  void* p = std::malloc(size);
  if (p == nullptr) return nullptr;
  std::lock_guard<std::mutex> lk(mm->mu);
  mm->live.emplace(p, size);
  ++mm->alloc_count;
  mm->total_bytes += size;
  mm->live_bytes += size;
  if (mm->live_bytes > mm->peak_bytes) mm->peak_bytes = mm->live_bytes;
  return p;
}

void CountingFree(void* opaque, void* ptr) {
  if (ptr == nullptr) return;
  auto* mm = static_cast<CountingMM*>(opaque);
  size_t size = 0;
  {
    std::lock_guard<std::mutex> lk(mm->mu);
    auto it = mm->live.find(ptr);
    if (it == mm->live.end()) {
      std::fprintf(stderr,
                   "CountingFree: untracked free of %p (library passed a "
                   "pointer not allocated through this memory_manager)\n",
                   ptr);
      std::abort();
    }
    size = it->second;
    mm->live.erase(it);
    ++mm->free_count;
    mm->live_bytes -= size;
  }
  std::free(ptr);
}

// Convert a decoded image (any data_type, 1/2/3/4 channels) to a
// 32-bit RGBA QImage suitable for QPixmap display.
QImage ToDisplayImage(const std::vector<uint8_t>& pixels, uint32_t w,
                      uint32_t h, size_t stride, JxlPixelFormat fmt) {
  QImage img(static_cast<int>(w), static_cast<int>(h), QImage::Format_RGBA8888);
  if (w == 0 || h == 0) return img;

  size_t bytes_per_sample = 0;
  switch (fmt.data_type) {
    case JXL_TYPE_UINT8:
      bytes_per_sample = 1;
      break;
    case JXL_TYPE_UINT16:
      bytes_per_sample = 2;
      break;
    case JXL_TYPE_FLOAT16:
      bytes_per_sample = 2;
      break;
    case JXL_TYPE_FLOAT:
      bytes_per_sample = 4;
      break;
  }
  const uint32_t nc = fmt.num_channels;

  auto sample_to_byte = [&](const uint8_t* sample) -> uint8_t {
    switch (fmt.data_type) {
      case JXL_TYPE_UINT8:
        return *sample;
      case JXL_TYPE_UINT16: {
        uint16_t v;
        std::memcpy(&v, sample, 2);
        return static_cast<uint8_t>(v >> 8);
      }
      case JXL_TYPE_FLOAT: {
        float v;
        std::memcpy(&v, sample, 4);
        v = std::clamp(v, 0.0f, 1.0f);
        return static_cast<uint8_t>(std::lround(v * 255.0f));
      }
      case JXL_TYPE_FLOAT16: {
        // Decode IEEE 754 binary16 by hand to keep this example free of
        // any half-float dependency. Saturating in [0, 1] is fine for
        // display.
        uint16_t bits;
        std::memcpy(&bits, sample, 2);
        const uint32_t sign = (bits >> 15) & 0x1;
        const uint32_t exp = (bits >> 10) & 0x1F;
        const uint32_t mant = bits & 0x3FF;
        float v;
        if (exp == 0) {
          v = std::ldexp(static_cast<float>(mant), -24);
        } else if (exp == 0x1F) {
          v = mant ? std::nanf("") : std::numeric_limits<float>::infinity();
        } else {
          v = std::ldexp(static_cast<float>(mant + 0x400),
                         static_cast<int>(exp) - 25);
        }
        if (sign) v = -v;
        v = std::clamp(v, 0.0f, 1.0f);
        return static_cast<uint8_t>(std::lround(v * 255.0f));
      }
    }
    return 0;
  };

  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* row = pixels.data() + y * stride;
    uint8_t* out = img.scanLine(static_cast<int>(y));
    for (uint32_t x = 0; x < w; ++x) {
      const uint8_t* px = row + x * nc * bytes_per_sample;
      uint8_t r = 0, g = 0, b = 0, a = 255;
      switch (nc) {
        case 1:
          r = g = b = sample_to_byte(px);
          break;
        case 2:
          r = g = b = sample_to_byte(px);
          a = sample_to_byte(px + bytes_per_sample);
          break;
        case 3:
          r = sample_to_byte(px);
          g = sample_to_byte(px + bytes_per_sample);
          b = sample_to_byte(px + 2 * bytes_per_sample);
          break;
        case 4:
          r = sample_to_byte(px);
          g = sample_to_byte(px + bytes_per_sample);
          b = sample_to_byte(px + 2 * bytes_per_sample);
          a = sample_to_byte(px + 3 * bytes_per_sample);
          break;
      }
      out[4 * x + 0] = r;
      out[4 * x + 1] = g;
      out[4 * x + 2] = b;
      out[4 * x + 3] = a;
    }
  }
  return img;
}

JxlColorEncoding MakeColorEncoding(int preset_index) {
  // Built by hand to keep this tool free of any encoder dependency.
  JxlColorEncoding ce{};
  ce.color_space = JXL_COLOR_SPACE_RGB;
  ce.white_point = JXL_WHITE_POINT_D65;
  ce.rendering_intent = JXL_RENDERING_INTENT_RELATIVE;
  switch (preset_index) {
    case 0:  // sRGB
      ce.primaries = JXL_PRIMARIES_SRGB;
      ce.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
      break;
    case 1:  // Linear sRGB
      ce.primaries = JXL_PRIMARIES_SRGB;
      ce.transfer_function = JXL_TRANSFER_FUNCTION_LINEAR;
      break;
    case 2:  // Display P3
      ce.primaries = JXL_PRIMARIES_P3;
      ce.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
      break;
    case 3:  // Rec.2020 (BT.2020 primaries, sRGB transfer for an SDR target)
      ce.primaries = JXL_PRIMARIES_2100;
      ce.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
      break;
  }
  return ce;
}

}  // namespace

// --------------------------------------------------------------------------
// DecodeWorker
// --------------------------------------------------------------------------

DecodeWorker::DecodeWorker(QObject* parent) : QObject(parent) {}

void DecodeWorker::Decode(const Request& req) {
  Result result;
  result.used_caller_buffer = req.use_caller_buffer;
  ResetCancel();
  QElapsedTimer timer;
  timer.start();

  // ---- Build options ----
  JxlPreviewOptions opts;
  JxlPreviewOptionsInit(&opts);
  opts.preview_downsampling = req.preview_downsampling;
  opts.target_xsize = req.target_xsize;
  opts.target_ysize = req.target_ysize;

  // Pixel format. requested_num_channels=0 keeps the library default (RGBA8).
  opts.format.num_channels = req.requested_num_channels;
  opts.format.data_type = req.data_type;
  opts.format.endianness = req.endianness;
  opts.format.align = req.format_align;

  // Color encoding (constructed from the UI preset by hand to keep this
  // tool free of any encoder dependency).
  JxlColorEncoding ce = MakeColorEncoding(req.color_preset);
  if (req.use_color_encoding) {
    opts.color_encoding = &ce;
  }
  switch (req.nits_mode) {
    case 0:
      opts.display_nits = 0.0f;
      break;
    case 1:
      opts.display_nits = JXL_PREVIEW_NO_TONE_MAPPING;
      break;
    default:
      opts.display_nits = req.display_nits;
      break;
  }
  opts.allowed_backends = req.allowed_backends;
  opts.preview_hooks = jxl::GetDecoderPreviewHooks();
  opts.cancel = cancel_flag();

  // Optional resizable parallel runner.
  JxlResizableParallelRunnerPtr runner;
  if (req.num_threads > 0) {
    runner = JxlResizableParallelRunnerMake(nullptr);
    JxlResizableParallelRunnerSetThreads(runner.get(), req.num_threads);
    opts.runner = JxlResizableParallelRunner;
    opts.runner_opaque = runner.get();
  }

  // Optional counting memory manager. Threads its stats through `opaque`.
  JxlMemoryManager mm{};
  CountingMM stats;
  if (req.use_memory_manager) {
    mm.opaque = &stats;
    mm.alloc = CountingAlloc;
    mm.free = CountingFree;
    opts.memory_manager = &mm;
  }

  // Optional caller-supplied destination buffer. We probe the source for
  // its full dimensions so we can size the buffer at the worst case
  // (full resolution, 4 channels, 4 bytes per sample) regardless of the
  // preview options the user picked. The same memory manager is passed to
  // the probe so its allocations are also counted.
  std::vector<uint8_t> dst_buffer;
  if (req.use_caller_buffer) {
    JxlPreviewInfoQuery query{};
    if (req.use_memory_manager) query.memory_manager = &mm;
    JxlPreviewInfo probe{};
    JxlPreviewStatus probe_st =
        JxlGetPreviewInfo(reinterpret_cast<const uint8_t*>(req.jxl.constData()),
                          static_cast<size_t>(req.jxl.size()), &query, &probe);
    if (probe_st == JXL_PREVIEW_SUCCESS) {
      size_t n = static_cast<size_t>(probe.xsize) * probe.ysize * 4 * 4;
      // Demo knobs: deliberately under- or over-size the buffer.
      if (req.dst_undersize) {
        n = n > 16 ? 16 : 0;  // Definitely too small for any real preview.
      } else if (req.dst_overprovision > 0) {
        n += static_cast<size_t>(req.dst_overprovision);
      }
      dst_buffer.resize(n);
      opts.dst = dst_buffer.data();
      opts.dst_size = dst_buffer.size();
      opts.dst_stride = req.dst_stride_override;
      result.dst_capacity = dst_buffer.size();
    }
  }

  uint8_t* out_ptr = nullptr;
  size_t out_size = 0;
  uint32_t w = 0;
  uint32_t h = 0;
  size_t stride = 0;
  JxlPixelFormat actual{};
  JxlPreviewBackend backend = JXL_PREVIEW_BACKEND_NONE;
  uint32_t out_factor = 0;
  JxlColorEncoding out_color_encoding{};
  opts.out_pixels = &out_ptr;
  opts.out_pixels_size = &out_size;
  opts.out_xsize = &w;
  opts.out_ysize = &h;
  opts.out_stride = &stride;
  opts.out_format = &actual;
  opts.out_backend_used = &backend;
  opts.out_downsampling = &out_factor;
  opts.out_color_encoding = &out_color_encoding;

  // ---- Run decode ----
  result.status =
      JxlGeneratePreview(reinterpret_cast<const uint8_t*>(req.jxl.constData()),
                         static_cast<size_t>(req.jxl.size()), &opts);
  result.elapsed_ms = timer.elapsed();

  // On BUFFER_TOO_SMALL the outputs describe the preview, with the stride
  // and size the buffer needs.
  if (result.status == JXL_PREVIEW_SUCCESS ||
      result.status == JXL_PREVIEW_BUFFER_TOO_SMALL) {
    result.xsize = w;
    result.ysize = h;
    result.stride = stride;
    result.format = actual;
    result.backend = backend;
    result.decode_factor = out_factor;
    result.raw_pixel_bytes = out_size;
    result.has_color_encoding = true;
    result.color_encoding = out_color_encoding;
  }
  if (result.status == JXL_PREVIEW_BUFFER_TOO_SMALL) {
    result.error_text =
        QStringLiteral("Buffer too small: needs %1 bytes at a stride of %2")
            .arg(static_cast<qulonglong>(out_size))
            .arg(static_cast<qulonglong>(stride));
  } else if (result.status == JXL_PREVIEW_SUCCESS && out_ptr != nullptr) {
    // out_size, not stride * h: a caller buffer ends at the last row's end.
    result.raw_bytes.assign(out_ptr, out_ptr + out_size);
    // Build the display QImage on the worker thread to keep the GUI thread
    // responsive on multi-megapixel previews.
    result.display_image =
        ToDisplayImage(result.raw_bytes, w, h, stride, actual);
  } else if (result.status != JXL_PREVIEW_SUCCESS) {
    result.error_text = QStringLiteral("Decoder returned status %1")
                            .arg(static_cast<int>(result.status));
  }

  // Free the API-owned buffer (only when we did *not* supply dst).
  if (!req.use_caller_buffer && out_ptr != nullptr) {
    if (req.use_memory_manager) {
      CountingFree(&stats, out_ptr);
    } else {
      std::free(out_ptr);
    }
  }

  if (req.use_memory_manager) {
    std::lock_guard<std::mutex> lk(stats.mu);
    result.stats_valid = true;
    result.mm_alloc_count = stats.alloc_count;
    result.mm_free_count = stats.free_count;
    result.mm_total_bytes = stats.total_bytes;
    result.mm_peak_bytes = stats.peak_bytes;
  }

  emit Finished(result);
}

// --------------------------------------------------------------------------
// "placeholder" pattern: progressive 8 -> 4 -> 2 -> 1 with a single
// caller-allocated dst buffer reused across decodes and the fallback
// downsample backend explicitly forbidden so each step proves a native
// one (and is reported via the JXL_PREVIEW_NO_BACKEND_AVAILABLE status
// when none of the native backends can satisfy that factor).
// --------------------------------------------------------------------------

void DecodeWorker::DecodeProgressive(const Request& req) {
  ResetCancel();

  // Probe once for the worst-case size so the dst buffer is sized for
  // factor=1 (full resolution, RGBA, 32-bit float). This mirrors what an
  // image viewer would do when streaming successively sharper previews
  // into the same backing store.
  JxlPreviewInfoQuery probe_query{};
  JxlPreviewInfo probe{};
  JxlPreviewStatus probe_st = JxlGetPreviewInfo(
      reinterpret_cast<const uint8_t*>(req.jxl.constData()),
      static_cast<size_t>(req.jxl.size()), &probe_query, &probe);
  if (probe_st != JXL_PREVIEW_SUCCESS) {
    Result fail;
    fail.status = probe_st;
    fail.error_text = QStringLiteral("Probe failed");
    emit ProgressiveStep(0, 4, 0, fail);
    emit ProgressiveDone();
    return;
  }
  const size_t worst_case_bytes =
      static_cast<size_t>(probe.xsize) * probe.ysize * 4 * 4;
  std::vector<uint8_t> dst(worst_case_bytes);

  // One runner is fine for every step (decoded one at a time).
  JxlResizableParallelRunnerPtr runner;
  bool have_runner = false;
  if (req.num_threads > 0) {
    runner = JxlResizableParallelRunnerMake(nullptr);
    JxlResizableParallelRunnerSetThreads(runner.get(), req.num_threads);
    have_runner = true;
  }

  const uint32_t kFactors[] = {8, 4, 2, 1};
  const int kTotal = static_cast<int>(sizeof(kFactors) / sizeof(kFactors[0]));
  for (int i = 0; i < kTotal; ++i) {
    if (cancel_.load(std::memory_order_relaxed)) break;
    const uint32_t factor = kFactors[i];
    QElapsedTimer timer;
    timer.start();

    JxlPreviewOptions opts;
    JxlPreviewOptionsInit(&opts);
    opts.preview_downsampling = factor;
    opts.format.num_channels = req.requested_num_channels;
    opts.format.data_type = req.data_type;
    opts.format.endianness = req.endianness;
    opts.format.align = req.format_align;
    JxlColorEncoding ce = MakeColorEncoding(req.color_preset);
    if (req.use_color_encoding) opts.color_encoding = &ce;
    opts.display_nits = 0.0f;
    // Fallback downsample explicitly forbidden so an "8" that has no
    // native backend reports JXL_PREVIEW_NO_BACKEND_AVAILABLE rather than
    // silently producing a non-native preview. The last step, factor 1, is
    // the full decode.
    opts.allowed_backends = (JXL_PREVIEW_BACKEND_BIT_FULL_DECODE |
                             JXL_PREVIEW_BACKEND_BIT_EMBEDDED_PREVIEW |
                             JXL_PREVIEW_BACKEND_BIT_NATIVE_DC_ONLY |
                             JXL_PREVIEW_BACKEND_BIT_NATIVE_PROGRESSION_FLUSH |
                             JXL_PREVIEW_BACKEND_BIT_NATIVE_REDUCED_INPUT |
                             JXL_PREVIEW_BACKEND_BIT_NATIVE_FUSED_UPSAMPLING);
    opts.preview_hooks = jxl::GetDecoderPreviewHooks();
    opts.cancel = cancel_flag();
    if (have_runner) {
      opts.runner = JxlResizableParallelRunner;
      opts.runner_opaque = runner.get();
    }
    // Reuse the SAME caller buffer across every decode in the sequence.
    opts.dst = dst.data();
    opts.dst_size = dst.size();
    opts.dst_stride = 0;

    uint8_t* out_ptr = nullptr;
    size_t out_size = 0;
    uint32_t w = 0, h = 0;
    size_t stride = 0;
    JxlPixelFormat actual{};
    JxlPreviewBackend backend = JXL_PREVIEW_BACKEND_NONE;
    opts.out_pixels = &out_ptr;
    opts.out_pixels_size = &out_size;
    opts.out_xsize = &w;
    opts.out_ysize = &h;
    opts.out_stride = &stride;
    opts.out_format = &actual;
    opts.out_backend_used = &backend;

    Result step;
    step.used_caller_buffer = true;
    step.dst_capacity = dst.size();
    step.status = JxlGeneratePreview(
        reinterpret_cast<const uint8_t*>(req.jxl.constData()),
        static_cast<size_t>(req.jxl.size()), &opts);
    step.elapsed_ms = timer.elapsed();
    if (step.status == JXL_PREVIEW_SUCCESS && out_ptr != nullptr) {
      step.xsize = w;
      step.ysize = h;
      step.stride = stride;
      step.format = actual;
      step.backend = backend;
      step.raw_pixel_bytes = out_size;
      step.raw_bytes.assign(out_ptr, out_ptr + out_size);
      step.display_image = ToDisplayImage(step.raw_bytes, w, h, stride, actual);
    } else if (step.status != JXL_PREVIEW_SUCCESS) {
      step.error_text = QStringLiteral("Status %1 at factor %2")
                            .arg(static_cast<int>(step.status))
                            .arg(factor);
    }
    emit ProgressiveStep(i, kTotal, static_cast<int>(factor), step);
  }
  emit ProgressiveDone();
}

// --------------------------------------------------------------------------
// "gallery" pattern: a batch loop that decodes many files with one shared
// runner and one shared atomic cancel flag, emitting a thumbnail signal
// per file as it completes.
// --------------------------------------------------------------------------

void DecodeWorker::DecodeGallery(const QStringList& paths,
                                 const Request& tmpl) {
  ResetCancel();

  JxlResizableParallelRunnerPtr runner =
      JxlResizableParallelRunnerMake(nullptr);
  // Sized to a typical preview; the API tolerates more or fewer threads.
  const uint32_t kThreads = static_cast<uint32_t>(
      std::max(size_t{1}, static_cast<size_t>(tmpl.num_threads)));
  JxlResizableParallelRunnerSetThreads(runner.get(), kThreads);

  int succeeded = 0;
  int failed = 0;
  const int total = paths.size();
  for (int i = 0; i < total; ++i) {
    if (cancel_.load(std::memory_order_relaxed)) break;
    const QString& path = paths[i];
    QFile f(path);
    Result step;
    step.used_caller_buffer = false;
    if (!f.open(QIODevice::ReadOnly)) {
      step.status = JXL_PREVIEW_INVALID_ARGUMENT;
      step.error_text = QStringLiteral("Could not open file");
      ++failed;
      emit GalleryThumb(i, total, path, step);
      continue;
    }
    const QByteArray bytes = f.readAll();

    QElapsedTimer timer;
    timer.start();
    JxlPreviewOptions opts;
    JxlPreviewOptionsInit(&opts);
    opts.target_xsize = tmpl.target_xsize ? tmpl.target_xsize : 256;
    opts.target_ysize = tmpl.target_ysize ? tmpl.target_ysize : 256;
    opts.format.num_channels = tmpl.requested_num_channels;
    opts.format.data_type = tmpl.data_type;
    opts.format.endianness = tmpl.endianness;
    opts.format.align = tmpl.format_align;
    JxlColorEncoding ce = MakeColorEncoding(tmpl.color_preset);
    if (tmpl.use_color_encoding) opts.color_encoding = &ce;
    opts.display_nits = 0.0f;
    opts.allowed_backends = tmpl.allowed_backends;
    opts.preview_hooks = jxl::GetDecoderPreviewHooks();
    opts.cancel = cancel_flag();
    opts.runner = JxlResizableParallelRunner;
    opts.runner_opaque = runner.get();

    uint8_t* out_ptr = nullptr;
    size_t out_size = 0;
    uint32_t w = 0, h = 0;
    size_t stride = 0;
    JxlPixelFormat actual{};
    JxlPreviewBackend backend = JXL_PREVIEW_BACKEND_NONE;
    uint32_t out_factor = 0;
    opts.out_pixels = &out_ptr;
    opts.out_pixels_size = &out_size;
    opts.out_xsize = &w;
    opts.out_ysize = &h;
    opts.out_stride = &stride;
    opts.out_format = &actual;
    opts.out_backend_used = &backend;
    opts.out_downsampling = &out_factor;

    step.status =
        JxlGeneratePreview(reinterpret_cast<const uint8_t*>(bytes.constData()),
                           static_cast<size_t>(bytes.size()), &opts);
    step.elapsed_ms = timer.elapsed();
    if (step.status == JXL_PREVIEW_SUCCESS && out_ptr != nullptr) {
      step.xsize = w;
      step.ysize = h;
      step.stride = stride;
      step.format = actual;
      step.backend = backend;
      step.decode_factor = out_factor;
      step.raw_pixel_bytes = out_size;
      step.raw_bytes.assign(out_ptr, out_ptr + out_size);
      step.display_image = ToDisplayImage(step.raw_bytes, w, h, stride, actual);
      std::free(out_ptr);
      ++succeeded;
    } else {
      if (out_ptr) std::free(out_ptr);
      step.error_text =
          QStringLiteral("Status %1").arg(static_cast<int>(step.status));
      ++failed;
    }
    emit GalleryThumb(i, total, path, step);
  }
  emit GalleryDone(succeeded, failed);
}

// --------------------------------------------------------------------------
// PreviewImageView
// --------------------------------------------------------------------------

PreviewImageView::PreviewImageView(QWidget* parent) : QScrollArea(parent) {
  label_ = new QLabel;
  label_->setAlignment(Qt::AlignCenter);
  label_->setBackgroundRole(QPalette::Dark);
  label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
  setWidget(label_);
  setWidgetResizable(true);
  setBackgroundRole(QPalette::Dark);
  setAlignment(Qt::AlignCenter);
  Clear(tr("(no image)"));
}

void PreviewImageView::SetImage(const QImage& image) {
  image_ = image;
  Rerender();
}

void PreviewImageView::Clear(const QString& placeholder) {
  image_ = QImage();
  label_->setText(placeholder);
  label_->setPixmap(QPixmap());
}

void PreviewImageView::SetFitToWindow(bool fit) {
  if (fit_ == fit) return;
  fit_ = fit;
  setWidgetResizable(fit);
  Rerender();
  emit FitChanged(fit_);
}

void PreviewImageView::ZoomIn() {
  const bool was_fit = fit_;
  fit_ = false;
  setWidgetResizable(false);
  zoom_ = std::min(zoom_ * 1.25, 16.0);
  Rerender();
  if (was_fit) emit FitChanged(false);
}

void PreviewImageView::ZoomOut() {
  const bool was_fit = fit_;
  fit_ = false;
  setWidgetResizable(false);
  zoom_ = std::max(zoom_ / 1.25, 0.0625);
  Rerender();
  if (was_fit) emit FitChanged(false);
}

void PreviewImageView::ZoomReset() {
  const bool was_fit = fit_;
  fit_ = false;
  setWidgetResizable(false);
  zoom_ = 1.0;
  Rerender();
  if (was_fit) emit FitChanged(false);
}

void PreviewImageView::resizeEvent(QResizeEvent* e) {
  QScrollArea::resizeEvent(e);
  if (fit_ && !image_.isNull()) Rerender();
}

void PreviewImageView::Rerender() {
  if (image_.isNull()) return;
  if (fit_) {
    QSize area = viewport()->size();
    QPixmap pm = QPixmap::fromImage(image_).scaled(area, Qt::KeepAspectRatio,
                                                   Qt::SmoothTransformation);
    label_->setPixmap(pm);
    label_->resize(area);
  } else {
    QSize target(static_cast<int>(image_.width() * zoom_),
                 static_cast<int>(image_.height() * zoom_));
    QPixmap pm = QPixmap::fromImage(image_).scaled(
        target, Qt::KeepAspectRatio,
        zoom_ < 1.0 ? Qt::SmoothTransformation : Qt::FastTransformation);
    label_->setPixmap(pm);
    label_->resize(target);
  }
}

// --------------------------------------------------------------------------
// PreviewDemoWindow
// --------------------------------------------------------------------------

PreviewDemoWindow::PreviewDemoWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle(tr("JPEG XL preview API demo"));
  resize(1300, 850);
  setAcceptDrops(true);

  // ---- Actions for the Single-decode tab's local toolbar ----
  // Each mode tab is self-contained; nothing here applies to the other
  // tabs, so we never put it in a global QMainWindow toolbar.
  const auto icon_for = [this](QStyle::StandardPixmap p) {
    return style()->standardIcon(p);
  };
  act_open_ =
      new QAction(icon_for(QStyle::SP_DialogOpenButton), tr("Open..."), this);
  act_open_->setShortcut(QKeySequence::Open);
  act_open_->setToolTip(tr("Add one or more .jxl files to the list (Ctrl+O)."));
  act_remove_ = new QAction(icon_for(QStyle::SP_TrashIcon), tr("Remove"), this);
  act_remove_->setShortcut(QKeySequence::Delete);
  act_remove_->setToolTip(tr("Remove the selected file from the list."));
  act_probe_ = new QAction(icon_for(QStyle::SP_FileDialogContentsView),
                           tr("Probe"), this);
  act_probe_->setShortcut(QKeySequence("Ctrl+P"));
  act_probe_->setToolTip(
      tr("Read header info (size, channels, ICC) without decoding pixels "
         "(Ctrl+P)."));
  act_save_icc_ = new QAction(icon_for(QStyle::SP_DialogSaveButton),
                              tr("Save ICC..."), this);
  act_save_icc_->setEnabled(false);
  act_save_icc_->setToolTip(
      tr("Save the ICC profile retrieved by the most recent Probe."));
  act_decode_ = new QAction(icon_for(QStyle::SP_MediaPlay), tr("Decode"), this);
  act_decode_->setShortcut(QKeySequence("Ctrl+R"));
  act_decode_->setToolTip(
      tr("Run JxlGeneratePreview with the chosen options (Ctrl+R)."));
  act_cancel_ = new QAction(icon_for(QStyle::SP_MediaStop), tr("Cancel"), this);
  act_cancel_->setEnabled(false);
  act_cancel_->setToolTip(tr("Cancel the running decode."));
  act_save_image_ = new QAction(icon_for(QStyle::SP_DialogSaveButton),
                                tr("Save image..."), this);
  act_save_image_->setEnabled(false);
  act_save_image_->setToolTip(tr("Save the rendered preview as PNG/JPEG/BMP."));
  act_zoom_in_ = new QAction(tr("Zoom in"), this);
  act_zoom_in_->setShortcut(QKeySequence::ZoomIn);
  act_zoom_in_->setToolTip(tr("Zoom in (Ctrl++). Disengages Fit mode."));
  act_zoom_out_ = new QAction(tr("Zoom out"), this);
  act_zoom_out_->setShortcut(QKeySequence::ZoomOut);
  act_zoom_out_->setToolTip(tr("Zoom out (Ctrl+-). Disengages Fit mode."));
  act_zoom_reset_ = new QAction(tr("100%"), this);
  act_zoom_reset_->setShortcut(QKeySequence("Ctrl+0"));
  act_zoom_reset_->setToolTip(
      tr("Restore 1:1 pixel zoom (Ctrl+0). Disengages Fit mode."));
  act_zoom_fit_ = new QAction(tr("Fit"), this);
  act_zoom_fit_->setCheckable(true);
  act_zoom_fit_->setChecked(true);
  act_zoom_fit_->setShortcut(QKeySequence("Ctrl+9"));
  act_zoom_fit_->setToolTip(
      tr("Fit the preview image to the available area (Ctrl+9). "
         "Zoom In / Out / 100%% turn this off."));

  // ---- Top-level mode tabs: Single decode / Placeholder / Gallery ----
  // Each tab is a self-contained demo of one usage pattern from
  // examples/decode_preview.cc and owns its own input controls.
  mode_tabs_ = new QTabWidget;
  mode_tabs_->setDocumentMode(true);
  mode_tabs_->addTab(BuildSingleDecodeTab(), tr("Single decode"));
  mode_tabs_->addTab(BuildPlaceholderTab(), tr("Placeholder progression"));
  mode_tabs_->addTab(BuildGalleryTab(), tr("Gallery"));
  mode_tabs_->setTabToolTip(
      0, tr("Decode one file with one chosen set of preview options."));
  mode_tabs_->setTabToolTip(
      1, tr("Decode the same file four times at factors 8 -> 4 -> 2 -> 1, "
            "all into a single reused dst buffer."));
  mode_tabs_->setTabToolTip(
      2, tr("Batch-decode every .jxl in a folder using one shared parallel "
            "runner and one shared cancel flag."));
  setCentralWidget(mode_tabs_);
  // Hide the QMainWindow status bar -- each tab carries its own status text
  // next to the relevant controls, so the global bar is pure noise.
  statusBar()->setVisible(false);

  // ---- Wire actions ----
  connect(act_open_, &QAction::triggered, this,
          &PreviewDemoWindow::OnOpenClicked);
  connect(act_remove_, &QAction::triggered, this,
          &PreviewDemoWindow::OnRemoveSelected);
  connect(act_probe_, &QAction::triggered, this,
          &PreviewDemoWindow::OnProbeClicked);
  connect(act_save_icc_, &QAction::triggered, this,
          &PreviewDemoWindow::OnSaveIccClicked);
  connect(act_decode_, &QAction::triggered, this,
          &PreviewDemoWindow::OnDecodeClicked);
  connect(act_cancel_, &QAction::triggered, this,
          &PreviewDemoWindow::OnCancelClicked);
  connect(act_save_image_, &QAction::triggered, this,
          &PreviewDemoWindow::OnSaveImageClicked);
  connect(act_zoom_in_, &QAction::triggered, image_view_,
          &PreviewImageView::ZoomIn);
  connect(act_zoom_out_, &QAction::triggered, image_view_,
          &PreviewImageView::ZoomOut);
  connect(act_zoom_reset_, &QAction::triggered, image_view_,
          &PreviewImageView::ZoomReset);
  connect(act_zoom_fit_, &QAction::toggled, image_view_,
          &PreviewImageView::SetFitToWindow);
  // Keep the toolbar's checkable 'Fit' action in sync when ZoomIn/Out/
  // Reset turn fit off as a side effect.
  connect(image_view_, &PreviewImageView::FitChanged, act_zoom_fit_,
          &QAction::setChecked);

  connect(file_list_, &QListWidget::currentRowChanged, this,
          [this](int) { OnFileSelectionChanged(); });

  // ---- Worker on background thread ----
  worker_ = new DecodeWorker;
  worker_->moveToThread(&worker_thread_);
  connect(&worker_thread_, &QThread::finished, worker_, &QObject::deleteLater);
  connect(worker_, &DecodeWorker::Finished, this,
          &PreviewDemoWindow::OnDecodeFinished);
  connect(worker_, &DecodeWorker::ProgressiveStep, this,
          &PreviewDemoWindow::OnPlaceholderStep);
  connect(worker_, &DecodeWorker::ProgressiveDone, this,
          &PreviewDemoWindow::OnPlaceholderDone);
  connect(worker_, &DecodeWorker::GalleryThumb, this,
          &PreviewDemoWindow::OnGalleryThumb);
  connect(worker_, &DecodeWorker::GalleryDone, this,
          &PreviewDemoWindow::OnGalleryDone);
  worker_thread_.start();

  RefreshControlsForFile();
}

PreviewDemoWindow::~PreviewDemoWindow() {
  // Cancel any in-flight decode and join the worker thread cleanly.
  if (worker_) worker_->RequestCancel();
  worker_thread_.quit();
  worker_thread_.wait();
}

// ---- Tab factories ------------------------------------------------------

QWidget* PreviewDemoWindow::BuildSingleDecodeTab() {
  // Local toolbar at the top of the tab. Every action here applies only
  // to the Single-decode workflow, so we deliberately avoid putting them
  // in a global QMainWindow toolbar where they would be visible (and
  // misleading) on the Placeholder and Gallery tabs.
  auto* toolbar = new QToolBar;
  toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  toolbar->setMovable(false);
  toolbar->setFloatable(false);
  toolbar->addAction(act_open_);
  toolbar->addAction(act_remove_);
  toolbar->addSeparator();
  toolbar->addAction(act_probe_);
  toolbar->addAction(act_save_icc_);
  toolbar->addSeparator();
  toolbar->addAction(act_decode_);
  toolbar->addAction(act_cancel_);
  toolbar->addAction(act_save_image_);
  toolbar->addSeparator();
  toolbar->addAction(act_zoom_fit_);
  toolbar->addAction(act_zoom_reset_);
  toolbar->addAction(act_zoom_out_);
  toolbar->addAction(act_zoom_in_);

  // Workflow toggle: re-decode automatically whenever any option below
  // changes. Lives on the toolbar (not in the Resources tab) because it
  // governs *when* to decode, not how many resources to spend.
  auto* spacer = new QWidget;
  spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  toolbar->addWidget(spacer);
  auto_decode_check_ = new QCheckBox(tr("Auto-decode"));
  auto_decode_check_->setToolTip(
      tr("When enabled, every change to the Sizing / Format / Color / "
         "Backends / Resources controls immediately re-runs the decode."));
  toolbar->addWidget(auto_decode_check_);

  // Left column: file list above the option-controls tab widget.
  file_list_ = new QListWidget;
  file_list_->setSelectionMode(QAbstractItemView::SingleSelection);
  file_list_->setMinimumWidth(200);
  file_list_->setToolTip(
      tr("Loaded .jxl files. Drop files onto this tab to add more."));

  control_tabs_ = new QTabWidget;
  control_tabs_->addTab(BuildSizingTab(), tr("Sizing"));
  control_tabs_->addTab(BuildFormatTab(), tr("Format"));
  control_tabs_->addTab(BuildColorTab(), tr("Color"));
  control_tabs_->addTab(BuildBackendTab(), tr("Backends"));
  control_tabs_->addTab(BuildResourcesTab(), tr("Resources"));

  auto* left_split = new QSplitter(Qt::Vertical);
  left_split->addWidget(file_list_);
  left_split->addWidget(control_tabs_);
  left_split->setStretchFactor(0, 1);
  left_split->setStretchFactor(1, 2);

  // Center: image viewer + per-tab status + decode progress spinner.
  image_view_ = new PreviewImageView;
  image_view_->setMinimumSize(320, 240);

  single_status_ =
      new QLabel(tr("Open or drop a .jxl file, then click Probe or Decode."));
  single_status_->setTextInteractionFlags(Qt::TextSelectableByMouse);

  // Indeterminate spinner lives next to the status label so the visual
  // busy indicator is co-located with "Decoding...".
  progress_ = new QProgressBar;
  progress_->setRange(0, 0);
  progress_->setVisible(false);
  progress_->setMaximumWidth(120);
  progress_->setTextVisible(false);

  auto* status_row = new QHBoxLayout;
  status_row->setContentsMargins(4, 0, 4, 0);
  status_row->addWidget(progress_);
  status_row->addWidget(single_status_, 1);

  auto* center = new QWidget;
  auto* center_lay = new QVBoxLayout(center);
  center_lay->setContentsMargins(0, 0, 0, 4);
  center_lay->setSpacing(4);
  center_lay->addWidget(image_view_, 1);
  center_lay->addLayout(status_row);

  // Right: probe + decode-result info panes.
  probe_view_ = new QPlainTextEdit;
  probe_view_->setReadOnly(true);
  probe_view_->setStyleSheet("font-family: Consolas, Menlo, monospace;");
  probe_view_->setPlaceholderText(
      tr("Click 'Probe' to read header info (size, channels, ICC) without "
         "decoding the codestream."));
  result_view_ = new QPlainTextEdit;
  result_view_->setReadOnly(true);
  result_view_->setStyleSheet("font-family: Consolas, Menlo, monospace;");
  result_view_->setPlaceholderText(
      tr("Click 'Decode' to run JxlGeneratePreview with the chosen options."));

  auto* info_tabs = new QTabWidget;
  info_tabs->addTab(probe_view_, tr("Probe info"));
  info_tabs->addTab(result_view_, tr("Decode result"));
  info_tabs->setMinimumWidth(360);

  auto* main_split = new QSplitter(Qt::Horizontal);
  main_split->addWidget(left_split);
  main_split->addWidget(center);
  main_split->addWidget(info_tabs);
  main_split->setStretchFactor(0, 0);
  main_split->setStretchFactor(1, 1);
  main_split->setStretchFactor(2, 0);

  auto* tab = new QWidget;
  auto* lay = new QVBoxLayout(tab);
  lay->addWidget(toolbar);
  lay->addWidget(main_split, 1);
  return tab;
}

QWidget* PreviewDemoWindow::BuildSizingTab() {
  auto* w = new QWidget;
  auto* form = new QFormLayout(w);
  sizing_mode_ = new QComboBox;
  sizing_mode_->addItem(tr("Auto from target size (downsampling=0)"));
  sizing_mode_->addItem(tr("Explicit downsampling factor"));
  sizing_mode_->setToolTip(
      tr("Auto: the library picks the smallest native downsampling factor that "
         "fits within target_xsize × target_ysize.\n"
         "Explicit: forces a specific factor; target sizes are ignored."));
  form->addRow(tr("Mode"), sizing_mode_);
  downsample_spin_ = new QSpinBox;
  downsample_spin_->setRange(1, 8);
  downsample_spin_->setValue(1);
  downsample_spin_->setToolTip(
      tr("Snapped to the nearest valid value in {1, 2, 4, 8} at decode time."));
  form->addRow(tr("Factor"), downsample_spin_);
  target_w_spin_ = new QSpinBox;
  target_w_spin_->setRange(0, 65535);
  target_w_spin_->setValue(256);
  target_w_spin_->setToolTip(
      tr("Maximum preview width in pixels. Used in Auto mode; ignored when "
         "Explicit factor is selected. 0 = unconstrained."));
  form->addRow(tr("target_xsize"), target_w_spin_);
  target_h_spin_ = new QSpinBox;
  target_h_spin_->setRange(0, 65535);
  target_h_spin_->setValue(256);
  target_h_spin_->setToolTip(
      tr("Maximum preview height in pixels. Used in Auto mode; ignored when "
         "Explicit factor is selected. 0 = unconstrained."));
  form->addRow(tr("target_ysize"), target_h_spin_);
  connect(sizing_mode_, qOverload<int>(&QComboBox::currentIndexChanged), this,
          &PreviewDemoWindow::OnAutoChanged);
  for (auto* s : {downsample_spin_, target_w_spin_, target_h_spin_}) {
    connect(s, qOverload<int>(&QSpinBox::valueChanged), this,
            &PreviewDemoWindow::OnAutoChanged);
  }
  return w;
}

QWidget* PreviewDemoWindow::BuildFormatTab() {
  auto* w = new QWidget;
  auto* form = new QFormLayout(w);
  data_type_combo_ = new QComboBox;
  data_type_combo_->addItem(tr("UINT8"), static_cast<int>(JXL_TYPE_UINT8));
  data_type_combo_->addItem(tr("UINT16"), static_cast<int>(JXL_TYPE_UINT16));
  data_type_combo_->addItem(tr("FLOAT16"), static_cast<int>(JXL_TYPE_FLOAT16));
  data_type_combo_->addItem(tr("FLOAT"), static_cast<int>(JXL_TYPE_FLOAT));
  data_type_combo_->setToolTip(
      tr("Requested sample data type for the output pixel buffer. "
         "UINT8 is the most widely compatible; FLOAT gives the full "
         "HDR range when combined with an appropriate display_nits setting."));
  form->addRow(tr("data_type"), data_type_combo_);
  num_channels_spin_ = new QSpinBox;
  num_channels_spin_->setRange(0, 4);
  num_channels_spin_->setValue(0);
  num_channels_spin_->setToolTip(
      tr("0 = library default (RGBA8 layout). Otherwise treated as a "
         "preference; the actual output channel count matches the source."));
  form->addRow(tr("num_channels"), num_channels_spin_);
  endian_combo_ = new QComboBox;
  endian_combo_->addItem(tr("NATIVE"), static_cast<int>(JXL_NATIVE_ENDIAN));
  endian_combo_->addItem(tr("LITTLE"), static_cast<int>(JXL_LITTLE_ENDIAN));
  endian_combo_->addItem(tr("BIG"), static_cast<int>(JXL_BIG_ENDIAN));
  endian_combo_->setToolTip(
      tr("Byte order for multi-byte sample types (UINT16, FLOAT16, FLOAT). "
         "NATIVE matches the host CPU; ignored for UINT8."));
  form->addRow(tr("endianness"), endian_combo_);
  align_spin_ = new QSpinBox;
  align_spin_->setRange(0, 4096);
  align_spin_->setSingleStep(16);
  align_spin_->setValue(0);
  align_spin_->setToolTip(tr("Row alignment in bytes. 0 = tightly packed."));
  form->addRow(tr("format.align"), align_spin_);

  auto* dst_box = new QGroupBox(tr("Caller-supplied dst buffer"));
  auto* dst_form = new QFormLayout(dst_box);
  dst_check_ = new QCheckBox(tr("Use dst (worst-case-sized)"));
  dst_check_->setToolTip(tr(
      "Supply a caller-allocated output buffer instead of letting the library "
      "allocate one. The buffer is sized for the full-resolution RGBA32F "
      "worst case so it can safely hold any preview the API produces."));
  dst_form->addRow(dst_check_);
  dst_stride_spin_ = new QSpinBox;
  dst_stride_spin_->setRange(0, 1 << 20);
  dst_stride_spin_->setSingleStep(64);
  dst_stride_spin_->setValue(0);
  dst_stride_spin_->setToolTip(
      tr("dst_stride in bytes. 0 = tightly packed. A stride below the row size "
         "returns BUFFER_TOO_SMALL with the stride and size the buffer "
         "needs."));
  dst_form->addRow(tr("dst_stride"), dst_stride_spin_);
  dst_overprovision_spin_ = new QSpinBox;
  dst_overprovision_spin_->setRange(0, 1 << 24);
  dst_overprovision_spin_->setSingleStep(1024);
  dst_overprovision_spin_->setValue(0);
  dst_overprovision_spin_->setToolTip(
      tr("Extra bytes to add to dst_size beyond the worst-case requirement."));
  dst_form->addRow(tr("+ extra dst_size"), dst_overprovision_spin_);
  dst_undersize_check_ =
      new QCheckBox(tr("Force buffer too small (expect BUFFER_TOO_SMALL)"));
  dst_undersize_check_->setToolTip(
      tr("Intentionally sizes the dst buffer to 16 bytes so the API returns "
         "JXL_PREVIEW_BUFFER_TOO_SMALL, which reports the size the buffer "
         "needs. Useful for testing caller error handling."));
  dst_form->addRow(dst_undersize_check_);
  form->addRow(dst_box);

  for (auto* c : {data_type_combo_, endian_combo_}) {
    connect(c, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &PreviewDemoWindow::OnAutoChanged);
  }
  for (auto* s : {num_channels_spin_, align_spin_, dst_stride_spin_,
                  dst_overprovision_spin_}) {
    connect(s, qOverload<int>(&QSpinBox::valueChanged), this,
            &PreviewDemoWindow::OnAutoChanged);
  }
  for (auto* c : {dst_check_, dst_undersize_check_}) {
    connect(c, &QCheckBox::toggled, this, &PreviewDemoWindow::OnAutoChanged);
  }
  return w;
}

QWidget* PreviewDemoWindow::BuildColorTab() {
  auto* w = new QWidget;
  auto* form = new QFormLayout(w);
  color_enable_check_ =
      new QCheckBox(tr("Override color_encoding (else: library default sRGB)"));
  color_enable_check_->setChecked(true);
  color_enable_check_->setToolTip(
      tr("When checked, the selected color space preset is passed to the API "
         "via opts.color_encoding. When unchecked, the field is left null and "
         "the library falls back to its built-in sRGB default."));
  form->addRow(color_enable_check_);
  color_combo_ = new QComboBox;
  color_combo_->addItem(tr("sRGB"));
  color_combo_->addItem(tr("Linear sRGB"));
  color_combo_->addItem(tr("Display P3"));
  color_combo_->addItem(tr("Rec.2020 primaries, sRGB transfer"));
  color_combo_->setToolTip(
      tr("Color space the output pixels should be converted into. "
         "Only active when the override checkbox above is enabled."));
  form->addRow(tr("Preset"), color_combo_);
  nits_mode_combo_ = new QComboBox;
  nits_mode_combo_->addItem(tr("Auto SDR (~250 nits)"));
  nits_mode_combo_->addItem(tr("No tone mapping"));
  nits_mode_combo_->addItem(tr("Explicit display peak"));
  nits_mode_combo_->setToolTip(
      tr("Auto: library applies a standard SDR tone-mapping curve "
         "(~250 nits peak).\n"
         "No tone mapping: passes JXL_PREVIEW_NO_TONE_MAPPING; HDR content is "
         "not compressed to SDR range.\n"
         "Explicit: uses the nits value below as the display peak luminance."));
  form->addRow(tr("display_nits mode"), nits_mode_combo_);
  nits_spin_ = new QSpinBox;
  nits_spin_->setRange(1, 10000);
  nits_spin_->setValue(100);
  nits_spin_->setSuffix(tr(" nits"));
  nits_spin_->setToolTip(
      tr("Peak display luminance used for tone mapping when mode is set to "
         "\"Explicit display peak\". Typical SDR monitors: 100–300 nits; "
         "HDR displays: 1000+ nits."));
  form->addRow(tr("Explicit value"), nits_spin_);
  for (auto* c : {color_combo_, nits_mode_combo_}) {
    connect(c, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &PreviewDemoWindow::OnAutoChanged);
  }
  connect(color_enable_check_, &QCheckBox::toggled, this,
          &PreviewDemoWindow::OnAutoChanged);
  connect(nits_spin_, qOverload<int>(&QSpinBox::valueChanged), this,
          &PreviewDemoWindow::OnAutoChanged);
  return w;
}

QWidget* PreviewDemoWindow::BuildBackendTab() {
  auto* w = new QWidget;
  auto* v = new QVBoxLayout(w);
  v->addWidget(new QLabel(
      tr("Bitmask of allowed backends. Empty = all backends allowed.")));
  backend_full_decode_ = new QCheckBox(tr("Full decode (factor 1)"));
  backend_full_decode_->setToolTip(
      tr("Decode at full resolution when the factor is 1. A non-empty mask "
         "without it makes factor 1, also when picked from the target size, "
         "fail with NO_BACKEND_AVAILABLE."));
  backend_embedded_ = new QCheckBox(tr("Embedded preview frame"));
  backend_embedded_->setToolTip(tr(
      "Use the small JPEG XL preview frame embedded in the file header. "
      "Very fast but limited to the preview resolution baked into the file."));
  backend_dc_ = new QCheckBox(tr("Native DC-only (1/8)"));
  backend_dc_->setToolTip(
      tr("Decode only the DC coefficients — always 1/8 resolution. "
         "Fast and memory-efficient; works on any VarDCT image."));
  backend_progression_flush_ = new QCheckBox(tr("Native progression flush"));
  backend_progression_flush_->setToolTip(
      tr("Flush an incomplete progressive scan at the requested downsampling "
         "factor. Produces a downsampled image at any factor in {2,4,8}."));
  backend_modular_ = new QCheckBox(tr("Native reduced-input"));
  backend_modular_->setToolTip(
      tr("Use native reduced-resolution decode work where supported, including "
         "Modular reduced input and VarDCT reduced IDCT."));
  backend_fallback_ = new QCheckBox(tr("Fallback downsample"));
  backend_fallback_->setToolTip(
      tr("If no native backend can satisfy the request, decode the full image "
         "and scale it down. Always succeeds but is the slowest and most "
         "memory-hungry option."));
  for (auto* c :
       {backend_full_decode_, backend_embedded_, backend_dc_,
        backend_progression_flush_, backend_modular_, backend_fallback_}) {
    v->addWidget(c);
    connect(c, &QCheckBox::toggled, this, &PreviewDemoWindow::OnAutoChanged);
  }
  auto* row = new QHBoxLayout;
  backend_all_ = new QPushButton(tr("All"));
  backend_all_->setToolTip(tr("Check all backends (explicit full mask)."));
  backend_none_ = new QPushButton(tr("None (= all allowed)"));
  backend_none_->setToolTip(
      tr("Uncheck all backends. An empty mask (0) is the API default and "
         "means every backend is permitted."));
  row->addWidget(backend_all_);
  row->addWidget(backend_none_);
  row->addStretch(1);
  v->addLayout(row);
  v->addStretch(1);
  connect(backend_all_, &QPushButton::clicked, this, [this] {
    for (auto* c :
         {backend_full_decode_, backend_embedded_, backend_dc_,
          backend_progression_flush_, backend_modular_, backend_fallback_})
      c->setChecked(true);
  });
  connect(backend_none_, &QPushButton::clicked, this, [this] {
    for (auto* c :
         {backend_full_decode_, backend_embedded_, backend_dc_,
          backend_progression_flush_, backend_modular_, backend_fallback_})
      c->setChecked(false);
  });
  return w;
}

QWidget* PreviewDemoWindow::BuildResourcesTab() {
  auto* w = new QWidget;
  auto* form = new QFormLayout(w);
  threads_spin_ = new QSpinBox;
  threads_spin_->setRange(0, 64);
  threads_spin_->setValue(0);
  threads_spin_->setToolTip(tr("0 = single-threaded (no runner installed)."));
  form->addRow(tr("Worker threads"), threads_spin_);
  mm_check_ = new QCheckBox(tr("Track allocations (counting memory manager)"));
  mm_check_->setToolTip(
      tr("Installs a custom JxlMemoryManager that counts every alloc/free call "
         "and accumulates total and peak live bytes. Results appear in the "
         "Decode result panel after each decode."));
  form->addRow(mm_check_);
  connect(threads_spin_, qOverload<int>(&QSpinBox::valueChanged), this,
          &PreviewDemoWindow::OnAutoChanged);
  connect(mm_check_, &QCheckBox::toggled, this,
          &PreviewDemoWindow::OnAutoChanged);
  return w;
}

QWidget* PreviewDemoWindow::BuildPlaceholderTab() {
  // Demonstrates the "placeholder" pattern: progressively sharper previews
  // (factors 8 -> 4 -> 2 -> 1) all decoded into ONE caller-allocated dst
  // buffer, with the fallback-downsample backend disallowed so each step
  // either produces a native preview or surfaces NO_BACKEND_AVAILABLE.
  //
  // This tab is fully self-contained: the user opens one file with the
  // local Open button, then runs the progression. None of the
  // Single-decode controls are consulted here; the pattern uses fixed
  // settings that match the upstream example.
  auto* w = new QWidget;
  auto* lay = new QVBoxLayout(w);

  // ---- Local toolbar ----
  auto* toolbar = new QToolBar;
  toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  toolbar->setMovable(false);
  toolbar->setFloatable(false);
  ph_open_button_ =
      new QPushButton(style()->standardIcon(QStyle::SP_DialogOpenButton),
                      tr("Open .jxl file..."));
  ph_open_button_->setToolTip(tr("Pick the file to progressively decode."));
  ph_run_button_ =
      new QPushButton(style()->standardIcon(QStyle::SP_MediaPlay),
                      tr("Run progression  8 \u2192 4 \u2192 2 \u2192 1"));
  ph_run_button_->setEnabled(false);
  ph_run_button_->setToolTip(
      tr("Decode the file four times, each into the same dst buffer, with "
         "fallback downsample forbidden."));
  ph_cancel_button_ = new QPushButton(
      style()->standardIcon(QStyle::SP_MediaStop), tr("Cancel"));
  ph_cancel_button_->setEnabled(false);
  ph_cancel_button_->setToolTip(tr("Abort the running progression."));
  toolbar->addWidget(ph_open_button_);
  toolbar->addWidget(ph_run_button_);
  toolbar->addWidget(ph_cancel_button_);
  toolbar->addSeparator();
  ph_path_label_ = new QLabel(tr("(no file loaded)"));
  ph_path_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  ph_path_label_->setMinimumWidth(200);
  toolbar->addWidget(ph_path_label_);
  lay->addWidget(toolbar);

  // ---- Image | per-step log ----
  auto* split = new QSplitter(Qt::Horizontal);
  ph_image_view_ = new PreviewImageView;
  ph_image_view_->setMinimumSize(320, 240);
  ph_image_view_->Clear(
      tr("Open a .jxl file with the toolbar, then press Run."));
  ph_log_ = new QPlainTextEdit;
  ph_log_->setReadOnly(true);
  ph_log_->setStyleSheet("font-family: Consolas, Menlo, monospace;");
  ph_log_->setPlaceholderText(
      tr("Per-step log: factor, backend, output size, elapsed time."));
  ph_log_->setMinimumWidth(280);
  split->addWidget(ph_image_view_);
  split->addWidget(ph_log_);
  split->setStretchFactor(0, 3);
  split->setStretchFactor(1, 2);
  lay->addWidget(split, 1);

  // ---- Status line ----
  ph_status_ =
      new QLabel(tr("Open a .jxl file with the toolbar, then press Run."));
  lay->addWidget(ph_status_);

  connect(ph_open_button_, &QPushButton::clicked, this,
          &PreviewDemoWindow::OnPlaceholderOpenClicked);
  connect(ph_run_button_, &QPushButton::clicked, this,
          &PreviewDemoWindow::OnPlaceholderRunClicked);
  connect(ph_cancel_button_, &QPushButton::clicked, this,
          &PreviewDemoWindow::OnPlaceholderCancelClicked);
  return w;
}

// ---------------------------------------------------------------------------
// GalleryIconDelegate -- paints only the icon (no label, no text space).
// This is the only reliable way to get a tight icon grid in QListWidget
// IconMode; even an empty-string item reserves text space in the default
// delegate, so the grid cell is always larger than the icon.
// ---------------------------------------------------------------------------
namespace {
class GalleryIconDelegate : public QStyledItemDelegate {
 public:
  explicit GalleryIconDelegate(QObject* parent = nullptr)
      : QStyledItemDelegate(parent) {}

  void paint(QPainter* p, const QStyleOptionViewItem& option,
             const QModelIndex& index) const override {
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    const bool selected = opt.state & QStyle::State_Selected;
    const bool hovered = opt.state & QStyle::State_MouseOver;

    // 1. Draw icon centered in the full cell rect.
    if (!opt.icon.isNull()) {
      const QPixmap pm = opt.icon.pixmap(opt.decorationSize);
      const QPoint tl =
          opt.rect.topLeft() + QPoint((opt.rect.width() - pm.width()) / 2,
                                      (opt.rect.height() - pm.height()) / 2);
      p->drawPixmap(tl, pm);
    }

    // 2. Draw highlights on TOP of the image so they are always visible,
    //    even when the thumbnail fills the entire cell.
    //    Selection: semi-transparent color wash.
    //    Hover:     inset border ring only (no wash, just an outline).
    if (selected) {
      QColor c = opt.palette.color(QPalette::Highlight);
      c.setAlpha(80);  // ~31% wash
      p->fillRect(opt.rect, c);
      // Solid 2 px border for extra crispness.
      c.setAlpha(200);
      p->setPen(QPen(c, 2));
      p->drawRect(opt.rect.adjusted(1, 1, -1, -1));
    } else if (hovered) {
      QColor c = opt.palette.color(QPalette::Highlight).lighter(140);
      c.setAlpha(180);
      p->setPen(QPen(c, 2));
      p->drawRect(opt.rect.adjusted(1, 1, -1, -1));
    }
  }

  QSize sizeHint(const QStyleOptionViewItem& option,
                 const QModelIndex& /*index*/) const override {
    // Exact icon size -- the grid cell size (set on the view) provides the
    // outer margin, so sizeHint itself can be tight.
    return option.decorationSize;
  }
};
}  // namespace

QWidget* PreviewDemoWindow::BuildGalleryTab() {
  // Demonstrates the "gallery" batch pattern: pick a folder, enumerate
  // *.jxl files, decode every file with one shared parallel runner and
  // one shared atomic cancel flag, populating an icon grid as
  // thumbnails arrive.
  //
  // Self-contained: the only inputs are the local toolbar's Open folder
  // button and the thumbnail-size / threads spinboxes.
  auto* w = new QWidget;
  auto* lay = new QVBoxLayout(w);

  // ---- Local toolbar ----
  auto* toolbar = new QToolBar;
  toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  toolbar->setMovable(false);
  toolbar->setFloatable(false);
  gal_open_folder_button_ = new QPushButton(
      style()->standardIcon(QStyle::SP_DirOpenIcon), tr("Open folder..."));
  gal_open_folder_button_->setToolTip(
      tr("Pick a folder; every .jxl file in it will be batch-decoded."));
  gal_redecode_button_ = new QPushButton(
      style()->standardIcon(QStyle::SP_BrowserReload), tr("Redecode"));
  gal_redecode_button_->setEnabled(false);
  gal_redecode_button_->setToolTip(
      tr("Re-decode every .jxl file in the current folder with the current "
         "thumbnail size and thread-count settings."));
  gal_cancel_button_ = new QPushButton(
      style()->standardIcon(QStyle::SP_MediaStop), tr("Cancel batch"));
  gal_cancel_button_->setEnabled(false);
  gal_cancel_button_->setToolTip(tr("Abort the batch decode in progress."));
  toolbar->addWidget(gal_open_folder_button_);
  toolbar->addWidget(gal_redecode_button_);
  toolbar->addWidget(gal_cancel_button_);
  toolbar->addSeparator();
  auto* sz_label = new QLabel(tr("Thumbnail size:"));
  toolbar->addWidget(sz_label);
  gal_thumb_size_spin_ = new QSpinBox;
  gal_thumb_size_spin_->setRange(64, 512);
  gal_thumb_size_spin_->setValue(192);
  gal_thumb_size_spin_->setSingleStep(16);
  gal_thumb_size_spin_->setSuffix(tr(" px"));
  gal_thumb_size_spin_->setToolTip(
      tr("Target width and height for each thumbnail (square). Changing this "
         "resizes the grid cells immediately; press Redecode to re-generate "
         "thumbnails at the new resolution."));
  toolbar->addWidget(gal_thumb_size_spin_);
  toolbar->addSeparator();
  auto* threads_label = new QLabel(tr("Runner threads:"));
  toolbar->addWidget(threads_label);
  gal_threads_spin_ = new QSpinBox;
  gal_threads_spin_->setRange(1, 64);
  gal_threads_spin_->setValue(4);
  gal_threads_spin_->setToolTip(
      tr("All files in the batch share one JxlResizableParallelRunner with "
         "this many worker threads."));
  toolbar->addWidget(gal_threads_spin_);

  // ---- Grid | selected-thumbnail info ----
  // Icon-only grid: no text label in the cell; filename lives in the
  // tooltip and in the info panel on the right. The custom delegate is the
  // only reliable approach -- QListWidget IconMode always reserves text
  // space in the default delegate regardless of empty item text.
  auto* split = new QSplitter(Qt::Horizontal);

  gal_grid_ = new QListWidget;
  gal_grid_->setViewMode(QListView::IconMode);
  gal_grid_->setResizeMode(QListView::Adjust);
  gal_grid_->setMovement(QListView::Static);
  gal_grid_->setUniformItemSizes(true);
  gal_grid_->setSpacing(4);
  const int sz = gal_thumb_size_spin_->value();
  gal_grid_->setIconSize(QSize(sz, sz));
  // gridSize = icon + uniform 8 px margin on all sides.
  gal_grid_->setGridSize(QSize(sz + 16, sz + 16));
  gal_grid_->setItemDelegate(new GalleryIconDelegate(gal_grid_));
  gal_grid_->setToolTip(
      tr("Double-click a thumbnail to inspect it on the Single decode tab."));
  split->addWidget(gal_grid_);

  gal_info_ = new QPlainTextEdit;
  gal_info_->setReadOnly(true);
  gal_info_->setStyleSheet("font-family: Consolas, Menlo, monospace;");
  gal_info_->setPlaceholderText(
      tr("Select a thumbnail to see its decode details (size, backend, "
         "format, elapsed time)."));
  gal_info_->setMinimumWidth(280);
  split->addWidget(gal_info_);
  split->setStretchFactor(0, 3);
  split->setStretchFactor(1, 1);

  // ---- Progress + status ----
  gal_progress_ = new QProgressBar;
  gal_progress_->setVisible(false);
  gal_progress_->setTextVisible(true);
  gal_progress_->setMaximumWidth(220);
  gal_status_ = new QLabel(
      tr("Open or drop a folder of .jxl files to batch-decode their previews "
         "using a single shared runner and a single cancel flag."));
  gal_status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  auto* progress_row = new QHBoxLayout;
  progress_row->setContentsMargins(4, 0, 4, 0);
  progress_row->addWidget(gal_status_, 1);
  progress_row->addWidget(gal_progress_);

  lay->addWidget(toolbar);
  lay->addWidget(split, 1);
  lay->addLayout(progress_row);

  connect(gal_open_folder_button_, &QPushButton::clicked, this,
          &PreviewDemoWindow::OnGalleryOpenFolderClicked);
  connect(gal_redecode_button_, &QPushButton::clicked, this, [this]() {
    if (!gal_dir_.isEmpty()) RunGalleryBatch(gal_dir_);
  });
  connect(gal_cancel_button_, &QPushButton::clicked, this,
          &PreviewDemoWindow::OnGalleryCancelClicked);
  // Size spin only updates the grid geometry; the user must press Redecode
  // to actually re-run the batch at the new size.
  connect(gal_thumb_size_spin_, qOverload<int>(&QSpinBox::valueChanged), this,
          [this](int v) {
            gal_grid_->setIconSize(QSize(v, v));
            gal_grid_->setGridSize(QSize(v + 16, v + 16));
          });
  // Threads spin has no immediate side-effect; value is read at batch start.
  connect(gal_grid_, &QListWidget::itemActivated, this,
          [this](QListWidgetItem*) { OnGalleryItemActivated(); });
  connect(gal_grid_, &QListWidget::currentRowChanged, this,
          [this](int) { OnGallerySelectionChanged(); });
  return w;
}

// ---- File handling ------------------------------------------------------

void PreviewDemoWindow::OpenFiles(const QStringList& paths) {
  int first_added = static_cast<int>(files_.size());
  for (const QString& path : paths) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
      single_status_->setText(tr("Could not open: %1").arg(path));
      continue;
    }
    LoadedFile lf;
    lf.path = path;
    lf.bytes = f.readAll();
    files_.push_back(std::move(lf));
    file_list_->addItem(QFileInfo(path).fileName());
    file_list_->item(file_list_->count() - 1)->setToolTip(path);
  }
  if (file_list_->currentRow() < 0 && !files_.empty()) {
    file_list_->setCurrentRow(first_added);
  }
}

void PreviewDemoWindow::OnOpenClicked() {
  const QStringList paths =
      QFileDialog::getOpenFileNames(this, tr("Open .jxl files"), QString(),
                                    tr("JPEG XL files (*.jxl);;All files (*)"));
  if (!paths.isEmpty()) OpenFiles(paths);
}

void PreviewDemoWindow::OnRemoveSelected() {
  int row = file_list_->currentRow();
  if (row < 0 || row >= static_cast<int>(files_.size())) return;
  files_.erase(files_.begin() + row);
  delete file_list_->takeItem(row);
}

void PreviewDemoWindow::OnFileSelectionChanged() {
  current_file_ = file_list_->currentRow();
  RefreshControlsForFile();
}

void PreviewDemoWindow::RefreshControlsForFile() {
  bool have =
      current_file_ >= 0 && current_file_ < static_cast<int>(files_.size());
  act_probe_->setEnabled(have);
  act_decode_->setEnabled(have);
  act_remove_->setEnabled(have);
  act_save_icc_->setEnabled(false);
  act_save_image_->setEnabled(false);
  last_icc_.clear();
  last_image_ = QImage();
  image_view_->Clear(have ? tr("Click 'Decode' to render a preview.")
                          : tr("Open a .jxl file to begin."));
  single_status_->setText(
      have ? tr("Click 'Probe' or 'Decode' to inspect this file.")
           : tr("Open or drop a .jxl file, then click Probe or Decode."));
  probe_view_->clear();
  result_view_->clear();
}

void PreviewDemoWindow::dragEnterEvent(QDragEnterEvent* e) {
  if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void PreviewDemoWindow::dropEvent(QDropEvent* e) {
  QStringList paths;
  for (const QUrl& url : e->mimeData()->urls()) {
    if (url.isLocalFile()) paths << url.toLocalFile();
  }
  if (paths.isEmpty()) return;
  // Route the drop to whichever mode tab is active so the dropped
  // payload lands where the user is actually working.
  switch (mode_tabs_->currentIndex()) {
    case 0: {
      // Single decode: accept files (folders are ignored here).
      QStringList files;
      for (const QString& p : paths) {
        if (QFileInfo(p).isFile()) files << p;
      }
      if (!files.isEmpty()) OpenFiles(files);
      break;
    }
    case 1: {
      // Placeholder: load the first file, ignore extras.
      QString file;
      for (const QString& p : paths) {
        if (QFileInfo(p).isFile()) {
          file = p;
          break;
        }
      }
      if (file.isEmpty()) break;
      QFile f(file);
      if (f.open(QIODevice::ReadOnly)) {
        ph_path_ = file;
        ph_bytes_ = f.readAll();
        ph_path_label_->setText(QFileInfo(ph_path_).fileName());
        ph_path_label_->setToolTip(ph_path_);
        ph_run_button_->setEnabled(true);
        ph_image_view_->Clear(
            tr("Press Run to decode 8 \u2192 4 \u2192 2 "
               "\u2192 1 into one shared dst buffer."));
        ph_log_->clear();
        ph_status_->setText(tr("Loaded %1.").arg(ph_path_));
      }
      break;
    }
    case 2: {
      // Gallery: a dropped folder runs a batch on it directly; a dropped
      // file is interpreted as "batch the folder this file lives in".
      QString dir;
      for (const QString& p : paths) {
        QFileInfo fi(p);
        if (fi.isDir()) {
          dir = fi.absoluteFilePath();
          break;
        }
      }
      if (dir.isEmpty()) {
        for (const QString& p : paths) {
          QFileInfo fi(p);
          if (fi.isFile()) {
            dir = fi.absolutePath();
            break;
          }
        }
      }
      if (!dir.isEmpty()) RunGalleryBatch(dir);
      break;
    }
  }
}

// ---- Probe / Decode -----------------------------------------------------

void PreviewDemoWindow::OnProbeClicked() {
  if (current_file_ < 0) return;
  const QByteArray& jxl = files_[current_file_].bytes;

  // Always retrieve ICC so the user can see how big it is and save it.
  uint8_t* icc = nullptr;
  size_t icc_size = 0;
  JxlPreviewInfoQuery query{};
  query.target_xsize = static_cast<uint32_t>(target_w_spin_->value());
  query.target_ysize = static_cast<uint32_t>(target_h_spin_->value());
  query.out_icc = &icc;
  query.out_icc_size = &icc_size;

  JxlPreviewInfo info{};
  JxlPreviewStatus st =
      JxlGetPreviewInfo(reinterpret_cast<const uint8_t*>(jxl.constData()),
                        static_cast<size_t>(jxl.size()), &query, &info);
  if (st != JXL_PREVIEW_SUCCESS) {
    probe_view_->setPlainText(tr("Probe failed: %1").arg(StatusName(st)));
    last_icc_.clear();
    act_save_icc_->setEnabled(false);
    std::free(icc);
    return;
  }
  if (icc != nullptr) {
    last_icc_ = QByteArray(reinterpret_cast<const char*>(icc),
                           static_cast<int>(icc_size));
    std::free(icc);
    act_save_icc_->setEnabled(true);
  } else {
    last_icc_.clear();
    act_save_icc_->setEnabled(false);
  }
  probe_view_->setPlainText(
      FormatPreviewInfo(info, !last_icc_.isEmpty(), last_icc_.size()));
}

void PreviewDemoWindow::OnSaveIccClicked() {
  if (last_icc_.isEmpty()) return;
  const QString path = QFileDialog::getSaveFileName(
      this, tr("Save ICC profile"),
      QFileInfo(files_[current_file_].path).completeBaseName() + ".icc",
      tr("ICC profiles (*.icc *.icm);;All files (*)"));
  if (path.isEmpty()) return;
  QFile f(path);
  if (f.open(QIODevice::WriteOnly)) {
    f.write(last_icc_);
    single_status_->setText(
        tr("Wrote %1 bytes to %2").arg(last_icc_.size()).arg(path));
  } else {
    QMessageBox::warning(this, tr("Save ICC"),
                         tr("Could not write %1").arg(path));
  }
}

DecodeWorker::Request PreviewDemoWindow::BuildRequestForBytes(
    const QByteArray& bytes) {
  DecodeWorker::Request req;
  req.jxl = bytes;

  if (sizing_mode_->currentIndex() == 1) {
    // Explicit factor; UI exposes 1..8 but the API only accepts {1,2,4,8}.
    int v = downsample_spin_->value();
    int valid = 1;
    for (int f : {1, 2, 4, 8}) {
      if (v >= f) valid = f;
    }
    req.preview_downsampling = static_cast<uint32_t>(valid);
    req.target_xsize = 0;
    req.target_ysize = 0;
  } else {
    req.preview_downsampling = 0;
    req.target_xsize = static_cast<uint32_t>(target_w_spin_->value());
    req.target_ysize = static_cast<uint32_t>(target_h_spin_->value());
  }

  req.requested_num_channels =
      static_cast<uint32_t>(num_channels_spin_->value());
  req.data_type =
      static_cast<JxlDataType>(data_type_combo_->currentData().toInt());
  req.endianness =
      static_cast<JxlEndianness>(endian_combo_->currentData().toInt());
  req.format_align = static_cast<size_t>(align_spin_->value());

  req.use_color_encoding = color_enable_check_->isChecked();
  req.color_preset = color_combo_->currentIndex();
  req.nits_mode = nits_mode_combo_->currentIndex();
  req.display_nits = static_cast<float>(nits_spin_->value());

  uint32_t mask = 0;
  if (backend_full_decode_->isChecked())
    mask |= JXL_PREVIEW_BACKEND_BIT_FULL_DECODE;
  if (backend_embedded_->isChecked())
    mask |= JXL_PREVIEW_BACKEND_BIT_EMBEDDED_PREVIEW;
  if (backend_dc_->isChecked()) mask |= JXL_PREVIEW_BACKEND_BIT_NATIVE_DC_ONLY;
  if (backend_progression_flush_->isChecked())
    mask |= JXL_PREVIEW_BACKEND_BIT_NATIVE_PROGRESSION_FLUSH;
  if (backend_modular_->isChecked())
    mask |= JXL_PREVIEW_BACKEND_BIT_NATIVE_REDUCED_INPUT;
  if (backend_modular_->isChecked())
    mask |= JXL_PREVIEW_BACKEND_BIT_NATIVE_FUSED_UPSAMPLING;
  if (backend_fallback_->isChecked())
    mask |= JXL_PREVIEW_BACKEND_BIT_FALLBACK_DOWNSAMPLE;
  req.allowed_backends = mask;

  req.num_threads = threads_spin_->value();
  req.use_memory_manager = mm_check_->isChecked();
  req.use_caller_buffer = dst_check_->isChecked();
  req.dst_stride_override = static_cast<size_t>(dst_stride_spin_->value());
  req.dst_overprovision = dst_overprovision_spin_->value();
  req.dst_undersize = dst_undersize_check_->isChecked();
  return req;
}

bool PreviewDemoWindow::BuildRequest(DecodeWorker::Request* req) {
  if (current_file_ < 0) return false;
  *req = BuildRequestForBytes(files_[current_file_].bytes);
  return true;
}

void PreviewDemoWindow::OnDecodeClicked() {
  DecodeWorker::Request req;
  if (!BuildRequest(&req)) {
    QMessageBox::information(this, tr("No file"),
                             tr("Open a .jxl file first."));
    return;
  }
  worker_->ResetCancel();
  SetBusy(true);
  single_status_->setText(tr("Decoding..."));
  // Queued connection (worker lives on a different thread).
  QMetaObject::invokeMethod(worker_, "Decode", Qt::QueuedConnection,
                            Q_ARG(jpegxl::tools::DecodeWorker::Request, req));
}

void PreviewDemoWindow::OnCancelClicked() {
  worker_->RequestCancel();
  single_status_->setText(tr("Cancel requested..."));
}

void PreviewDemoWindow::OnSaveImageClicked() {
  if (last_image_.isNull()) return;
  const QFileInfo src_fi(files_[current_file_].path);
  const QString suggest =
      src_fi.absolutePath() + "/" + src_fi.completeBaseName() + "_preview.png";
  const QString path = QFileDialog::getSaveFileName(
      this, tr("Save preview image"), suggest,
      tr("PNG (*.png);;JPEG (*.jpg);;BMP (*.bmp);;All files (*)"));
  if (path.isEmpty()) return;
  if (last_image_.save(path)) {
    single_status_->setText(tr("Wrote preview to %1").arg(path));
  } else {
    QMessageBox::warning(this, tr("Save image"),
                         tr("Could not write %1").arg(path));
  }
}

void PreviewDemoWindow::OnDecodeFinished(const DecodeWorker::Result& result) {
  SetBusy(false);
  result_view_->setPlainText(FormatResult(result));
  if (result.status != JXL_PREVIEW_SUCCESS) {
    image_view_->Clear(StatusName(result.status));
    last_image_ = QImage();
    act_save_image_->setEnabled(false);
    single_status_->setText(tr("Status: %1 (%2 ms)")
                                .arg(StatusName(result.status))
                                .arg(result.elapsed_ms));
    return;
  }
  last_image_ = result.display_image;
  image_view_->SetImage(last_image_);
  act_save_image_->setEnabled(true);
  single_status_->setText(tr("Decoded %1\u00d7%2 via %3 in %4 ms")
                              .arg(result.xsize)
                              .arg(result.ysize)
                              .arg(BackendName(result.backend))
                              .arg(result.elapsed_ms));
}

void PreviewDemoWindow::OnAutoChanged() {
  if (auto_decode_check_ && auto_decode_check_->isChecked()) {
    TriggerAutoDecode();
  }
}

void PreviewDemoWindow::TriggerAutoDecode() {
  // Don't queue more than one outstanding decode -- if the worker is busy,
  // skip silently. The next change will retry.
  if (current_file_ < 0) return;
  if (!act_decode_->isEnabled()) return;
  OnDecodeClicked();
}

void PreviewDemoWindow::SetBusy(bool busy) {
  act_open_->setEnabled(!busy);
  act_remove_->setEnabled(!busy && current_file_ >= 0);
  act_probe_->setEnabled(!busy && current_file_ >= 0);
  act_decode_->setEnabled(!busy && current_file_ >= 0);
  act_cancel_->setEnabled(busy);
  progress_->setVisible(busy);
}

// ---- Placeholder progression --------------------------------------------

void PreviewDemoWindow::OnPlaceholderOpenClicked() {
  const QString path =
      QFileDialog::getOpenFileName(this, tr("Open .jxl file"), QString(),
                                   tr("JPEG XL files (*.jxl);;All files (*)"));
  if (path.isEmpty()) return;
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    QMessageBox::warning(this, tr("Open"), tr("Could not open %1").arg(path));
    return;
  }
  ph_path_ = path;
  ph_bytes_ = f.readAll();
  ph_path_label_->setText(QFileInfo(path).fileName());
  ph_path_label_->setToolTip(path);
  ph_run_button_->setEnabled(true);
  ph_image_view_->Clear(
      tr("Press Run to decode 8 \u2192 4 \u2192 2 \u2192 1 into one shared "
         "dst buffer."));
  ph_log_->clear();
  ph_status_->setText(tr("Loaded %1.").arg(path));
}

void PreviewDemoWindow::OnPlaceholderRunClicked() {
  if (ph_bytes_.isEmpty()) return;
  // The progressive worker controls its own dst buffer / backend mask /
  // sizing -- this tab deliberately does NOT consult the Single-decode
  // controls, so the demo always exhibits the canonical placeholder
  // pattern from examples/decode_preview.cc.
  DecodeWorker::Request req;
  req.jxl = ph_bytes_;
  // Use a small worker pool so the placeholder is also a multi-thread
  // demo. Adjust here if you want to test single-thread behaviour.
  req.num_threads = 4;
  ph_log_->clear();
  ph_image_view_->Clear(tr("Decoding..."));
  ph_status_->setText(tr("Running progression..."));
  ph_open_button_->setEnabled(false);
  ph_run_button_->setEnabled(false);
  ph_cancel_button_->setEnabled(true);
  worker_->ResetCancel();
  QMetaObject::invokeMethod(worker_, "DecodeProgressive", Qt::QueuedConnection,
                            Q_ARG(jpegxl::tools::DecodeWorker::Request, req));
}

void PreviewDemoWindow::OnPlaceholderCancelClicked() {
  worker_->RequestCancel();
  ph_status_->setText(tr("Cancel requested..."));
}

void PreviewDemoWindow::OnPlaceholderStep(int step_index, int total_steps,
                                          int factor,
                                          const DecodeWorker::Result& result) {
  QString line;
  if (result.status == JXL_PREVIEW_SUCCESS) {
    line = QStringLiteral("[%1/%2] factor=%3  %4x%5  backend=%6  %7 ms\n")
               .arg(step_index + 1)
               .arg(total_steps)
               .arg(factor)
               .arg(result.xsize)
               .arg(result.ysize)
               .arg(BackendName(result.backend))
               .arg(result.elapsed_ms);
    ph_image_view_->SetImage(result.display_image);
  } else {
    line = QStringLiteral("[%1/%2] factor=%3  status=%4  %5 ms (skipped)\n")
               .arg(step_index + 1)
               .arg(total_steps)
               .arg(factor)
               .arg(StatusName(result.status))
               .arg(result.elapsed_ms);
  }
  ph_log_->moveCursor(QTextCursor::End);
  ph_log_->insertPlainText(line);
}

void PreviewDemoWindow::OnPlaceholderDone() {
  ph_status_->setText(tr("Progression complete."));
  ph_open_button_->setEnabled(true);
  ph_run_button_->setEnabled(!ph_bytes_.isEmpty());
  ph_cancel_button_->setEnabled(false);
}

// ---- Gallery ------------------------------------------------------------

void PreviewDemoWindow::OnGalleryOpenFolderClicked() {
  const QString dir = QFileDialog::getExistingDirectory(
      this, tr("Pick a folder of .jxl files"), gal_dir_);
  if (dir.isEmpty()) return;
  RunGalleryBatch(dir);
}

void PreviewDemoWindow::RunGalleryBatch(const QString& dir) {
  if (gal_busy_) {
    // Don't queue overlapping batches; the user must Cancel first.
    return;
  }
  QDir d(dir);
  const QStringList names =
      d.entryList(QStringList() << "*.jxl", QDir::Files, QDir::Name);
  if (names.isEmpty()) {
    gal_status_->setText(tr("No .jxl files found in %1").arg(dir));
    return;
  }
  gal_dir_ = dir;
  gal_grid_->clear();
  gal_paths_.clear();
  gal_results_.clear();
  gal_results_.resize(names.size());
  for (const QString& n : names) {
    const QString full = d.filePath(n);
    gal_paths_ << full;
    auto* item = new QListWidgetItem(n);
    item->setToolTip(full);
    item->setIcon(style()->standardIcon(QStyle::SP_FileIcon));
    gal_grid_->addItem(item);
  }
  gal_info_->clear();

  // Build a self-contained Request: the gallery tab does not consult
  // any controls outside itself.
  DecodeWorker::Request tmpl;
  const int sz = gal_thumb_size_spin_->value();
  tmpl.target_xsize = static_cast<uint32_t>(sz);
  tmpl.target_ysize = static_cast<uint32_t>(sz);
  tmpl.num_threads = gal_threads_spin_->value();
  tmpl.allowed_backends = 0;  // 0 = all backends allowed.

  worker_->ResetCancel();
  gal_busy_ = true;
  gal_batch_timer_.start();
  gal_open_folder_button_->setEnabled(false);
  gal_redecode_button_->setEnabled(false);
  gal_cancel_button_->setEnabled(true);
  gal_progress_->setRange(0, gal_paths_.size());
  gal_progress_->setValue(0);
  gal_progress_->setVisible(true);
  gal_status_->setText(tr("Decoding %1 file(s) from %2 (%3 threads, "
                          "shared runner)...")
                           .arg(gal_paths_.size())
                           .arg(QDir::toNativeSeparators(dir))
                           .arg(tmpl.num_threads));
  QMetaObject::invokeMethod(worker_, "DecodeGallery", Qt::QueuedConnection,
                            Q_ARG(QStringList, gal_paths_),
                            Q_ARG(jpegxl::tools::DecodeWorker::Request, tmpl));
}

void PreviewDemoWindow::OnGalleryCancelClicked() {
  worker_->RequestCancel();
  gal_status_->setText(tr("Cancel requested..."));
}

void PreviewDemoWindow::OnGalleryThumb(int index, int total,
                                       const QString& /*path*/,
                                       const DecodeWorker::Result& result) {
  if (index < 0 || index >= gal_grid_->count()) return;
  if (index < static_cast<int>(gal_results_.size())) {
    gal_results_[index] = result;
  }
  QListWidgetItem* item = gal_grid_->item(index);
  if (result.status == JXL_PREVIEW_SUCCESS && !result.display_image.isNull()) {
    item->setIcon(QIcon(QPixmap::fromImage(result.display_image)));
  } else {
    item->setIcon(style()->standardIcon(QStyle::SP_MessageBoxWarning));
  }
  gal_progress_->setValue(index + 1);
  gal_status_->setText(tr("Decoded %1 of %2...").arg(index + 1).arg(total));
  // If this is the currently-selected item, refresh the info panel too.
  if (gal_grid_->currentRow() == index) OnGallerySelectionChanged();
}

void PreviewDemoWindow::OnGalleryDone(int succeeded, int failed) {
  gal_busy_ = false;
  gal_open_folder_button_->setEnabled(true);
  gal_redecode_button_->setEnabled(!gal_dir_.isEmpty());
  gal_cancel_button_->setEnabled(false);
  gal_progress_->setVisible(false);
  const qint64 wall_ms = gal_batch_timer_.elapsed();
  qint64 sum_ms = 0;
  for (const auto& r : gal_results_) sum_ms += r.elapsed_ms;
  // Wall-clock = how long the batch took end-to-end (the user-visible
  // number). Sum-of-decodes = total CPU-side decode time across files,
  // useful for showing how much the shared parallel runner overlapped.
  gal_status_->setText(tr("Batch complete: %1 of %2 succeeded\u2003\u2022\u2003"
                          "%3 ms wall \u00b7 %4 ms summed")
                           .arg(succeeded)
                           .arg(succeeded + failed)
                           .arg(wall_ms)
                           .arg(sum_ms));
}

void PreviewDemoWindow::OnGalleryItemActivated() {
  // Promote the activated gallery item to the single-decode tab so the
  // user can inspect it with the full set of controls.
  int row = gal_grid_->currentRow();
  if (row < 0 || row >= gal_paths_.size()) return;
  OpenFiles(QStringList() << gal_paths_[row]);
  mode_tabs_->setCurrentIndex(0);
}

void PreviewDemoWindow::OnGallerySelectionChanged() {
  const int row = gal_grid_->currentRow();
  if (row < 0 || row >= static_cast<int>(gal_results_.size())) {
    gal_info_->clear();
    return;
  }
  const auto& r = gal_results_[row];
  // Before this item's decode has completed, the Result is default-
  // constructed (elapsed_ms == 0, status == INTERNAL_ERROR).  Show a
  // placeholder rather than the misleading default values.
  if (r.elapsed_ms == 0 && r.xsize == 0) {
    gal_info_->setPlainText(tr("(pending — decode not yet complete)"));
    return;
  }
  QString s;
  s += QStringLiteral("file              : %1\n").arg(gal_paths_[row]);
  s += QStringLiteral("status            : %1\n").arg(StatusName(r.status));
  if (r.status == JXL_PREVIEW_SUCCESS) {
    s += QStringLiteral("decode factor     : %1 -> %2 x %3\n")
             .arg(r.decode_factor)
             .arg(r.xsize)
             .arg(r.ysize);
    s += QStringLiteral("backend used      : %1\n").arg(BackendName(r.backend));
    s += QStringLiteral("output format     : %1 (%2-ch, %3)\n")
             .arg(DataTypeName(r.format.data_type))
             .arg(r.format.num_channels)
             .arg(EndiannessName(r.format.endianness));
    s += QStringLiteral("row stride        : %1 bytes\n").arg(r.stride);
    s += QStringLiteral("elapsed           : %1 ms\n").arg(r.elapsed_ms);
  } else if (!r.error_text.isEmpty()) {
    s += QStringLiteral("error             : %1\n").arg(r.error_text);
  }
  gal_info_->setPlainText(s);
}

// ---- Formatting helpers -------------------------------------------------

QString PreviewDemoWindow::FormatPreviewInfo(const JxlPreviewInfo& info,
                                             bool icc_present,
                                             size_t icc_size) {
  QString s;
  s += QString("xsize/ysize       : %1 x %2\n").arg(info.xsize).arg(info.ysize);
  s += QString("color channels    : %1\n").arg(info.num_color_channels);
  s += QString("alpha             : %1\n").arg(info.has_alpha ? "yes" : "no");
  s += QString("animated          : %1\n").arg(info.is_animated ? "yes" : "no");
  s += QString("intensity target  : %1 nits%2\n")
           .arg(info.intensity_target, 0, 'f', 0)
           .arg(info.intensity_target > 255.0f ? "  (HDR)" : "");
  s += QString("recommended factor: %1 -> %2 x %3\n")
           .arg(info.recommended_factor)
           .arg(info.recommended_xsize)
           .arg(info.recommended_ysize);
  s += QString("ICC profile       : %1\n")
           .arg(icc_present ? QString("%1 bytes").arg(icc_size)
                            : QString("(none)"));
  return s;
}

namespace {

// A short description of an output color encoding.
QString ColorEncodingText(const JxlColorEncoding& ce) {
  if (ce.color_space == JXL_COLOR_SPACE_UNKNOWN) {
    return QStringLiteral("ICC profile only (see JxlGetPreviewInfo)");
  }
  const char* space = ce.color_space == JXL_COLOR_SPACE_RGB    ? "RGB"
                      : ce.color_space == JXL_COLOR_SPACE_GRAY ? "Gray"
                      : ce.color_space == JXL_COLOR_SPACE_XYB  ? "XYB"
                                                               : "?";
  QString transfer;
  switch (ce.transfer_function) {
    case JXL_TRANSFER_FUNCTION_SRGB:
      transfer = QStringLiteral("sRGB");
      break;
    case JXL_TRANSFER_FUNCTION_LINEAR:
      transfer = QStringLiteral("linear");
      break;
    case JXL_TRANSFER_FUNCTION_709:
      transfer = QStringLiteral("BT.709");
      break;
    case JXL_TRANSFER_FUNCTION_PQ:
      transfer = QStringLiteral("PQ");
      break;
    case JXL_TRANSFER_FUNCTION_HLG:
      transfer = QStringLiteral("HLG");
      break;
    case JXL_TRANSFER_FUNCTION_DCI:
      transfer = QStringLiteral("DCI");
      break;
    case JXL_TRANSFER_FUNCTION_GAMMA:
      transfer = QStringLiteral("gamma %1").arg(ce.gamma);
      break;
    default:
      transfer = QStringLiteral("?");
      break;
  }
  return QStringLiteral("%1, transfer %2").arg(QLatin1String(space), transfer);
}

}  // namespace

QString PreviewDemoWindow::FormatResult(const DecodeWorker::Result& result) {
  QString s;
  s += QString("status            : %1\n").arg(StatusName(result.status));
  if (!result.error_text.isEmpty()) {
    s += QString("error             : %1\n").arg(result.error_text);
  }
  s += QString("backend used      : %1\n").arg(BackendName(result.backend));
  s += QString("decode factor     : %1 -> %2 x %3\n")
           .arg(result.decode_factor)
           .arg(result.xsize)
           .arg(result.ysize);
  s += QString("output format     : %1 (%2-ch, %3)\n")
           .arg(DataTypeName(result.format.data_type))
           .arg(result.format.num_channels)
           .arg(EndiannessName(result.format.endianness));
  if (result.has_color_encoding) {
    s += QString("output color      : %1\n")
             .arg(ColorEncodingText(result.color_encoding));
  }
  s += QString("row stride        : %1 bytes\n").arg(result.stride);
  s += QString("pixel buffer      : %1 bytes\n").arg(result.raw_pixel_bytes);
  s += QString("buffer source     : %1\n")
           .arg(result.used_caller_buffer
                    ? QString("caller (capacity %1)").arg(result.dst_capacity)
                    : QString("library-allocated"));
  s += QString("elapsed           : %1 ms\n").arg(result.elapsed_ms);
  if (result.stats_valid) {
    s += "\n--- counting memory manager ---\n";
    s += QString("alloc / free count : %1 / %2\n")
             .arg(result.mm_alloc_count)
             .arg(result.mm_free_count);
    s += QString("total bytes alloc'd: %1\n").arg(result.mm_total_bytes);
    s += QString("peak live bytes    : %1\n").arg(result.mm_peak_bytes);
  }
  return s;
}

const char* PreviewDemoWindow::EndiannessName(JxlEndianness e) {
  switch (e) {
    case JXL_NATIVE_ENDIAN:
      return "native";
    case JXL_LITTLE_ENDIAN:
      return "little";
    case JXL_BIG_ENDIAN:
      return "big";
  }
  return "?";
}
QString PreviewDemoWindow::BackendName(JxlPreviewBackend b) {
  switch (b) {
    case JXL_PREVIEW_BACKEND_NONE:
      return "none";
    case JXL_PREVIEW_BACKEND_EMBEDDED_PREVIEW:
      return "embedded preview";
    case JXL_PREVIEW_BACKEND_NATIVE_DC_ONLY:
      return "native DC-only";
    case JXL_PREVIEW_BACKEND_NATIVE_PROGRESSION_FLUSH:
      return "native progression flush";
    case JXL_PREVIEW_BACKEND_FALLBACK_DOWNSAMPLE:
      return "fallback downsample";
    case JXL_PREVIEW_BACKEND_NATIVE_REDUCED_INPUT:
      return "native reduced-input";
    case JXL_PREVIEW_BACKEND_NATIVE_FUSED_UPSAMPLING:
      return "native fused upsampling";
    case JXL_PREVIEW_BACKEND_DECODER_DOWNSAMPLE:
      return "decoder downsample";
  }
  return "?";
}

QString PreviewDemoWindow::StatusName(JxlPreviewStatus s) {
  switch (s) {
    case JXL_PREVIEW_SUCCESS:
      return "SUCCESS";
    case JXL_PREVIEW_INVALID_ARGUMENT:
      return "INVALID_ARGUMENT";
    case JXL_PREVIEW_CORRUPT_INPUT:
      return "CORRUPT_INPUT";
    case JXL_PREVIEW_OUT_OF_MEMORY:
      return "OUT_OF_MEMORY";
    case JXL_PREVIEW_BUFFER_TOO_SMALL:
      return "BUFFER_TOO_SMALL";
    case JXL_PREVIEW_CANCELLED:
      return "CANCELLED";
    case JXL_PREVIEW_UNSUPPORTED_FORMAT:
      return "UNSUPPORTED_FORMAT";
    case JXL_PREVIEW_NO_BACKEND_AVAILABLE:
      return "NO_BACKEND_AVAILABLE";
    case JXL_PREVIEW_INTERNAL_ERROR:
      return "INTERNAL_ERROR";
  }
  return "?";
}

const char* PreviewDemoWindow::DataTypeName(JxlDataType t) {
  switch (t) {
    case JXL_TYPE_UINT8:
      return "UINT8";
    case JXL_TYPE_UINT16:
      return "UINT16";
    case JXL_TYPE_FLOAT16:
      return "FLOAT16";
    case JXL_TYPE_FLOAT:
      return "FLOAT";
  }
  return "?";
}

}  // namespace tools
}  // namespace jpegxl
