#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

struct PipeCloser {
  void operator()(FILE* pipe) const noexcept {
    if (pipe != nullptr) {
      pclose(pipe);
    }
  }
};

std::string RunHelloWorld() {
  std::array<char, 256> buffer{};
  std::string output;
  std::unique_ptr<FILE, PipeCloser> pipe(popen(HELLO_WORLD_TEST_TARGET, "r"));
  if (pipe == nullptr) {
    throw std::runtime_error("failed to launch aquila_hello_world");
  }
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) !=
         nullptr) {
    output += buffer.data();
  }
  return output;
}

}  // namespace

TEST(HelloWorldTest, PrintsExpectedOutput) {
  EXPECT_EQ(RunHelloWorld(), "hello world\n");
}
