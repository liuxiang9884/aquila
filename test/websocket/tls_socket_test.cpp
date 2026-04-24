#define private public
#include "core/websocket/tls_socket.h"
#undef private

#include <gtest/gtest.h>

#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>

using namespace aquila::websocket;

TEST(WebsocketTlsSocketTest, InitializesVerificationAndResetsStateOnClose) {
  TlsSocket socket;
  ASSERT_TRUE(socket.Init());
  EXPECT_FALSE(socket.WantsRead());
  EXPECT_FALSE(socket.WantsWrite());
  EXPECT_EQ(socket.NativeFd(), -1);

  SSL* ssl = SSL_new(socket.ctx_);
  ASSERT_NE(ssl, nullptr);
  const bool configured =
      TlsSocket::ConfigurePeerVerification(ssl, "fx-ws.gateio.ws");
  ASSERT_TRUE(configured);

  X509_VERIFY_PARAM* params = SSL_get0_param(ssl);
  const char* configured_host = X509_VERIFY_PARAM_get0_host(params, 0);
  const bool matches = configured_host != nullptr &&
                       std::string_view(configured_host) == "fx-ws.gateio.ws";
  SSL_free(ssl);
  EXPECT_TRUE(matches);

  ssl = SSL_new(socket.ctx_);
  ASSERT_NE(ssl, nullptr);
  const bool empty_host_configured = TlsSocket::ConfigurePeerVerification(ssl, "");
  SSL_free(ssl);
  EXPECT_FALSE(empty_host_configured);

  socket.wants_read_ = true;
  socket.wants_write_ = true;
  socket.fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(socket.fd_, 0);
  socket.Close();
  EXPECT_FALSE(socket.WantsRead());
  EXPECT_FALSE(socket.WantsWrite());
  EXPECT_EQ(socket.NativeFd(), -1);
}
