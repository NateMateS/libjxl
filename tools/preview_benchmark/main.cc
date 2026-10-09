// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <QApplication>

#include "tools/preview_benchmark/preview_benchmark_window.h"

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QApplication::setOrganizationName("JPEG XL project");
  QApplication::setApplicationName("Preview benchmark");

  jpegxl::tools::PreviewBenchmarkWindow window;
  QStringList args = app.arguments();
  args.removeFirst();
  if (!args.isEmpty()) {
    window.AddEntries(args);
  }
  window.show();
  return app.exec();
}
