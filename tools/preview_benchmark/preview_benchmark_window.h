// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#ifndef TOOLS_PREVIEW_BENCHMARK_PREVIEW_BENCHMARK_WINDOW_H_
#define TOOLS_PREVIEW_BENCHMARK_PREVIEW_BENCHMARK_WINDOW_H_

#include <QMainWindow>
#include <vector>

#include "tools/comparison_viewer/split_image_view.h"
#include "tools/preview_benchmark/benchmark_core.h"

class QListWidget;
class QComboBox;
class QSpinBox;
class QPushButton;
class QTableWidget;
class QListWidgetItem;

namespace jpegxl {
namespace tools {

class PreviewBenchmarkWindow : public QMainWindow {
  Q_OBJECT

 public:
  explicit PreviewBenchmarkWindow(QWidget* parent = nullptr);
  void AddEntries(const QStringList& entries);

 private slots:
  void OpenFiles();
  void OpenDirectory();
  void RunCurrent();
  void RunBatch();
  void ExportCsv();
  void SavePreviewImages();
  void HandleCurrentItemChanged(QListWidgetItem* current,
                                QListWidgetItem* previous);

 private:
  PreviewBenchOptions CurrentOptions() const;
  QString CurrentPath() const;
  void RefreshInputList();
  void AddPathRecursive(const QString& entry);
  void UpdateMetricsTable(const PreviewBenchComparison& comparison);
  void UpdateBatchTable();

  QListWidget* inputs_;
  QComboBox* preview_downsampling_;
  QSpinBox* iterations_;
  QSpinBox* threads_;
  QPushButton* export_csv_;
  QPushButton* save_previews_;
  QPushButton* run_current_;
  QPushButton* run_batch_;
  SplitImageView* split_view_;
  QTableWidget* metrics_table_;
  QTableWidget* batch_table_;

  // Decided once at startup, from whether preview_bench_worker can be run.
  PreviewBenchMeasurement measurement_ = PreviewBenchMeasurement::kWorker;
  QStringList input_paths_;
  bool has_current_result_ = false;
  PreviewBenchComparison current_result_;
  std::vector<PreviewBenchComparison> batch_results_;
};

}  // namespace tools
}  // namespace jpegxl

#endif  // TOOLS_PREVIEW_BENCHMARK_PREVIEW_BENCHMARK_WINDOW_H_
