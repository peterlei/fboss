// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include "fboss/agent/hw/test/HwTestFabricUtils.h"
#include "fboss/agent/HwSwitch.h"

#include <gtest/gtest.h>

namespace facebook::fboss {
void checkFabricReachability(const HwSwitch* hw) {
  auto reachability = hw->getFabricReachability();
  EXPECT_GT(reachability.size(), 0);
  for (auto [port, endpoint] : reachability) {
    if (!*endpoint.isAttached()) {
      continue;
    }

    int expectedPortId = -1;
    int expectedSwitchId = -1;

    if (endpoint.expectedPortId().has_value()) {
      expectedPortId = *endpoint.expectedPortId();
    }
    if (endpoint.expectedSwitchId().has_value()) {
      expectedSwitchId = *endpoint.expectedSwitchId();
    }

    XLOG(DBG2) << " On port: " << port
               << " got switch id: " << *endpoint.switchId()
               << " expected switch id: " << expectedSwitchId
               << " expected port id: " << expectedPortId
               << "port id: " << *endpoint.portId();
    EXPECT_EQ(*endpoint.switchId(), expectedSwitchId);
    EXPECT_EQ(*endpoint.switchType(), hw->getSwitchType());
    EXPECT_EQ(*endpoint.portId(), expectedPortId);
  }
}
} // namespace facebook::fboss
