// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#ifndef TOOLS_PREVIEW_BENCHMARK_PREVIEW_DEMO_WINDOW_H_
#define TOOLS_PREVIEW_BENCHMARK_PREVIEW_DEMO_WINDOW_H_

#include <QByteArray>
#include <QElapsedTimer>
#include <QImage>
#include <QMainWindow>
#include <QMetaType>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QString>
#include <QStringList>
#include <QThread>
#include <atomic>
#include <cstdint>
#include <vector>

#include "lib/extras/preview.h"
#include "lib/jxl/dec_preview_internal.h"

class QAction;
class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTabWidget;

namespace jpegxl {
namespace tools {

// Worker object that runs JxlGeneratePreview on a background thread so the
// UI stays responsive and the cancel flag has somewhere to be polled from.
//
// Typical Qt threading idiom: this QObject is moved to a QThread; UI
// invokes Decode() through a queued signal; the worker emits Finished()
// when done. The decode is configured by a snapshot struct so the worker
// holds no pointers back into UI state.
class DecodeWorker : public QObject {
  Q_OBJECT
 public:
  // Snapshot of every JxlPreviewOptions field plus per-call extras the
  // demo wants to exercise. Captured on the UI side before being shipped
  // across the queued signal so the worker holds no live pointers back into
  // UI state.
  struct Request {
    QByteArray jxl;  // The encoded codestream.

    // Sizing.
    uint32_t preview_downsampling = 0;
    uint32_t target_xsize = 0;
    uint32_t target_ysize = 0;

    // Format preference.
    uint32_t requested_num_channels = 0;  // 0 = library default (RGBA8).
    JxlDataType data_type = JXL_TYPE_UINT8;
    JxlEndianness endianness = JXL_NATIVE_ENDIAN;
    size_t format_align = 0;  // Row alignment in bytes.

    // Caller-supplied output buffer.
    bool use_caller_buffer = false;  // Pass dst / dst_size.
    size_t dst_stride_override = 0;  // 0 = let library tightly pack.
    int dst_overprovision = 0;       // Extra bytes added to dst_size.
    bool dst_undersize = false;      // Force BUFFER_TOO_SMALL on purpose.

    // Color management.
    bool use_color_encoding = true;  // false -> library default (sRGB).
    int color_preset = 0;            // 0..3 -- see MakeColorEncoding.
    int nits_mode = 0;               // 0=auto, 1=no-tonemap, 2=explicit.
    float display_nits = 100.0f;     // Used when nits_mode == 2.

    // Resources.
    int num_threads = 0;              // 0 -> single-threaded, no runner.
    bool use_memory_manager = false;  // Counting allocator.

    // Backends.
    uint32_t allowed_backends = 0;  // 0 = all allowed.
  };

  // Aggregated outcome of one decode -- everything the GUI panel renders.
  struct Result {
    JxlPreviewStatus status = JXL_PREVIEW_INTERNAL_ERROR;
    QString error_text;

    // Output image (only valid on success).
    uint32_t xsize = 0;
    uint32_t ysize = 0;
    size_t stride = 0;
    JxlPixelFormat format{};
    JxlPreviewBackend backend = JXL_PREVIEW_BACKEND_NONE;
    std::vector<uint8_t> raw_bytes;  // Raw decoded bytes (any data_type).
    size_t raw_pixel_bytes = 0;      // Decoder-reported buffer size.
    QImage display_image;            // Always RGBA8, ready for QPixmap.

    // Stats from the counting allocator (when use_memory_manager).
    bool stats_valid = false;
    qulonglong mm_alloc_count = 0;
    qulonglong mm_free_count = 0;
    qulonglong mm_total_bytes = 0;
    qulonglong mm_peak_bytes = 0;

    qint64 elapsed_ms = 0;
    bool used_caller_buffer = false;  // Echo of req.use_caller_buffer.
    size_t dst_capacity = 0;          // Bytes actually offered to the API.
    uint32_t decode_factor = 0;       // Downsampling factor used (1/2/4/8).
    // Color encoding of the output (out_color_encoding); UNKNOWN = ICC only.
    bool has_color_encoding = false;
    JxlColorEncoding color_encoding{};
  };

