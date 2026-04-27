#ifndef AQUILA_TEST_WEBSOCKET_TLS_BLACKHOLE_SERVER_H_
#define AQUILA_TEST_WEBSOCKET_TLS_BLACKHOLE_SERVER_H_

#include "core/websocket/handshake.h"

#include <fmt/format.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace aquila::websocket::test {

enum class TlsServerAction {
  kCloseTcpImmediately,
  kBlackholeTcp,
  kHandshake101ThenClose,
  kHandshake101ThenStayOpen,
  kHandshake101ThenDrain,
};

class TlsBlackholeServer {
 public:
  explicit TlsBlackholeServer(std::vector<TlsServerAction> actions)
      : actions_(std::move(actions)) {}

  ~TlsBlackholeServer() noexcept {
    stop_.store(true, std::memory_order_release);
    if (accept_thread_.joinable()) {
      accept_thread_.join();
    }
    if (listen_fd_ >= 0) {
      ::close(listen_fd_);
    }
    if (!trust_store_path_.empty()) {
      ::unlink(trust_store_path_.c_str());
    }
    if (ctx_ != nullptr) {
      SSL_CTX_free(ctx_);
    }
    if (cert_ != nullptr) {
      X509_free(cert_);
    }
    if (pkey_ != nullptr) {
      EVP_PKEY_free(pkey_);
    }
  }

  TlsBlackholeServer(const TlsBlackholeServer&) = delete;
  TlsBlackholeServer& operator=(const TlsBlackholeServer&) = delete;

  bool Start() noexcept {
    if (!InitTls()) {
      return false;
    }
    if (!OpenListenSocket()) {
      return false;
    }
    accept_thread_ = std::thread([this] { AcceptLoop(); });
    return true;
  }

  int port() const noexcept { return port_; }

  size_t accepted_count() const noexcept {
    return accepted_count_.load(std::memory_order_acquire);
  }

  size_t switched_count() const noexcept {
    return switched_count_.load(std::memory_order_acquire);
  }

  size_t drained_bytes() const noexcept {
    return drained_bytes_.load(std::memory_order_acquire);
  }

