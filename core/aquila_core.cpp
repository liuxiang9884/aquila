#include "core/websocket/active_spin_loop.h"
#include "core/websocket/cold_path_loop.h"
#include "core/websocket/critical_session.h"
#include "core/websocket/frame_codec.h"
#include "core/websocket/handshake.h"
#include "core/websocket/message_view.h"
#include "core/websocket/metrics.h"
#include "core/websocket/prepared_write.h"
#include "core/websocket/runtime_policy.h"
#include "core/websocket/state_machine.h"
#include "core/websocket/tls_socket.h"
#include "core/websocket/types.h"
#include "core/websocket/websocket_client.h"

namespace aquila {
namespace {

[[maybe_unused]] constexpr int kCoreBuildAnchor = 0;

}  // namespace
}  // namespace aquila
