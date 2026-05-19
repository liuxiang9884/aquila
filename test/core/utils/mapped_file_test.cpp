#include "core/utils/mapped_file.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <gtest/gtest.h>

namespace {

class TempDir {
 public:
  TempDir()
      : path_(std::filesystem::temp_directory_path() /
              fmt::format("aquila_mapped_file_test_{}_{}", ::getpid(),
                          next_id_++)) {
    std::filesystem::create_directories(path_);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  ~TempDir() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] std::filesystem::path File(std::string_view name) const {
    return path_ / std::string{name};
  }

 private:
  std::filesystem::path path_;
  inline static std::uint32_t next_id_{0};
};

void WriteFile(const std::filesystem::path& path, std::string_view content) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(output.is_open()) << path;
  output.write(content.data(), static_cast<std::streamsize>(content.size()));
  ASSERT_TRUE(output.good()) << path;
}

TEST(MappedFileTest, MapsReadOnlyFileBytes) {
  TempDir temp_dir;
  const std::filesystem::path file = temp_dir.File("records.bin");
  WriteFile(file, "abcdef");

  const aquila::MappedFile mapped(file);

  ASSERT_NE(mapped.data(), nullptr);
  EXPECT_EQ(mapped.size(), 6U);
  EXPECT_EQ(std::string_view(mapped.data(), mapped.size()), "abcdef");
}

TEST(MappedFileTest, MapsSequentialReadOnlyFileBytes) {
  TempDir temp_dir;
  const std::filesystem::path file = temp_dir.File("sequential.bin");
  WriteFile(file, "abcdef");

  const aquila::MappedFile mapped(file,
                                  aquila::MappedFileAccessPattern::kSequential);

  ASSERT_NE(mapped.data(), nullptr);
  EXPECT_EQ(mapped.size(), 6U);
  EXPECT_EQ(std::string_view(mapped.data(), mapped.size()), "abcdef");
}

TEST(MappedFileTest, EmptyFileHasNullDataAndZeroSize) {
  TempDir temp_dir;
  const std::filesystem::path file = temp_dir.File("empty.bin");
  WriteFile(file, "");

  const aquila::MappedFile mapped(file);

  EXPECT_EQ(mapped.data(), nullptr);
  EXPECT_EQ(mapped.size(), 0U);
}

TEST(MappedFileTest, MoveTransfersMappingOwnership) {
  TempDir temp_dir;
  const std::filesystem::path file = temp_dir.File("move.bin");
  WriteFile(file, "xyz");

  aquila::MappedFile mapped(file);
  aquila::MappedFile moved(std::move(mapped));

  ASSERT_NE(moved.data(), nullptr);
  EXPECT_EQ(moved.size(), 3U);
  EXPECT_EQ(std::string_view(moved.data(), moved.size()), "xyz");
  EXPECT_EQ(mapped.data(), nullptr);
  EXPECT_EQ(mapped.size(), 0U);
}

TEST(MappedFileTest, RejectsMissingFile) {
  TempDir temp_dir;
  EXPECT_THROW((aquila::MappedFile(temp_dir.File("missing.bin"))),
               std::runtime_error);
}

}  // namespace
