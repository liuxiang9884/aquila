#ifndef AQUILA_CORE_WEBSOCKET_TLS_SOCKET_H_
#define AQUILA_CORE_WEBSOCKET_TLS_SOCKET_H_

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>

#include "core/websocket/types.h"

namespace aquila::websocket {

class TlsSocket {
 public:
  TlsSocket() = default;

  ~TlsSocket() noexcept {
    ResetSsl();
    ResetContext();
  }

  TlsSocket(const TlsSocket&) = delete;
  TlsSocket& operator=(const TlsSocket&) = delete;

  TlsSocket(TlsSocket&& other) noexcept { MoveFrom(other); }

  TlsSocket& operator=(TlsSocket&& other) noexcept {
    if (this != &other) {
      Close();
      ResetContext();
      MoveFrom(other);
    }
    return *this;
  }

  bool Init() noexcept {
    IgnoreSigpipeOnce();
    wants_read_ = false;
    wants_write_ = false;

    if (ctx_ != nullptr) {
      return true;
    }

    if (OPENSSL_init_ssl(0, nullptr) != 1) {
      return false;
    }

    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == nullptr) {
      return false;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
      SSL_CTX_free(ctx);
      return false;
    }

    ctx_ = ctx;
    return true;
  }

  bool OpenAndConnect(const ConnectionConfig& config) noexcept {
    wants_read_ = false;
    wants_write_ = false;

    if (!Init()) {
      return false;
    }

    Close();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* addresses = nullptr;
    if (getaddrinfo(config.host.c_str(), config.service.c_str(), &hints,
                    &addresses) != 0) {
      return false;
    }

    bool connected = false;
    for (addrinfo* current = addresses; current != nullptr;
         current = current->ai_next) {
      const int fd =
          ::socket(current->ai_family, current->ai_socktype, current->ai_protocol);
      if (fd < 0) {
        continue;
      }

      if (!SetNonBlocking(fd)) {
        ::close(fd);
        continue;
      }

      const int result = ::connect(fd, current->ai_addr, current->ai_addrlen);
      if (result == 0) {
        fd_ = fd;
        connected = true;
        wants_read_ = false;
        wants_write_ = false;
        break;
      }

      if (result < 0 && errno == EINPROGRESS) {
        fd_ = fd;
        connected = true;
        wants_read_ = false;
        wants_write_ = true;
        break;
      }

      ::close(fd);
    }
    freeaddrinfo(addresses);

    if (!connected) {
      return false;
    }

    SSL* ssl = SSL_new(ctx_);
    if (ssl == nullptr) {
      Close();
      return false;
    }
    ssl_ = ssl;

    if (!ConfigurePeerVerification(ssl_, config.host) ||
        SSL_set_tlsext_host_name(ssl_, config.host.c_str()) != 1 ||
        SSL_set_fd(ssl_, fd_) != 1) {
      Close();
      return false;
    }

    SSL_set_connect_state(ssl_);
    return true;
  }

  bool FinishHandshake() noexcept {
    wants_read_ = false;
    wants_write_ = false;

    if (ssl_ == nullptr) {
      return false;
    }

    const int result = SSL_connect(ssl_);
    if (result == 1) {
      return true;
    }

    const int error = SSL_get_error(ssl_, result);
    if (error == SSL_ERROR_WANT_READ) {
      wants_read_ = true;
      return false;
    }
    if (error == SSL_ERROR_WANT_WRITE) {
      wants_write_ = true;
      return false;
    }
    return false;
  }

  ssize_t ReadSome(std::span<std::byte> buffer) noexcept {
    wants_read_ = false;
    wants_write_ = false;

    if (ssl_ == nullptr || buffer.empty()) {
      return -1;
    }

    int result = SSL_read(ssl_, buffer.data(), static_cast<int>(buffer.size()));
    if (result > 0) {
      return result;
    }

    const int error = SSL_get_error(ssl_, result);
    if (error == SSL_ERROR_WANT_READ) {
      wants_read_ = true;
      errno = EAGAIN;
      return -1;
    }
    if (error == SSL_ERROR_WANT_WRITE) {
      wants_write_ = true;
      errno = EAGAIN;
      return -1;
    }

    errno = EIO;
    return -1;
  }

  ssize_t WriteSome(std::span<const std::byte> buffer) noexcept {
    wants_read_ = false;
    wants_write_ = false;

    if (ssl_ == nullptr || buffer.empty()) {
      return -1;
    }

    int result =
        SSL_write(ssl_, buffer.data(), static_cast<int>(buffer.size()));
    if (result > 0) {
      return result;
    }

    const int error = SSL_get_error(ssl_, result);
    if (error == SSL_ERROR_WANT_READ) {
      wants_read_ = true;
      errno = EAGAIN;
      return -1;
    }
    if (error == SSL_ERROR_WANT_WRITE) {
      wants_write_ = true;
      errno = EAGAIN;
      return -1;
    }

    errno = EIO;
    return -1;
  }

  bool WantsRead() const noexcept { return wants_read_; }

  bool WantsWrite() const noexcept { return wants_write_; }

  int NativeFd() const noexcept { return fd_; }

  void Close() noexcept {
    wants_read_ = false;
    wants_write_ = false;
    ResetSsl();
  }

 private:
  static bool ConfigurePeerVerification(SSL* ssl,
                                        std::string_view host) noexcept {
    if (ssl == nullptr || host.empty()) {
      return false;
    }

    X509_VERIFY_PARAM* params = SSL_get0_param(ssl);
    if (params == nullptr) {
      return false;
    }

    return X509_VERIFY_PARAM_set1_host(params, host.data(), host.size()) == 1;
  }

  static bool SetNonBlocking(int fd) noexcept {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
      return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
  }

  static void IgnoreSigpipeOnce() noexcept {
    // OpenSSL's socket BIO uses write(2), which can raise SIGPIPE when a peer
    // closes during TLS handshake/write. Reconnect policy needs that as an
    // ordinary socket error, not process termination.
    static const bool ignored = [] {
      std::signal(SIGPIPE, SIG_IGN);
      return true;
    }();
    (void)ignored;
  }

  void ResetSsl() noexcept {
    if (ssl_ != nullptr) {
      SSL_shutdown(ssl_);
      SSL_free(ssl_);
      ssl_ = nullptr;
    }
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  void ResetContext() noexcept {
    if (ctx_ != nullptr) {
      SSL_CTX_free(ctx_);
      ctx_ = nullptr;
    }
  }

  void MoveFrom(TlsSocket& other) noexcept {
    ctx_ = other.ctx_;
    ssl_ = other.ssl_;
    fd_ = other.fd_;
    wants_read_ = other.wants_read_;
    wants_write_ = other.wants_write_;

    other.ctx_ = nullptr;
    other.ssl_ = nullptr;
    other.fd_ = -1;
    other.wants_read_ = false;
    other.wants_write_ = false;
  }

  SSL_CTX* ctx_{nullptr};
  SSL* ssl_{nullptr};
  int fd_{-1};
  bool wants_read_{false};
  bool wants_write_{false};
};

}  // namespace aquila::websocket

#endif  // AQUILA_CORE_WEBSOCKET_TLS_SOCKET_H_