  bool WaitForAccepted(size_t count,
                       std::chrono::milliseconds timeout) noexcept {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, timeout, [this, count] {
      return accepted_count_.load(std::memory_order_acquire) >= count;
    });
  }

 private:
  bool InitTls() noexcept {
    if (OPENSSL_init_ssl(0, nullptr) != 1) {
      return false;
    }

    EVP_PKEY_CTX* keygen = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (keygen == nullptr) {
      return false;
    }
    EVP_PKEY* generated_key = nullptr;
    const bool key_ok =
        EVP_PKEY_keygen_init(keygen) == 1 &&
        EVP_PKEY_CTX_set_rsa_keygen_bits(keygen, 2048) == 1 &&
        EVP_PKEY_keygen(keygen, &generated_key) == 1;
    EVP_PKEY_CTX_free(keygen);
    if (!key_ok || generated_key == nullptr) {
      return false;
    }
    pkey_ = generated_key;

    cert_ = X509_new();
    if (cert_ == nullptr) {
      return false;
    }
    ASN1_INTEGER_set(X509_get_serialNumber(cert_), 1);
    X509_gmtime_adj(X509_get_notBefore(cert_), 0);
    X509_gmtime_adj(X509_get_notAfter(cert_), 60 * 60);
    X509_set_version(cert_, 2);
    if (X509_set_pubkey(cert_, pkey_) != 1) {
      return false;
    }

    X509_NAME* name = X509_get_subject_name(cert_);
    if (name == nullptr ||
        X509_NAME_add_entry_by_txt(
            name, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("localhost"), -1, -1,
            0) != 1 ||
        X509_set_issuer_name(cert_, name) != 1) {
      return false;
    }

    X509V3_CTX extension_ctx;
    X509V3_set_ctx(&extension_ctx, cert_, cert_, nullptr, nullptr, 0);
    X509_EXTENSION* san = X509V3_EXT_conf_nid(
        nullptr, &extension_ctx, NID_subject_alt_name,
        const_cast<char*>("DNS:localhost,IP:127.0.0.1"));
    if (san == nullptr) {
      return false;
    }
    const int add_ext_ok = X509_add_ext(cert_, san, -1);
    X509_EXTENSION_free(san);
    if (add_ext_ok != 1 || X509_sign(cert_, pkey_, EVP_sha256()) == 0) {
      return false;
    }

    ctx_ = SSL_CTX_new(TLS_server_method());
    if (ctx_ == nullptr ||
        SSL_CTX_use_certificate(ctx_, cert_) != 1 ||
        SSL_CTX_use_PrivateKey(ctx_, pkey_) != 1) {
      return false;
    }

    trust_store_path_ =
        fmt::format("/tmp/aquila_tls_blackhole_ca_{}.pem", ::getpid());
    FILE* file = std::fopen(trust_store_path_.c_str(), "w");
    if (file == nullptr) {
      return false;
    }
    const bool wrote = PEM_write_X509(file, cert_) == 1;
    std::fclose(file);
    if (!wrote) {
      return false;
    }
    return ::setenv("SSL_CERT_FILE", trust_store_path_.c_str(), 1) == 0;
  }

  bool OpenListenSocket() noexcept {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
      return false;
    }
    int enable = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) !=
        0) {
      return false;
    }
    if (::listen(listen_fd_, 16) != 0) {
      return false;
    }

    sockaddr_in actual{};
    socklen_t len = sizeof(actual);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&actual),
                      &len) != 0) {
      return false;
    }
    port_ = ntohs(actual.sin_port);
    return true;
  }

  void AcceptLoop() noexcept {
    while (!stop_.load(std::memory_order_acquire)) {
      fd_set read_set;
      FD_ZERO(&read_set);
      FD_SET(listen_fd_, &read_set);
      timeval tv{0, 20'000};
      const int ready =
          ::select(listen_fd_ + 1, &read_set, nullptr, nullptr, &tv);
      if (ready <= 0) {
        continue;
      }

      const int client_fd = ::accept(listen_fd_, nullptr, nullptr);
      if (client_fd < 0) {
        continue;
      }
      accepted_count_.fetch_add(1, std::memory_order_acq_rel);
      cv_.notify_all();
      HandleClient(client_fd, NextAction());
    }
  }

  TlsServerAction NextAction() noexcept {
    if (next_action_ >= actions_.size()) {
      return actions_.empty() ? TlsServerAction::kHandshake101ThenClose
                              : actions_.back();
    }
    return actions_[next_action_++];
  }

  void HandleClient(int client_fd, TlsServerAction action) noexcept {
    timeval timeout{2, 0};
    ::setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                 sizeof(timeout));
    ::setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                 sizeof(timeout));

    if (action == TlsServerAction::kCloseTcpImmediately) {
      ::close(client_fd);
      return;
    }
    if (action == TlsServerAction::kBlackholeTcp) {
      while (!stop_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      ::close(client_fd);
      return;
    }

    SSL* ssl = SSL_new(ctx_);
    if (ssl == nullptr || SSL_set_fd(ssl, client_fd) != 1 ||
        SSL_accept(ssl) != 1) {
      if (ssl != nullptr) {
        SSL_free(ssl);
      }
      ::close(client_fd);
      return;
    }

    if (SendSwitchingProtocols(ssl)) {
      switched_count_.fetch_add(1, std::memory_order_acq_rel);
      cv_.notify_all();
    }

    if (action == TlsServerAction::kHandshake101ThenStayOpen) {
      while (!stop_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    } else if (action == TlsServerAction::kHandshake101ThenDrain) {
      DrainApplicationData(ssl);
    }

    SSL_shutdown(ssl);
    SSL_free(ssl);
    ::close(client_fd);
  }

  bool SendSwitchingProtocols(SSL* ssl) noexcept {
    std::string request;
    char buffer[1024];
    while (request.find("\r\n\r\n") == std::string::npos) {
      const int received = SSL_read(ssl, buffer, sizeof(buffer));
      if (received <= 0) {
        return false;
      }
      request.append(buffer, static_cast<size_t>(received));
      if (request.size() > 8192) {
        return false;
      }
    }

    const std::string_view key = FindHeader(request, "Sec-WebSocket-Key");
    if (key.empty()) {
      return false;
    }
    std::array<char, 64> accept_storage{};
    std::string_view accept_key{};
    if (!detail::ComputeAcceptKey(key, accept_storage, accept_key)) {
      return false;
    }

    const std::string response =
        fmt::format("HTTP/1.1 101 Switching Protocols\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    "Sec-WebSocket-Accept: {}\r\n"
                    "\r\n",
                    accept_key);
    return SSL_write(ssl, response.data(),
                     static_cast<int>(response.size())) ==
           static_cast<int>(response.size());
  }

  void DrainApplicationData(SSL* ssl) noexcept {
    std::array<std::byte, 4096> buffer{};
    while (!stop_.load(std::memory_order_acquire)) {
      const int received = SSL_read(ssl, buffer.data(),
                                    static_cast<int>(buffer.size()));
      if (received > 0) {
        drained_bytes_.fetch_add(static_cast<size_t>(received),
                                 std::memory_order_acq_rel);
        continue;
      }
      const int error = SSL_get_error(ssl, received);
      if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      return;
    }
  }

  static std::string_view FindHeader(std::string_view request,
                                     std::string_view header) noexcept {
    size_t line_begin = 0;
    while (line_begin < request.size()) {
      const size_t line_end = request.find("\r\n", line_begin);
      if (line_end == std::string_view::npos || line_end == line_begin) {
        return {};
      }
      const std::string_view line =
          request.substr(line_begin, line_end - line_begin);
      const size_t colon = line.find(':');
      if (colon != std::string_view::npos) {
        const auto name = detail::TrimAsciiWhitespace(line.substr(0, colon));
        if (detail::AsciiEqualsIgnoreCase(name, header)) {
          return detail::TrimAsciiWhitespace(line.substr(colon + 1));
        }
      }
      line_begin = line_end + 2;
    }
    return {};
  }

  std::vector<TlsServerAction> actions_;
  size_t next_action_{0};
  std::atomic<bool> stop_{false};
  std::atomic<size_t> accepted_count_{0};
  std::atomic<size_t> switched_count_{0};
  std::atomic<size_t> drained_bytes_{0};
  mutable std::mutex mutex_{};
  std::condition_variable cv_{};
  int listen_fd_{-1};
  int port_{0};
  std::thread accept_thread_{};
  SSL_CTX* ctx_{nullptr};
  X509* cert_{nullptr};
  EVP_PKEY* pkey_{nullptr};
  std::string trust_store_path_{};
};

}  // namespace aquila::websocket::test

#endif  // AQUILA_TEST_WEBSOCKET_TLS_BLACKHOLE_SERVER_H_
