// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <QApplication>
#include <QMetaType>
#include <QStringList>

#include "tools/preview_benchmark/preview_demo_window.h"

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QApplication::setOrganizationName("JPEG XL project");
  QApplication::setApplicationName("Preview API demo");

  qRegisterMetaType<jpegxl::tools::DecodeWorker::Request>();
  qRegisterMetaType<jpegxl::tools::DecodeWorker::Result>();

  jpegxl::tools::PreviewDemoWindow window;
  QStringList args = app.arguments();
  args.removeFirst();
  if (!args.isEmpty()) {
    window.OpenFiles(args);
  }
  window.show();
  return app.exec();
}
