#include "core/websocket/tls_socket.h"

using namespace aquila::websocket;

int main() {
  TlsSocket socket;
  return socket.Init() ? 0 : 1;
}
