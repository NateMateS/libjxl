// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "tools/file_io.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "lib/jxl/testing.h"

namespace jpegxl {
namespace tools {
namespace {

// A path whose parent directory does not exist.
std::string PathInMissingDirectory(const char* name) {
  return ::testing::TempDir() + "jxl_file_io_test_no_such_dir/" + name;
}

TEST(FileIoTest, OpenMissingFileKeepsErrno) {
  const std::string path = ::testing::TempDir() + "jxl_file_io_test_missing";
  std::remove(path.c_str());
  errno = 0;
  FileWrapper f(path, "rb");
  EXPECT_EQ(static_cast<FILE*>(f), nullptr);
  // WriteFile reports fopen's error with strerror(errno).
  EXPECT_EQ(errno, ENOENT);
  EXPECT_EQ(f.size(), -1);
}

TEST(FileIoTest, ReadMissingFile) {
  std::vector<uint8_t> bytes;
  EXPECT_FALSE(ReadFile(PathInMissingDirectory("input"), &bytes));
}

TEST(FileIoTest, WriteIntoMissingDirectory) {
  const std::vector<uint8_t> bytes = {1, 2, 3};
  EXPECT_FALSE(WriteFile(PathInMissingDirectory("output"), bytes));
}

TEST(FileIoTest, WriteThenRead) {
  const std::string path = ::testing::TempDir() + "jxl_file_io_test_file";
  const std::vector<uint8_t> written = {0, 1, 2, 3, 255};
  ASSERT_TRUE(WriteFile(path, written));
  {
    FileWrapper f(path, "rb");
    ASSERT_NE(static_cast<FILE*>(f), nullptr);
    EXPECT_EQ(f.size(), static_cast<int64_t>(written.size()));
  }
  std::vector<uint8_t> read;
  EXPECT_TRUE(ReadFile(path, &read));
  EXPECT_EQ(read, written);
  std::remove(path.c_str());
}

}  // namespace
}  // namespace tools
}  // namespace jpegxl
