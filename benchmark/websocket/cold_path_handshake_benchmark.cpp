#define private public
#include "core/websocket/tls_socket.h"
#undef private

#include "benchmark/websocket/benchmark_support.h"
#include "benchmark/websocket/io_benchmark_support.h"
#include "core/websocket/cold_path_loop.h"
#include "core/websocket/handshake.h"
#include "core/websocket/state_machine.h"
#include "core/websocket/types.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

using namespace aquila::websocket;
using namespace aquila::websocket::benchmarking;

namespace {

constexpr std::string_view kLocalhostCert = R"(-----BEGIN CERTIFICATE-----
MIIDHzCCAgegAwIBAgIUOjgmcE40zpN8c5N+5BxiZIH8mZ4wDQYJKoZIhvcNAQEL
BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDQyNDA0NDQ1MVoXDTI3MDQy
NDA0NDQ1MVowFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF
AAOCAQ8AMIIBCgKCAQEA42xdDcvc2A8nOZx3IHHsM67YTNugRaePqTsSzYllzfbT
aBIu9Z+Z12QyQcjM0MrAfSF+1LOdO6Y9uWIbIFuOYx0QRwikIgrV39O/05+O5ycH
c2q/uZpuaK54R1z5dQy+KpEcR8Yo1x3oZ1Iz0za29zVcTI8p4iNIV7tvzNq+SuQb
rW+zf6wgZ2gHFEx2hjhQigpAtRvLvVwiZm7X0pWYUVWORul1UKAztlPt55sXLcNv
9CnGl+BxT63p3MNnFOYQiTmlESHFsonfnj2R+OnKdpdxhqCShhEYXzXjnf88u/jj
49TgVGH8YyucgfT76Ir4Hdu+bnR1C9IvYoI/Tblw5wIDAQABo2kwZzAdBgNVHQ4E
FgQUWeIEh89oA0AUV9nrBiyO5ub0yXUwHwYDVR0jBBgwFoAUWeIEh89oA0AUV9nr
BiyO5ub0yXUwDwYDVR0TAQH/BAUwAwEB/zAUBgNVHREEDTALgglsb2NhbGhvc3Qw
DQYJKoZIhvcNAQELBQADggEBAHnZy/h2S3wD3lUV3zH8+cDarDlzgLSt6jylFRft
t6qxph0hCeMyxZzlqQTDsTbXUpHvjqJfM4WfeZf+3xFnOAzJT6YsyJC9DqgD6Bk4
D0uPQKTqU9yPZgvVTiUJNxO/WUIBEmB+kMpNkggww2VDh3jV0Z3g0uJZa4E0bHKx
F+nWE+uuaRgid1HX6IXJ3+dYItrNiBLEZaey2JBk0ZvRTC8ZnLmXOn/vcYMaXiF6
BUzFs/8nB+zRlXy3o0y68BFg1L/x69kmt7iJaIo63oaC/FSG5l2hon3txRSbzKue
3TvO39BU7bFCRX+XJfEzbmQr5aLhc3+kLQpaBXbG1Na2X0c=
-----END CERTIFICATE-----
)";

