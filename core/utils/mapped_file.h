#ifndef AQUILA_CORE_UTILS_MAPPED_FILE_H_
#define AQUILA_CORE_UTILS_MAPPED_FILE_H_

#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <fmt/format.h>

#include <fcntl.h>

namespace aquila {

enum class MappedFileAccessPattern : std::uint8_t {
  kNormal,
  kSequential,
};

class MappedFile {
 public:
  MappedFile() noexcept = default;

  explicit MappedFile(const std::filesystem::path& path,
                      MappedFileAccessPattern access_pattern =
                          MappedFileAccessPattern::kNormal) {
    Open(path, access_pattern);
  }

  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;

  MappedFile(MappedFile&& other) noexcept {
    MoveFrom(other);
  }

  MappedFile& operator=(MappedFile&& other) noexcept {
    if (this != &other) {
      Reset();
      MoveFrom(other);
    }
    return *this;
  }

  ~MappedFile() {
    Reset();
  }

  [[nodiscard]] const char* data() const noexcept {
    return data_;
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return size_;
  }

  [[nodiscard]] bool empty() const noexcept {
    return size_ == 0;
  }

 private:
  void Open(const std::filesystem::path& path,
            MappedFileAccessPattern access_pattern) {
    if (path.empty()) {
      throw std::invalid_argument("mapped file path is empty");
    }

    std::error_code file_size_error;
    const std::uintmax_t file_size =
        std::filesystem::file_size(path, file_size_error);
    if (file_size_error) {
      throw std::runtime_error(
          fmt::format("failed to inspect mapped file '{}': {}", path.string(),
                      file_size_error.message()));
    }
    if (file_size == 0) {
      return;
    }
    if (file_size >
        static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
      throw std::runtime_error(
          fmt::format("mapped file '{}' is too large", path.string()));
    }

    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      const std::error_code error(errno, std::generic_category());
      throw std::runtime_error(
          fmt::format("failed to open mapped file '{}': {}", path.string(),
                      error.message()));
    }

    const std::size_t map_size = static_cast<std::size_t>(file_size);
    void* mapping = ::mmap(nullptr, map_size, PROT_READ, MAP_PRIVATE, fd, 0);
    const int saved_errno = errno;
    ::close(fd);
    if (mapping == MAP_FAILED) {
      const std::error_code error(saved_errno, std::generic_category());
      throw std::runtime_error(fmt::format("failed to mmap file '{}': {}",
                                           path.string(), error.message()));
    }

    ApplyAccessPattern(mapping, map_size, access_pattern);
    data_ = static_cast<const char*>(mapping);
    size_ = map_size;
  }

  static void ApplyAccessPattern(
      void* mapping, std::size_t map_size,
      MappedFileAccessPattern access_pattern) noexcept {
    switch (access_pattern) {
      case MappedFileAccessPattern::kNormal:
        return;
      case MappedFileAccessPattern::kSequential:
        (void)::madvise(mapping, map_size, MADV_SEQUENTIAL);
        return;
    }
  }

  void Reset() noexcept {
    if (data_ != nullptr) {
      ::munmap(const_cast<char*>(data_), size_);
      data_ = nullptr;
      size_ = 0;
    }
  }

  void MoveFrom(MappedFile& other) noexcept {
    data_ = std::exchange(other.data_, nullptr);
    size_ = std::exchange(other.size_, 0);
  }

  const char* data_{nullptr};
  std::size_t size_{0};
};

}  // namespace aquila

#endif  // AQUILA_CORE_UTILS_MAPPED_FILE_H_