  explicit DecodeWorker(QObject* parent = nullptr);

  // UI -> worker. The UI calls RequestCancel() from any thread; the worker
  // reads it from inside the C decoder via cancel_flag(). The flag is
  // std::atomic<int> for portable cross-thread visibility; we hand the C
  // API its address as a plain int*, which is sound because
  // std::atomic<int> is required to be lock-free with the same
  // representation as int on every supported platform.
  int* cancel_flag() { return reinterpret_cast<int*>(&cancel_); }
  void RequestCancel() { cancel_.store(1, std::memory_order_relaxed); }
  void ResetCancel() { cancel_.store(0, std::memory_order_relaxed); }

 public slots:
  // Fully-qualified types so Qt's moc registration matches the metatype.
  void Decode(const jpegxl::tools::DecodeWorker::Request& req);

  // "placeholder" pattern from examples/decode_preview.cc: progressive
  // 8 -> 4 -> 2 -> 1 decodes that all share one caller-allocated dst
  // buffer (size = worst case at factor 1) and forbid the fallback
  // downsample backend so each step actually proves a native one.
  void DecodeProgressive(const jpegxl::tools::DecodeWorker::Request& req);

  // "gallery" pattern from examples/decode_preview.cc: a batch of files
  // decoded one after another with a single shared
  // JxlResizableParallelRunner and a single shared atomic cancel flag,
  // emitting one signal per file as a thumbnail Result.
  void DecodeGallery(const QStringList& paths,
                     const jpegxl::tools::DecodeWorker::Request& tmpl);

 signals:
  void Finished(const jpegxl::tools::DecodeWorker::Result& result);

  // Per-step progressive emission (step_index in [0, total_steps)).
  void ProgressiveStep(int step_index, int total_steps, int factor,
                       const jpegxl::tools::DecodeWorker::Result& result);
  void ProgressiveDone();

  // Per-image gallery emission. "path" is the absolute path of the file
  // that produced this thumbnail.
  void GalleryThumb(int index, int total, const QString& path,
                    const jpegxl::tools::DecodeWorker::Result& result);
  void GalleryDone(int succeeded, int failed);

 private:
  std::atomic<int> cancel_{0};
};

// Image viewer with fit-to-window and explicit-zoom modes. Keeps the
// original QImage so re-scaling on resize stays loss-free.
class PreviewImageView : public QScrollArea {
  Q_OBJECT
 public:
  explicit PreviewImageView(QWidget* parent = nullptr);
  void SetImage(const QImage& image);
  void Clear(const QString& placeholder);

 public slots:
  void SetFitToWindow(bool fit);
  void ZoomIn();
  void ZoomOut();
  void ZoomReset();

 signals:
  // Emitted whenever the fit-to-window state changes, including as a
  // side effect of ZoomIn/ZoomOut/ZoomReset turning fit off. Lets the
  // toolbar's checkable 'Fit' action stay in sync with the actual mode.
  void FitChanged(bool fit);

 protected:
  void resizeEvent(QResizeEvent* e) override;

 private:
  void Rerender();

  QLabel* label_ = nullptr;
  QImage image_;
  bool fit_ = true;
  double zoom_ = 1.0;
};

// Main window: file list, tabbed option controls, image preview, info panes.
class PreviewDemoWindow : public QMainWindow {
  Q_OBJECT
 public:
  explicit PreviewDemoWindow(QWidget* parent = nullptr);
  ~PreviewDemoWindow() override;

  // Add command-line files. Also called by the drag-and-drop handler.
  void OpenFiles(const QStringList& paths);

 protected:
  void dragEnterEvent(QDragEnterEvent* e) override;
  void dropEvent(QDropEvent* e) override;