constexpr std::string_view kLocalhostKey = R"(-----BEGIN PRIVATE KEY-----
MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQDjbF0Ny9zYDyc5
nHcgcewzrthM26BFp4+pOxLNiWXN9tNoEi71n5nXZDJByMzQysB9IX7Us507pj25
YhsgW45jHRBHCKQiCtXf07/Tn47nJwdzar+5mm5ornhHXPl1DL4qkRxHxijXHehn
UjPTNrb3NVxMjyniI0hXu2/M2r5K5Butb7N/rCBnaAcUTHaGOFCKCkC1G8u9XCJm
btfSlZhRVY5G6XVQoDO2U+3nmxctw2/0KcaX4HFPrencw2cU5hCJOaURIcWyid+e
PZH46cp2l3GGoJKGERhfNeOd/zy7+OPj1OBUYfxjK5yB9Pvoivgd275udHUL0i9i
gj9NuXDnAgMBAAECggEAbBYm/bUbv3YoOtnRxlJOo9ugAptJR4GVJiWk0JnSEDsU
i7xEpElX2QZOWnSgX0VVicMfH+FDw8NFR7PIV68b86AvCcuiEmna7zeYzm/hf5vN
jz23ZHNwsQDmumgGSWqmgN2ZtsUHyQF2YJCxf9tbxw8N0HckPU1uhv/W6WI3Gaj9
0efKyHGGM/tKiIzp4cKbK7DWOPUc49XRFSUDAHXgtIQ3qHSHunjKtHeZT63Od9qa
MJ9ovqx3uLCA68EIcgdCJDWHgakHfZ+rF2ApC1Rw9AHLLi8UjzxxBXI14i7ZgYjZ
TAEOur+NYWJP/xO24RMGZl367xsYR2lKohCwO1Gq1QKBgQD24Rxzwxca5IgIbont
9vqSRLSSq2HARKJtw1hN6JhgxjMFY5QrWnkaoh0b9urZDKBSc/46eElBjMT6FpXh
Jte9fWNnQyZbWLXmHHkGY+N+TSDS+Lw8d7YQvNG8R0nYh2KG0d0kutejTAGzOiSV
+m1ldj78FXV5HxYkLPLeBY7tTQKBgQDr0z6gRT6SxToHd74M8NypVrz/OPItbK3H
1CJHPwT0KUbJf/wmELIYdNAr1S/VJKzDCnxzppj4j8HjL0czn+/ry7xe1+Ce0chM
Py4HK7HVQ0kJFJLosBrL6pOQ7bSRZwUj2uj8b8YCJISXjVcxjGBfZvaHPHazzrS8
nMzE4h7NAwKBgQDh4r7hvJMbbBZ7gIh7TwudYXfG60CZZzMnMyFMx5EEmtR8Dcy4
jiNYlxQDTj07My5NwwvN+9krPN3N5XRleeDT5DJbfTlPQy/LyCmEC0lPesqZvjSd
LvtK/Tj6CFAs6lLgAI1S2hILu7OihYSMJYKK31Jp3EiL9kGQAACH3JYV0QKBgQCX
PDEEfYPEgmFVmrAyAmGw46RvFOkSwoisB1o4UKzvnGz+GmrtSnW8g2VhRuXXDh8R
Me9gB65AcYkJFi/WZrJpiI30UQOHNsf6ReZRyO7R8sWq4hvYx99XeMdMAXV0bhn4
xZH3GgNlKmAyBP2vE/RWGmTtk5QYee6kqvYTKWRRWwKBgESoOCjswcNvs0Z/bBQW
GVVdXmOxtlfN1sDhkX4Ya5cPJyul7uH4wHfM1Dwlav6hA4V0CgUQ43Pxk3PIxJi3
l63SjC7KVjAzP9ySCs/kGpbC47AKQ3WGmhlgAYmKfG4EOv4A0NqY6YSAKZFkE7HJ
XO9B9uvibPfgqnC8aMYs2r/X
-----END PRIVATE KEY-----
)";

constexpr size_t kSamples = 256;
constexpr size_t kHandshakeBufferBytes = 4096;

bool LoadCertificate(SSL_CTX* ctx, std::string_view pem) noexcept {
  BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
  if (bio == nullptr) {
    return false;
  }
  X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  if (cert == nullptr) {
    return false;
  }
  const bool ok = SSL_CTX_use_certificate(ctx, cert) == 1;
  X509_free(cert);
  return ok;
}

bool LoadPrivateKey(SSL_CTX* ctx, std::string_view pem) noexcept {
  BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
  if (bio == nullptr) {
    return false;
  }
  EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  if (key == nullptr) {
    return false;
  }
  const bool ok = SSL_CTX_use_PrivateKey(ctx, key) == 1;
  EVP_PKEY_free(key);
  return ok;
}

