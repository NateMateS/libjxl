// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "tools/preview_benchmark/preview_benchmark_window.h"

#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QComboBox>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <cmath>

namespace jpegxl {
namespace tools {

namespace {

QString HumanBytesStr(uint64_t bytes) {
  const char* kUnits[] = {"B", "KiB", "MiB", "GiB"};
  double value = static_cast<double>(bytes);
  int unit = 0;
  while (value >= 1024.0 && unit + 1 < 4) {
    value /= 1024.0;
    ++unit;
  }
  return unit == 0 ? QString::number(bytes) + " B"
                   : QString::number(value, 'f', 2) + " " + kUnits[unit];
}

QString FloatText(double value, int decimals = 2) {
  return QString::number(value, 'f', decimals);
}

QString MeanSigmaMs(const PreviewBenchStats& s) {
  return s.stddev > 0.0
             ? FloatText(s.mean) + " \u00b1 " + FloatText(s.stddev) + " ms"
             : FloatText(s.mean) + " ms";
}

QImage FrameToQImage(const jxl::extras::PackedPixelFile& ppf) {
  if (ppf.frames.empty()) return QImage();
  const jxl::extras::PackedImage& image = ppf.frames.front().color;
  if (image.xsize == 0 || image.ysize == 0) return QImage();

  QImage out(static_cast<int>(image.xsize), static_cast<int>(image.ysize),
             QImage::Format_RGBA8888);
  const auto to_u8 = [](float value) {
    const double rounded = std::round(static_cast<double>(value) * 255.0);
    return static_cast<uchar>(std::clamp(rounded, 0.0, 255.0));
  };

  for (int y = 0; y < out.height(); ++y) {
    uchar* row = out.scanLine(y);
    for (int x = 0; x < out.width(); ++x) {
      const size_t channels = image.format.num_channels;
      float r = 0.0f;
      float g = 0.0f;
      float b = 0.0f;
      float a = 1.0f;
      if (channels == 1) {
        r = g = b = image.GetPixelValue(y, x, 0);
      } else if (channels == 2) {
        r = g = b = image.GetPixelValue(y, x, 0);
        a = image.GetPixelValue(y, x, 1);
      } else if (channels == 3) {
        r = image.GetPixelValue(y, x, 0);
        g = image.GetPixelValue(y, x, 1);
        b = image.GetPixelValue(y, x, 2);
      } else {
        r = image.GetPixelValue(y, x, 0);
        g = image.GetPixelValue(y, x, 1);
        b = image.GetPixelValue(y, x, 2);
        a = image.GetPixelValue(y, x, 3);
      }
      row[4 * x + 0] = to_u8(r);
      row[4 * x + 1] = to_u8(g);
      row[4 * x + 2] = to_u8(b);
      row[4 * x + 3] = to_u8(a);
    }
  }
  return out;
}

QTableWidgetItem* NumItem(const QString& text) {
  auto* item = new QTableWidgetItem(text);
  item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
  return item;
}

// reduction_pct > 0 means preview is better.
QTableWidgetItem* DeltaItem(double reduction_pct) {
  if (std::abs(reduction_pct) < 0.05) {
    auto* item = new QTableWidgetItem("—");
    item->setTextAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    return item;
  }
  const QString text = (reduction_pct > 0 ? "↓ " : "↑ ") +
                       FloatText(std::abs(reduction_pct), 1) + " %";
  auto* item = new QTableWidgetItem(text);
  item->setTextAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
  item->setForeground(reduction_pct > 0 ? QColor(0, 130, 0)
                                        : QColor(180, 0, 0));
  return item;
}

void SetMetric(QTableWidget* table, int row, const QString& label,
               const QString& full, const QString& preview,
               QTableWidgetItem* delta = nullptr) {
  table->setItem(row, 0, new QTableWidgetItem(label));
  table->setItem(row, 1, NumItem(full));
  table->setItem(row, 2, NumItem(preview));
  table->setItem(row, 3, delta != nullptr ? delta : new QTableWidgetItem());
}

void SetMetricWithDelta(QTableWidget* table, int row, const QString& label,
                        const QString& full, const QString& preview,
                        double reduction_pct) {
  SetMetric(table, row, label, full, preview, DeltaItem(reduction_pct));
}

}  // namespace

PreviewBenchmarkWindow::PreviewBenchmarkWindow(QWidget* parent)
    : QMainWindow(parent),
      inputs_(new QListWidget(this)),
      preview_downsampling_(new QComboBox(this)),
      iterations_(new QSpinBox(this)),
      threads_(new QSpinBox(this)),
      export_csv_(new QPushButton(tr("Export CSV"), this)),
      save_previews_(new QPushButton(tr("Save preview images\u2026"), this)),
      run_current_(new QPushButton(tr("Run Current"), this)),
      run_batch_(new QPushButton(tr("Run Batch"), this)),
      split_view_(new SplitImageView(this)),
      metrics_table_(new QTableWidget(this)),
      batch_table_(new QTableWidget(this)) {
  setWindowTitle(tr("JPEG XL Preview Benchmark"));
  resize(1600, 950);

  QWidget* central = new QWidget(this);
  QVBoxLayout* root = new QVBoxLayout(central);

  QHBoxLayout* controls = new QHBoxLayout();
  QPushButton* open_files = new QPushButton(tr("Open Files"), this);
  QPushButton* open_directory = new QPushButton(tr("Open Directory"), this);

  preview_downsampling_->addItem("1/2", 2);
  preview_downsampling_->addItem("1/4", 4);
  preview_downsampling_->addItem("1/8", 8);
  preview_downsampling_->setCurrentIndex(1);

  iterations_->setRange(1, 100);
  iterations_->setValue(5);
  iterations_->setFocusPolicy(Qt::StrongFocus);
  threads_->setRange(0, 128);
  threads_->setSpecialValueText(tr("Auto"));
  threads_->setValue(0);
  threads_->setFocusPolicy(Qt::StrongFocus);

  controls->addWidget(open_files);
  controls->addWidget(open_directory);
  controls->addSpacing(8);
  controls->addWidget(run_current_);
  controls->addWidget(run_batch_);
  controls->addSpacing(16);
  controls->addWidget(new QLabel(tr("Preview"), this));
  controls->addWidget(preview_downsampling_);
  controls->addWidget(new QLabel(tr("Reps"), this));
  controls->addWidget(iterations_);
  controls->addWidget(new QLabel(tr("Threads"), this));
  controls->addWidget(threads_);
  {
    // Decided once, from whether preview_bench_worker can be run now.
    std::string worker_error;
    const bool has_worker = PreviewBenchWorkerAvailable(&worker_error);
    measurement_ = has_worker ? PreviewBenchMeasurement::kWorker
                              : PreviewBenchMeasurement::kInProcess;
    QLabel* worker_label = new QLabel(
        has_worker ? tr("Workers: \u2713") : tr("Workers: \u2717"), this);
    QPalette pal = worker_label->palette();
    pal.setColor(QPalette::WindowText,
                 has_worker ? QColor(0, 130, 0) : QColor(160, 100, 0));
    worker_label->setPalette(pal);
    worker_label->setToolTip(
        has_worker
            ? tr("Each decode runs in a preview_bench_worker process: timing "
                 "is isolated, and the process memory peaks are the worker's "
                 "high-water marks.")
            : tr("preview_bench_worker cannot be run (%1): decodes run in "
                 "this process, and process memory peaks are not measured.")
                  .arg(QString::fromStdString(worker_error)));
    controls->addSpacing(8);
    controls->addWidget(worker_label);
  }
  controls->addStretch();
  controls->addWidget(save_previews_);
  controls->addWidget(export_csv_);
  root->addLayout(controls);

  QSplitter* splitter = new QSplitter(this);
  inputs_->setSelectionMode(QAbstractItemView::SingleSelection);
  splitter->addWidget(inputs_);

  QTabWidget* tabs = new QTabWidget(this);

  QWidget* single_tab = new QWidget(this);
  QVBoxLayout* single_layout = new QVBoxLayout(single_tab);
  QLabel* split_label = new QLabel(tr("\u2190 Preview  |  Full \u2192"), this);
  split_label->setAlignment(Qt::AlignCenter);
  {
    QFont f = split_label->font();
    f.setItalic(true);
    split_label->setFont(f);
  }
  single_layout->addWidget(split_label);
  single_layout->addWidget(split_view_, 1);
  metrics_table_->setColumnCount(4);
  metrics_table_->setHorizontalHeaderLabels(
      {tr("Metric"), tr("Full"), tr("Preview"), tr("Delta")});
  metrics_table_->horizontalHeader()->setSectionResizeMode(
      QHeaderView::ResizeToContents);
  metrics_table_->horizontalHeader()->setStretchLastSection(true);
  metrics_table_->verticalHeader()->setVisible(false);
  metrics_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  metrics_table_->setSelectionMode(QAbstractItemView::NoSelection);
  metrics_table_->setAlternatingRowColors(true);
  single_layout->addWidget(metrics_table_);
  tabs->addTab(single_tab, tr("Single Image"));

  QWidget* batch_tab = new QWidget(this);
  QVBoxLayout* batch_layout = new QVBoxLayout(batch_tab);
  batch_table_->setColumnCount(11);
  batch_table_->setHorizontalHeaderLabels(
      {tr("Image"), tr("Type"), tr("Backend"), tr("Full (ms \u00b1 \u03c3)"),
       tr("Preview (ms \u00b1 \u03c3)"), tr("Speedup"), tr("Prev Mpx/s"),
       tr("Bytes \u0394"), tr("Mem Peak \u0394"), tr("Allocs \u0394"),
       tr("RMSE")});
  batch_table_->horizontalHeader()->setSectionResizeMode(
      QHeaderView::ResizeToContents);
  batch_table_->horizontalHeader()->setStretchLastSection(true);
  batch_table_->verticalHeader()->setVisible(false);
  batch_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  batch_table_->setAlternatingRowColors(true);
  batch_layout->addWidget(batch_table_);
  tabs->addTab(batch_tab, tr("Batch Results"));

  splitter->addWidget(tabs);
  splitter->setStretchFactor(0, 0);
  splitter->setStretchFactor(1, 1);
  root->addWidget(splitter, 1);
  setCentralWidget(central);

  connect(open_files, &QPushButton::clicked, this,
          &PreviewBenchmarkWindow::OpenFiles);
  connect(open_directory, &QPushButton::clicked, this,
          &PreviewBenchmarkWindow::OpenDirectory);
  connect(run_current_, &QPushButton::clicked, this,
          &PreviewBenchmarkWindow::RunCurrent);
  connect(run_batch_, &QPushButton::clicked, this,
          &PreviewBenchmarkWindow::RunBatch);
  connect(export_csv_, &QPushButton::clicked, this,
          &PreviewBenchmarkWindow::ExportCsv);
  connect(save_previews_, &QPushButton::clicked, this,
          &PreviewBenchmarkWindow::SavePreviewImages);
  connect(inputs_, &QListWidget::currentItemChanged, this,
          &PreviewBenchmarkWindow::HandleCurrentItemChanged);
}

void PreviewBenchmarkWindow::AddEntries(const QStringList& entries) {
  for (const QString& entry : entries) {
    AddPathRecursive(entry);
  }
  input_paths_.removeDuplicates();
  std::sort(input_paths_.begin(), input_paths_.end());
  RefreshInputList();
}

void PreviewBenchmarkWindow::OpenFiles() {
  const QStringList files = QFileDialog::getOpenFileNames(
      this, tr("Open JPEG XL files"), QString(), tr("JPEG XL images (*.jxl)"));
  if (!files.isEmpty()) AddEntries(files);
}

void PreviewBenchmarkWindow::OpenDirectory() {
  const QString directory = QFileDialog::getExistingDirectory(
      this, tr("Open directory containing JPEG XL files"));
  if (!directory.isEmpty()) AddEntries({directory});
}

void PreviewBenchmarkWindow::RunCurrent() {
  const QString path = CurrentPath();
  if (path.isEmpty()) return;

  run_current_->setEnabled(false);
  run_batch_->setEnabled(false);
  statusBar()->showMessage(tr("Benchmarking\u2026"));
  QApplication::setOverrideCursor(Qt::WaitCursor);
  QApplication::processEvents();
  PreviewBenchComparison comparison;
  const PreviewBenchOptions options = CurrentOptions();
  const bool ok =
      BenchmarkJxlComparison(path.toStdString(), options, &comparison);
  QApplication::restoreOverrideCursor();
  run_current_->setEnabled(true);
  run_batch_->setEnabled(true);
  if (!ok) {
    QString details;
    for (const PreviewBenchSummary* summary :
         {&comparison.full, &comparison.preview}) {
      if (summary->error.empty()) continue;
      details +=
          tr("\n%1 decode: %2")
              .arg(QString::fromLatin1(PreviewBenchModeName(summary->mode)),
                   QString::fromStdString(summary->error));
    }
    QMessageBox::critical(
        this, tr("Benchmark failed"),
        tr("Failed to benchmark \"%1\".%2").arg(path, details));
    return;
  }

  PreviewBenchImage full_image;
  PreviewBenchImage preview_image;
  std::string error;
  if (!DecodeJxlForPreviewBenchmark(path.toStdString(), options,
                                    PreviewBenchMode::kFull, &full_image,
                                    &error) ||
      !DecodeJxlForPreviewBenchmark(path.toStdString(), options,
                                    PreviewBenchMode::kPreview, &preview_image,
                                    &error)) {
    QMessageBox::critical(this, tr("Decode failed"),
                          tr("Failed to decode \"%1\": %2")
                              .arg(path, QString::fromStdString(error)));
    return;
  }

  QImage full_qimage = FrameToQImage(full_image.ppf);
  QImage preview_qimage = FrameToQImage(preview_image.ppf);
  if (!full_qimage.isNull() && !preview_qimage.isNull() &&
      preview_qimage.size() != full_qimage.size()) {
    preview_qimage = preview_qimage.scaled(
        full_qimage.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  }
  split_view_->setLeftImage(preview_qimage);
  split_view_->setRightImage(full_qimage);
  split_view_->setMiddleImage(full_qimage);

  current_result_ = comparison;
  has_current_result_ = true;
  UpdateMetricsTable(comparison);
  statusBar()->showMessage(tr("Done \u2014 %1  \u2014  backend: %2")
                               .arg(QFileInfo(path).fileName(),
                                    QString::fromLatin1(PreviewBackendName(
                                        comparison.preview.preview_backend))));
}

void PreviewBenchmarkWindow::RunBatch() {
  batch_results_.clear();
  UpdateBatchTable();
  if (input_paths_.isEmpty()) return;

  run_current_->setEnabled(false);
  run_batch_->setEnabled(false);
  QApplication::setOverrideCursor(Qt::WaitCursor);
  const PreviewBenchOptions options = CurrentOptions();
  int failed = 0;
  QString last_error;
  for (int i = 0; i < input_paths_.size(); ++i) {
    statusBar()->showMessage(tr("Benchmarking %1 / %2: %3")
                                 .arg(i + 1)
                                 .arg(input_paths_.size())
                                 .arg(QFileInfo(input_paths_[i]).fileName()));
    QApplication::processEvents();
    PreviewBenchComparison comparison;
    if (BenchmarkJxlComparison(input_paths_[i].toStdString(), options,
                               &comparison)) {
      batch_results_.push_back(std::move(comparison));
    } else {
      ++failed;
      last_error = QString::fromStdString(comparison.full.error.empty()
                                              ? comparison.preview.error
                                              : comparison.full.error);
    }
    UpdateBatchTable();
    qApp->processEvents();
  }
  QApplication::restoreOverrideCursor();
  run_current_->setEnabled(true);
  run_batch_->setEnabled(true);
  statusBar()->showMessage(tr("Batch complete \u2014 %1 image(s) processed")
                               .arg(batch_results_.size()));
  if (failed > 0) {
    QMessageBox::warning(
        this, tr("Benchmark failed"),
        tr("%1 image(s) failed. Last error: %2").arg(failed).arg(last_error));
  }
}

void PreviewBenchmarkWindow::ExportCsv() {
  if (batch_results_.empty() && !has_current_result_) return;
  const QString path = QFileDialog::getSaveFileName(
      this, tr("Export benchmark CSV"), QString(), tr("CSV files (*.csv)"));
  if (path.isEmpty()) return;

  const std::vector<PreviewBenchComparison> to_export =
      batch_results_.empty()
          ? std::vector<PreviewBenchComparison>{current_result_}
          : batch_results_;
  std::string error;
  if (!WriteComparisonsCsv(to_export, path.toStdString(), &error)) {
    QMessageBox::critical(this, tr("Export failed"),
                          QString::fromStdString(error));
    return;
  }
  statusBar()->showMessage(tr("Exported %1").arg(path));
}

void PreviewBenchmarkWindow::SavePreviewImages() {
  if (input_paths_.isEmpty()) {
    QMessageBox::information(this, tr("No inputs"),
                             tr("Add input files first."));
    return;
  }
  const QString dir = QFileDialog::getExistingDirectory(
      this, tr("Save preview images to directory"));
  if (dir.isEmpty()) return;

  run_current_->setEnabled(false);
  run_batch_->setEnabled(false);
  save_previews_->setEnabled(false);
  QApplication::setOverrideCursor(Qt::WaitCursor);
  const PreviewBenchOptions options = CurrentOptions();
  int ok_count = 0;
  int fail_count = 0;
  for (int i = 0; i < input_paths_.size(); ++i) {
    statusBar()->showMessage(tr("Saving preview %1/%2: %3")
                                 .arg(i + 1)
                                 .arg(input_paths_.size())
                                 .arg(QFileInfo(input_paths_[i]).fileName()));
    QApplication::processEvents();
    PreviewBenchImage image;
    std::string error;
    if (!DecodeJxlForPreviewBenchmark(input_paths_[i].toStdString(), options,
                                      PreviewBenchMode::kPreview, &image,
                                      &error)) {
      ++fail_count;
      continue;
    }
    QImage qimg = FrameToQImage(image.ppf);
    if (qimg.isNull()) {
      ++fail_count;
      continue;
    }
    const QFileInfo fi(input_paths_[i]);
    const QString out_path = QDir(dir).filePath(
        fi.completeBaseName() + "_ds" +
        QString::number(options.preview_downsampling) + ".png");
    if (qimg.save(out_path, "PNG")) {
      ++ok_count;
    } else {
      ++fail_count;
    }
  }
  QApplication::restoreOverrideCursor();
  run_current_->setEnabled(true);
  run_batch_->setEnabled(true);
  save_previews_->setEnabled(true);
  statusBar()->showMessage(tr("Saved %1 preview image(s) to %2%3")
                               .arg(ok_count)
                               .arg(dir)
                               .arg(fail_count > 0
                                        ? tr(" (%1 failed)").arg(fail_count)
                                        : QString()));
}

void PreviewBenchmarkWindow::HandleCurrentItemChanged(
    QListWidgetItem* current, QListWidgetItem* previous) {
  Q_UNUSED(previous);
  if (current != nullptr) RunCurrent();
}

PreviewBenchOptions PreviewBenchmarkWindow::CurrentOptions() const {
  PreviewBenchOptions options;
  options.preview_downsampling =
      static_cast<size_t>(preview_downsampling_->currentData().toUInt());
  options.num_reps = static_cast<size_t>(iterations_->value());
  options.num_threads = static_cast<size_t>(threads_->value());
  options.measurement = measurement_;
  return options;
}

QString PreviewBenchmarkWindow::CurrentPath() const {
  const QListWidgetItem* current = inputs_->currentItem();
  return current == nullptr ? QString()
                            : current->data(Qt::UserRole).toString();
}

void PreviewBenchmarkWindow::RefreshInputList() {
  inputs_->clear();
  for (const QString& path : input_paths_) {
    QListWidgetItem* item = new QListWidgetItem(QFileInfo(path).fileName());
    item->setData(Qt::UserRole, path);
    item->setToolTip(path);
    inputs_->addItem(item);
  }
  if (inputs_->count() > 0 && inputs_->currentItem() == nullptr) {
    inputs_->setCurrentRow(0);
  }
}

void PreviewBenchmarkWindow::AddPathRecursive(const QString& entry) {
  QFileInfo info(entry);
  if (!info.exists()) return;
  if (info.isFile()) {
    if (info.suffix().compare("jxl", Qt::CaseInsensitive) == 0) {
      input_paths_.push_back(info.absoluteFilePath());
    }
    return;
  }
  QDirIterator it(entry, QStringList() << "*.jxl",
                  QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    input_paths_.push_back(it.next());
  }
}

void PreviewBenchmarkWindow::UpdateMetricsTable(
    const PreviewBenchComparison& comparison) {
  metrics_table_->clearContents();
  metrics_table_->setRowCount(18);

  SetMetric(metrics_table_, 0, tr("Output size"),
            tr("%1 \u00d7 %2")
                .arg(comparison.full.output_xsize)
                .arg(comparison.full.output_ysize),
            tr("%1 \u00d7 %2")
                .arg(comparison.preview.output_xsize)
                .arg(comparison.preview.output_ysize));
  SetMetric(
      metrics_table_, 1, tr("Image type"),
      QString::fromLatin1(FrameEncodingName(comparison.full.frame_encoding)),
      QString::fromLatin1(
          FrameEncodingName(comparison.preview.frame_encoding)));
  SetMetric(metrics_table_, 2, tr("Preview backend"), "\u2014",
            QString::fromLatin1(
                PreviewBackendName(comparison.preview.preview_backend)));
  SetMetricWithDelta(metrics_table_, 3, tr("Wall time"),
                     MeanSigmaMs(comparison.full.wall_time_ms),
                     MeanSigmaMs(comparison.preview.wall_time_ms),
                     comparison.wall_time_reduction_pct);
  SetMetricWithDelta(metrics_table_, 4, tr("CPU time"),
                     MeanSigmaMs(comparison.full.cpu_time_ms),
                     MeanSigmaMs(comparison.preview.cpu_time_ms),
                     comparison.cpu_time_reduction_pct);
  SetMetric(metrics_table_, 5, tr("Wall speedup"), "\u2014",
            FloatText(comparison.wall_time_speedup) + "×",
            DeltaItem((comparison.wall_time_speedup - 1.0) * 100.0));
  const double thr_full = comparison.full.throughput_mpx_per_s;
  const double thr_prev = comparison.preview.throughput_mpx_per_s;
  const double thr_delta_pct =
      thr_full > 0.0 ? (thr_prev - thr_full) / thr_full * 100.0 : 0.0;
  SetMetric(metrics_table_, 6, tr("Throughput (src)"),
            FloatText(thr_full) + " Mpx/s", FloatText(thr_prev) + " Mpx/s",
            DeltaItem(thr_delta_pct));
  SetMetric(metrics_table_, 7, tr("Threads"),
            QString::number(
                static_cast<qulonglong>(comparison.full.effective_num_threads)),
            QString::number(static_cast<qulonglong>(
                comparison.preview.effective_num_threads)));
  SetMetricWithDelta(metrics_table_, 8, tr("Decoded bytes"),
                     HumanBytesStr(comparison.full.decoded_bytes),
                     HumanBytesStr(comparison.preview.decoded_bytes),
                     comparison.decoded_bytes_reduction_pct);
  SetMetricWithDelta(metrics_table_, 9, tr("Decoder peak memory"),
                     HumanBytesStr(comparison.full.decoder_peak_bytes),
                     HumanBytesStr(comparison.preview.decoder_peak_bytes),
                     comparison.decoder_peak_reduction_pct);
  SetMetricWithDelta(metrics_table_, 10, tr("Decoder allocations"),
                     QString::number(static_cast<qulonglong>(
                         comparison.full.decoder_num_allocations)),
                     QString::number(static_cast<qulonglong>(
                         comparison.preview.decoder_num_allocations)),
                     comparison.decoder_num_allocations_reduction_pct);
  // Process peaks are measured only in worker processes, and private bytes
  // only on Windows.
  const auto peak_text = [](bool available, uint64_t bytes) {
    return available ? HumanBytesStr(bytes) : tr("n/a");
  };
  const auto set_peak_metric = [&](int row, const QString& label,
                                   bool full_available, uint64_t full_bytes,
                                   bool preview_available,
                                   uint64_t preview_bytes,
                                   double reduction_pct) {
    if (full_available && preview_available) {
      SetMetricWithDelta(metrics_table_, row, label, HumanBytesStr(full_bytes),
                         HumanBytesStr(preview_bytes), reduction_pct);
    } else {
      SetMetric(metrics_table_, row, label,
                peak_text(full_available, full_bytes),
                peak_text(preview_available, preview_bytes));
    }
  };
  set_peak_metric(11, tr("Working set peak"),
                  comparison.full.has_process_peak_working_set,
                  comparison.full.process_peak_working_set_bytes,
                  comparison.preview.has_process_peak_working_set,
                  comparison.preview.process_peak_working_set_bytes,
                  comparison.process_peak_working_set_reduction_pct);
  set_peak_metric(12, tr("Private bytes peak"),
                  comparison.full.has_process_peak_private,
                  comparison.full.process_peak_private_bytes,
                  comparison.preview.has_process_peak_private,
                  comparison.preview.process_peak_private_bytes,
                  comparison.process_peak_private_reduction_pct);
  SetMetric(metrics_table_, 13, tr("Quality RMSE"), "\u2014",
            comparison.quality.available ? FloatText(comparison.quality.rmse, 5)
                                         : tr("n/a"));
  SetMetric(metrics_table_, 14, tr("Quality MAE"), "\u2014",
            comparison.quality.available ? FloatText(comparison.quality.mae, 5)
                                         : tr("n/a"));
  SetMetric(metrics_table_, 15, tr("Quality max |\u0394|"), "\u2014",
            comparison.quality.available
                ? FloatText(comparison.quality.max_abs, 5)
                : QString::fromStdString(comparison.quality.note));
  SetMetric(metrics_table_, 16, tr("Quality reference"), QString(),
            !comparison.quality.available ? tr("n/a")
            : comparison.quality.reference == "boxes"
                ? tr("box average of the full decode")
                : tr("proportional boxes (preview of another size)"));
  SetMetric(metrics_table_, 17, tr("Measurement"),
            QString::fromLatin1(
                PreviewBenchMeasurementName(comparison.full.measurement)),
            QString::fromLatin1(
                PreviewBenchMeasurementName(comparison.preview.measurement)));
}

void PreviewBenchmarkWindow::UpdateBatchTable() {
  batch_table_->clearContents();
  batch_table_->setRowCount(static_cast<int>(batch_results_.size()));
  for (int row = 0; row < static_cast<int>(batch_results_.size()); ++row) {
    const PreviewBenchComparison& comparison = batch_results_[row];
    const QString filename =
        QFileInfo(QString::fromStdString(comparison.input_path)).fileName();
    auto* name_item = new QTableWidgetItem(filename);
    name_item->setToolTip(QString::fromStdString(comparison.input_path));
    batch_table_->setItem(row, 0, name_item);
    batch_table_->setItem(
        row, 1,
        new QTableWidgetItem(QString::fromLatin1(
            FrameEncodingName(comparison.full.frame_encoding))));
    batch_table_->setItem(
        row, 2,
        new QTableWidgetItem(QString::fromLatin1(
            PreviewBackendName(comparison.preview.preview_backend))));
    batch_table_->setItem(row, 3,
                          NumItem(MeanSigmaMs(comparison.full.wall_time_ms)));
    batch_table_->setItem(
        row, 4, NumItem(MeanSigmaMs(comparison.preview.wall_time_ms)));
    {
      const double speedup = comparison.wall_time_speedup;
      auto* speedup_item = new QTableWidgetItem(FloatText(speedup) + "\u00d7");
      speedup_item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
      if (speedup > 1.05)
        speedup_item->setForeground(QColor(0, 130, 0));
      else if (speedup < 0.95)
        speedup_item->setForeground(QColor(180, 0, 0));
      batch_table_->setItem(row, 5, speedup_item);
    }
    batch_table_->setItem(
        row, 6,
        NumItem(FloatText(comparison.preview.throughput_mpx_per_s) + " Mpx/s"));
    batch_table_->setItem(row, 7,
                          DeltaItem(comparison.decoded_bytes_reduction_pct));
    batch_table_->setItem(row, 8,
                          DeltaItem(comparison.decoder_peak_reduction_pct));
    batch_table_->setItem(
        row, 9, DeltaItem(comparison.decoder_num_allocations_reduction_pct));
    {
      // Marked when the preview is compared with proportional boxes.
      const QString rmse_text =
          !comparison.quality.available ? QString("n/a")
          : comparison.quality.reference == "boxes"
              ? FloatText(comparison.quality.rmse, 5)
              : FloatText(comparison.quality.rmse, 5) + " (prop.)";
      auto* rmse_item = new QTableWidgetItem(rmse_text);
      rmse_item->setTextAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
      batch_table_->setItem(row, 10, rmse_item);
    }
  }
}

}  // namespace tools
}  // namespace jpegxl
