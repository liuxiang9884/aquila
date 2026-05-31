#include "core/websocket/socket_timestamping.h"

#include <gtest/gtest.h>

namespace aquila::websocket {
namespace {

TEST(WebsocketSocketTimestampingTest, DefaultConfigIsDisabled) {
  SocketTimestampingConfig config;

  EXPECT_FALSE(config.enabled);
  EXPECT_FALSE(config.tx_software);
  EXPECT_FALSE(config.tx_sched);
  EXPECT_FALSE(config.tx_ack);
  EXPECT_FALSE(config.rx_software);
}

TEST(WebsocketSocketTimestampingTest, ComputesLocalStageDurations) {
  SocketTimestampingSnapshot snapshot;
  snapshot.write_complete_ns = 100;
  snapshot.tx_software_ns = 140;
  snapshot.tx_ack_ns = 250;
  snapshot.rx_software_ns = 400;
  snapshot.ack_receive_local_ns = 430;

  const SocketTimestampingStages stages =
      ComputeSocketTimestampingStages(snapshot);

  EXPECT_EQ(stages.write_complete_to_tx_software_ns, 40);
  EXPECT_EQ(stages.tx_software_to_tx_ack_ns, 110);
  EXPECT_EQ(stages.tx_ack_to_rx_software_ns, 150);
  EXPECT_EQ(stages.rx_software_to_ack_receive_ns, 30);
}

}  // namespace
}  // namespace aquila::websocket
