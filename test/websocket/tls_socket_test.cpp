#define private public
#include "core/websocket/tls_socket.h"
#undef private

#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>

using namespace aquila::websocket;

int main() {
  TlsSocket socket;
  if (!socket.Init()) {
    return 1;
  }
  if (socket.WantsRead() || socket.WantsWrite() || socket.NativeFd() != -1) {
    return 1;
  }

  SSL* ssl = SSL_new(socket.ctx_);
  if (ssl == nullptr) {
    return 1;
  }
  const bool configured =
      TlsSocket::ConfigurePeerVerification(ssl, "fx-ws.gateio.ws");
  if (!configured) {
    SSL_free(ssl);
    return 1;
  }

  X509_VERIFY_PARAM* params = SSL_get0_param(ssl);
  const char* configured_host = X509_VERIFY_PARAM_get0_host(params, 0);
  const bool matches = configured_host != nullptr &&
                       std::string_view(configured_host) == "fx-ws.gateio.ws";
  SSL_free(ssl);
  if (!matches) {
    return 1;
  }

  ssl = SSL_new(socket.ctx_);
  if (ssl == nullptr) {
    return 1;
  }
  const bool empty_host_configured = TlsSocket::ConfigurePeerVerification(ssl, "");
  SSL_free(ssl);
  if (empty_host_configured) {
    return 1;
  }

  socket.wants_read_ = true;
  socket.wants_write_ = true;
  socket.fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (socket.fd_ < 0) {
    return 1;
  }
  socket.Close();
  return (!socket.WantsRead() && !socket.WantsWrite() &&
          socket.NativeFd() == -1)
             ? 0
             : 1;
}