 private slots:
  void OnOpenClicked();
  void OnRemoveSelected();
  void OnFileSelectionChanged();
  void OnProbeClicked();
  void OnSaveIccClicked();
  void OnDecodeClicked();
  void OnCancelClicked();
  void OnSaveImageClicked();
  void OnDecodeFinished(const DecodeWorker::Result& result);
  // Live-recompute on any control change when 'auto-decode' is on.
  void OnAutoChanged();

  // "Placeholder progression" tab (self-contained: own file picker).
  void OnPlaceholderOpenClicked();
  void OnPlaceholderRunClicked();
  void OnPlaceholderCancelClicked();
  void OnPlaceholderStep(int step_index, int total_steps, int factor,
                         const DecodeWorker::Result& result);
  void OnPlaceholderDone();

  // "Gallery" tab (self-contained: own folder picker).
  void OnGalleryOpenFolderClicked();
  void OnGalleryCancelClicked();
  void OnGallerySelectionChanged();
  void OnGalleryThumb(int index, int total, const QString& path,
                      const DecodeWorker::Result& result);
  void OnGalleryDone(int succeeded, int failed);
  void OnGalleryItemActivated();

 private:
  // Build a Request from current UI state. Returns false if no file loaded.
  bool BuildRequest(DecodeWorker::Request* req);

  void SetBusy(bool busy);
  void TriggerAutoDecode();

  // Enumerate `dir` for *.jxl files and dispatch a batch decode. Used by
  // the Gallery 'Open folder...' button, the Redecode button, and
  // drag-and-drop.
  void RunGalleryBatch(const QString& dir);

  // Tabbed-controls factories. Each returns a fresh QWidget owned by the
  // tab widget; member pointers are populated as a side effect.
  QWidget* BuildSingleDecodeTab();
  QWidget* BuildSizingTab();
  QWidget* BuildFormatTab();
  QWidget* BuildColorTab();
  QWidget* BuildBackendTab();
  QWidget* BuildResourcesTab();
  QWidget* BuildPlaceholderTab();
  QWidget* BuildGalleryTab();

  // Build a Request from the current single-decode controls but using an
  // explicit codestream (so it can be reused for placeholder/gallery).
  DecodeWorker::Request BuildRequestForBytes(const QByteArray& bytes);

  void RefreshControlsForFile();

  static QString FormatPreviewInfo(const JxlPreviewInfo& info, bool icc_present,
                                   size_t icc_size);
  static QString FormatResult(const DecodeWorker::Result& result);
  static QString BackendName(JxlPreviewBackend b);
  static QString StatusName(JxlPreviewStatus s);
  static const char* DataTypeName(JxlDataType t);
  static const char* EndiannessName(JxlEndianness e);

  // --- File state ---
  struct LoadedFile {
    QString path;
    QByteArray bytes;
  };
  std::vector<LoadedFile> files_;
  int current_file_ = -1;

  // ICC retained from the last probe so the "Save ICC" action can write it.
  QByteArray last_icc_;
  // Last decoded image (for "Save image").
  QImage last_image_;

  // --- Worker thread ---
  QThread worker_thread_;
  DecodeWorker* worker_ = nullptr;  // Lives in worker_thread_.

  // --- Toolbar actions ---
  QAction* act_open_ = nullptr;
  QAction* act_remove_ = nullptr;
  QAction* act_probe_ = nullptr;
  QAction* act_save_icc_ = nullptr;
  QAction* act_decode_ = nullptr;
  QAction* act_cancel_ = nullptr;
  QAction* act_save_image_ = nullptr;
  QAction* act_zoom_in_ = nullptr;
  QAction* act_zoom_out_ = nullptr;
  QAction* act_zoom_reset_ = nullptr;
  QAction* act_zoom_fit_ = nullptr;

  // --- File list ---
  QListWidget* file_list_ = nullptr;

  // --- Sizing tab ---
  QComboBox* sizing_mode_ = nullptr;  // auto-from-target / explicit factor.
  QSpinBox* downsample_spin_ = nullptr;
  QSpinBox* target_w_spin_ = nullptr;
  QSpinBox* target_h_spin_ = nullptr;