bool AddTrustedCertificate(TlsSocket& socket) noexcept {
  if (!socket.Init()) {
    return false;
  }

  BIO* bio =
      BIO_new_mem_buf(kLocalhostCert.data(), static_cast<int>(kLocalhostCert.size()));
  if (bio == nullptr) {
    return false;
  }
  X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  if (cert == nullptr) {
    return false;
  }

  X509_STORE* store = SSL_CTX_get_cert_store(socket.ctx_);
  const bool ok = store != nullptr && X509_STORE_add_cert(store, cert) == 1;
  X509_free(cert);
  return ok;
}

bool WriteAllSsl(SSL* ssl, std::string_view bytes) noexcept {
  size_t offset = 0;
  while (offset < bytes.size()) {
    const int written =
        SSL_write(ssl, bytes.data() + offset, static_cast<int>(bytes.size() - offset));
    if (written > 0) {
      offset += static_cast<size_t>(written);
      continue;
    }
    const int error = SSL_get_error(ssl, written);
    if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
      continue;
    }
    return false;
  }
  return true;
}

std::string_view ExtractClientKey(std::string_view request) noexcept {
  size_t line_begin = 0;
  while (line_begin < request.size()) {
    const size_t line_end = request.find("\r\n", line_begin);
    if (line_end == std::string_view::npos) {
      return {};
    }
    if (line_end == line_begin) {
      return {};
    }

    const std::string_view line = request.substr(line_begin, line_end - line_begin);
    const size_t colon = line.find(':');
    if (colon != std::string_view::npos) {
      const std::string_view name = detail::TrimAsciiWhitespace(line.substr(0, colon));
      if (detail::AsciiEqualsIgnoreCase(name, "Sec-WebSocket-Key")) {
        return detail::TrimAsciiWhitespace(line.substr(colon + 1));
      }
    }
    line_begin = line_end + 2;
  }
  return {};
}

class LocalTlsWsServer {
 public:
  ~LocalTlsWsServer() noexcept { Stop(); }

  bool Start() noexcept {
    if (OPENSSL_init_ssl(0, nullptr) != 1) {
      return false;
    }

    ctx_ = SSL_CTX_new(TLS_server_method());
    if (ctx_ == nullptr) {
      return false;
    }
    SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);

