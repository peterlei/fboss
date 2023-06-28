// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#pragma once

#include "fboss/agent/AgentConfig.h"
#include "fboss/agent/HwSwitchCallback.h"

namespace facebook::fboss {

class TxPacket;
class StateDelta;

struct PlatformData {
  std::string volatileStateDir;
  std::string persistentStateDir;
  std::string crashSwitchStateFile;
  std::string crashThriftSwitchStateFile;
  std::string warmBootDir;
  std::string crashBadStateUpdateDir;
  std::string crashBadStateUpdateOldStateFile;
  std::string crashBadStateUpdateNewStateFile;
  std::string runningConfigDumpFile;
  bool supportsAddRemovePort;
};

struct HwSwitchHandler {
  virtual ~HwSwitchHandler() = default;

  virtual void initPlatform(
      std::unique_ptr<AgentConfig> config,
      uint32_t features) = 0;

  virtual HwInitResult initHw(
      HwSwitchCallback* callback,
      bool failHwCallsOnWarmboot) = 0;

  virtual void exitFatal() const = 0;

  virtual std::unique_ptr<TxPacket> allocatePacket(uint32_t size) const = 0;

  virtual bool sendPacketOutOfPortAsync(
      std::unique_ptr<TxPacket> pkt,
      PortID portID,
      std::optional<uint8_t> queue = std::nullopt) noexcept = 0;

  virtual bool sendPacketSwitchedSync(
      std::unique_ptr<TxPacket> pkt) noexcept = 0;

  virtual bool isValidStateUpdate(const StateDelta& delta) const = 0;

  virtual void unregisterCallbacks() = 0;

  virtual void gracefulExit(
      folly::dynamic& follySwitchState,
      state::WarmbootState& thriftSwitchState) = 0;

  virtual bool getAndClearNeighborHit(RouterID vrf, folly::IPAddress& ip) = 0;

  virtual folly::dynamic toFollyDynamic() const = 0;

  virtual std::optional<uint32_t> getHwLogicalPortId(PortID portID) const = 0;

  const PlatformData& getPlatformData() const {
    return platformData_;
  }

 protected:
  virtual void initPlatformData() = 0;
  PlatformData platformData_;
};

} // namespace facebook::fboss