  // --- Format tab ---
  QComboBox* data_type_combo_ = nullptr;
  QSpinBox* num_channels_spin_ = nullptr;
  QComboBox* endian_combo_ = nullptr;
  QSpinBox* align_spin_ = nullptr;
  QCheckBox* dst_check_ = nullptr;
  QSpinBox* dst_stride_spin_ = nullptr;
  QSpinBox* dst_overprovision_spin_ = nullptr;
  QCheckBox* dst_undersize_check_ = nullptr;

  // --- Color tab ---
  QCheckBox* color_enable_check_ = nullptr;
  QComboBox* color_combo_ = nullptr;
  QComboBox* nits_mode_combo_ = nullptr;  // auto / no-tone / explicit.
  QSpinBox* nits_spin_ = nullptr;

  // --- Backend mask tab ---
  QCheckBox* backend_full_decode_ = nullptr;
  QCheckBox* backend_embedded_ = nullptr;
  QCheckBox* backend_dc_ = nullptr;
  QCheckBox* backend_progression_flush_ = nullptr;
  QCheckBox* backend_modular_ = nullptr;
  QCheckBox* backend_fallback_ = nullptr;
  QPushButton* backend_all_ = nullptr;
  QPushButton* backend_none_ = nullptr;

  // --- Resources tab ---
  QSpinBox* threads_spin_ = nullptr;
  QCheckBox* mm_check_ = nullptr;

  // Auto-decode lives on the Single-decode toolbar (workflow toggle, not
  // a resource setting) so it's always next to the Decode button.
  QCheckBox* auto_decode_check_ = nullptr;

  // --- Top-level mode tabs ---
  QTabWidget* mode_tabs_ = nullptr;

  // --- Single-decode tab ---
  QTabWidget* control_tabs_ = nullptr;  // option groups inside Single tab.
  PreviewImageView* image_view_ = nullptr;
  QPlainTextEdit* probe_view_ = nullptr;
  QPlainTextEdit* result_view_ = nullptr;
  QLabel* single_status_ = nullptr;   // per-tab operational status.
  QProgressBar* progress_ = nullptr;  // indeterminate spinner during decode.

  // --- Placeholder tab ---
  // The placeholder pattern decodes one file from start to finish, so it
  // owns its own input rather than borrowing from the Single-decode list.
  QPushButton* ph_open_button_ = nullptr;
  QPushButton* ph_run_button_ = nullptr;
  QPushButton* ph_cancel_button_ = nullptr;
  QLabel* ph_path_label_ = nullptr;
  PreviewImageView* ph_image_view_ = nullptr;
  QPlainTextEdit* ph_log_ = nullptr;
  QLabel* ph_status_ = nullptr;
  QString ph_path_;  // currently loaded file.
  QByteArray ph_bytes_;

  // --- Gallery tab ---
  QPushButton* gal_open_folder_button_ = nullptr;
  QPushButton* gal_redecode_button_ = nullptr;
  QPushButton* gal_cancel_button_ = nullptr;
  QSpinBox* gal_thumb_size_spin_ = nullptr;
  QSpinBox* gal_threads_spin_ = nullptr;
  QListWidget* gal_grid_ = nullptr;
  QProgressBar* gal_progress_ = nullptr;
  QLabel* gal_status_ = nullptr;
  QPlainTextEdit* gal_info_ = nullptr;  // selected-thumbnail details.
  QString gal_dir_;                     // remembered folder for re-decode.
  QStringList gal_paths_;               // paths matching gal_grid_ rows.
  std::vector<DecodeWorker::Result> gal_results_;  // matches gal_grid_ rows.
  bool gal_busy_ = false;
  QElapsedTimer gal_batch_timer_;  // wall-clock timing for batch.
};

}  // namespace tools
}  // namespace jpegxl

Q_DECLARE_METATYPE(jpegxl::tools::DecodeWorker::Request)
Q_DECLARE_METATYPE(jpegxl::tools::DecodeWorker::Result)

#endif  // TOOLS_PREVIEW_BENCHMARK_PREVIEW_DEMO_WINDOW_H_