    if (!LoadCertificate(ctx_, kLocalhostCert) ||
        !LoadPrivateKey(ctx_, kLocalhostKey) ||
        SSL_CTX_check_private_key(ctx_) != 1) {
      return false;
    }

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
      return false;
    }

    int reuse_addr = 1;
    if (::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse_addr,
                     sizeof(reuse_addr)) != 0) {
      return false;
    }
    if (!SetNonBlocking(listen_fd_)) {
      return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(0);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(listen_fd_, reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) != 0) {
      return false;
    }
    if (::listen(listen_fd_, 16) != 0) {
      return false;
    }

    socklen_t address_len = sizeof(address);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address),
                      &address_len) != 0) {
      return false;
    }
    port_ = ntohs(address.sin_port);

    stop_requested_.store(false);
    failed_.store(false);
    thread_ = std::thread([this]() { Run(); });
    return true;
  }

  void Stop() noexcept {
    stop_requested_.store(true);
    if (thread_.joinable()) {
      thread_.join();
    }
    if (listen_fd_ >= 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
    }
    if (ctx_ != nullptr) {
      SSL_CTX_free(ctx_);
      ctx_ = nullptr;
    }
  }

  std::uint16_t port() const noexcept { return port_; }

  std::uint64_t completed_handshakes() const noexcept {
    return completed_handshakes_.load();
  }

  bool failed() const noexcept { return failed_.load(); }

 private:
  void Run() noexcept {
    while (!stop_requested_.load()) {
      sockaddr_in client_address{};
      socklen_t client_address_len = sizeof(client_address);
      const int client_fd =
          ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_address),
                   &client_address_len);
      if (client_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          continue;
        }
        if (!stop_requested_.load()) {
          failed_.store(true);
        }
        return;
      }

      if (!HandleClient(client_fd)) {
        failed_.store(true);
        ::close(client_fd);
        return;
      }
      ::close(client_fd);
      completed_handshakes_.fetch_add(1);
    }
  }

  bool HandleClient(int client_fd) noexcept {
    SSL* ssl = SSL_new(ctx_);
    if (ssl == nullptr) {
      return false;
    }

    bool ok = false;
    if (SSL_set_fd(ssl, client_fd) == 1) {
      for (;;) {
        const int result = SSL_accept(ssl);
        if (result == 1) {
          ok = true;
          break;
        }
        const int error = SSL_get_error(ssl, result);
        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
          continue;
        }
        break;
      }
    }
    if (!ok) {
      SSL_free(ssl);
      return false;
    }

    std::array<char, kHandshakeBufferBytes> request_storage{};
    size_t request_bytes = 0;
    while (request_bytes < request_storage.size()) {
      const int received =
          SSL_read(ssl, request_storage.data() + request_bytes,
                   static_cast<int>(request_storage.size() - request_bytes));
      if (received > 0) {
        request_bytes += static_cast<size_t>(received);
        const std::string_view request(request_storage.data(), request_bytes);
        if (request.find("\r\n\r\n") != std::string_view::npos) {
          break;
        }
        continue;
      }
      const int error = SSL_get_error(ssl, received);
      if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
        continue;
      }
      SSL_free(ssl);
      return false;
    }

    const std::string_view request(request_storage.data(), request_bytes);
    const std::string_view client_key = ExtractClientKey(request);
    if (client_key.empty()) {
      SSL_free(ssl);
      return false;
    }

    std::array<char, 64> accept_storage{};
    std::string_view accept_key{};
    if (!detail::ComputeAcceptKey(client_key, accept_storage, accept_key)) {
      SSL_free(ssl);
      return false;
    }

    std::array<char, 512> response_storage{};
    const int response_size = std::snprintf(
        response_storage.data(), response_storage.size(),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %.*s\r\n"
        "\r\n",
        static_cast<int>(accept_key.size()), accept_key.data());
    if (response_size <= 0 ||
        static_cast<size_t>(response_size) >= response_storage.size()) {
      SSL_free(ssl);
      return false;
    }

    ok = WriteAllSsl(
        ssl, std::string_view(response_storage.data(), static_cast<size_t>(response_size)));
    SSL_shutdown(ssl);
    SSL_free(ssl);
    return ok;
  }

  SSL_CTX* ctx_{nullptr};
  int listen_fd_{-1};
  std::uint16_t port_{0};
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> failed_{false};
  std::atomic<std::uint64_t> completed_handshakes_{0};
  std::thread thread_{};
};

}  // namespace

int main() {
  LocalTlsWsServer server;
  if (!server.Start()) {
    return 1;
  }

  ConnectionConfig config{};
  config.host = "localhost";
  config.service = std::to_string(server.port());
  config.target = "/cold-path";
  config.enable_tls = true;
  config.read_buffer_bytes = 4096;
  config.frame_buffer_bytes = 4096;
  config.heartbeat_interval_ms = std::numeric_limits<std::uint32_t>::max();
  config.heartbeat_timeout_ms = std::numeric_limits<std::uint32_t>::max();
  config.runtime_policy.affinity_mode = AffinityMode::kNone;
  config.runtime_policy.lock_memory = false;
  config.runtime_policy.prefault_stack = false;

  TlsSocket socket;
  if (!AddTrustedCertificate(socket)) {
    return 1;
  }

  ColdPathLoop loop;
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kSamples);
  std::array<char, kHandshakeBufferBytes> handshake_storage{};

  for (size_t sample_index = 0; sample_index < kSamples; ++sample_index) {
    StateMachine state_machine;
    const std::uint64_t start_ns = NowNs();
    const bool ok =
        loop.RunUntilActive(socket, state_machine, config, handshake_storage);
    const std::uint64_t stop_ns = NowNs();
    if (!ok || state_machine.phase() != ConnectionPhase::kActive ||
        state_machine.last_error() != ConnectionError::kNone) {
      return 1;
    }
    samples_ns.push_back(stop_ns - start_ns);
    socket.Close();
  }

  if (server.failed() || server.completed_handshakes() != kSamples) {
    return 1;
  }

  PrintReport("cold_path_handshake", std::move(samples_ns), true,
              "local-tls-loopback", "handshakes",
              server.completed_handshakes());
  return 0;
}
