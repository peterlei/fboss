/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#pragma once

#include "fboss/agent/if/gen-cpp2/AgentHwTestCtrl.h"

#include "fboss/agent/FbossError.h"
#include "fboss/agent/HwSwitch.h"

#include "fboss/agent/gen-cpp2/switch_state_types.h"

#include <cstdint>
#include <memory>

namespace facebook::fboss {
class HwSwitch;
}
namespace facebook::fboss::utility {
class HwTestThriftHandler : public AgentHwTestCtrlSvIf {
 public:
  explicit HwTestThriftHandler(HwSwitch* hwSwitch) : hwSwitch_(hwSwitch) {}

  int32_t getDefaultAclTableNumAclEntries() override;

  int32_t getAclTableNumAclEntries(std::unique_ptr<std::string> name) override;

  bool isDefaultAclTableEnabled() override;

  bool isAclTableEnabled(std::unique_ptr<std::string> name) override;

  bool isAclEntrySame(
      std::unique_ptr<state::AclEntryFields> aclEntry,
      std::unique_ptr<std::string> aclTableName) override;

  bool areAllAclEntriesEnabled() override;

  bool isStatProgrammedInDefaultAclTable(
      std::unique_ptr<std::vector<::std::string>> aclEntryNames,
      std::unique_ptr<std::string> counterName,
      std::unique_ptr<std::vector<cfg::CounterType>> types) override;

  bool isStatProgrammedInAclTable(
      std::unique_ptr<std::vector<::std::string>> aclEntryNames,
      std::unique_ptr<std::string> counterName,
      std::unique_ptr<std::vector<cfg::CounterType>> types,
      std::unique_ptr<std::string> tableName) override;

  bool isMirrorProgrammed(std::unique_ptr<state::MirrorFields> mirror) override;

  bool isPortMirrored(
      int32_t port,
      std::unique_ptr<std::string> mirror,
      bool ingress) override;

  bool isPortSampled(
      int32_t port,
      std::unique_ptr<std::string> mirror,
      bool ingress) override;

  bool isAclEntryMirrored(
      std::unique_ptr<std::string> aclEntry,
      std::unique_ptr<std::string> mirror,
      bool ingress) override;
  void getNeighborInfo(
      NeighborInfo& neighborInfo,
      std::unique_ptr<::facebook::fboss::IfAndIP> neighbor) override;

  int getHwEcmpSize(
      std::unique_ptr<CIDRNetwork> prefix,
      int routerID,
      int sizeInSw) override;

  void getEcmpWeights(
      std::map<::std::int32_t, ::std::int32_t>& weights,
      std::unique_ptr<CIDRNetwork> prefix,
      int routerID) override;

  void injectFecError(
      std::unique_ptr<std::vector<int>> hwPorts,
      bool injectCorrectable) override;

  void injectSwitchReachabilityChangeNotification() override;
  void getRouteInfo(RouteInfo& routeInfo, std::unique_ptr<IpPrefix> prefix)
      override;
  bool isRouteHit(std::unique_ptr<IpPrefix> prefix) override;
  void clearRouteHit(std::unique_ptr<IpPrefix> prefix) override;
  bool isRouteToNexthop(
      std::unique_ptr<IpPrefix> prefix,
      std::unique_ptr<network::thrift::BinaryAddress> nexthop) override;
  bool isProgrammedInHw(
      int intfID,
      std::unique_ptr<IpPrefix> prefix,
      std::unique_ptr<MplsLabelStack> labelStack,
      int refCount) override;

  void getPortInfo(
      ::std::vector<::facebook::fboss::utility::PortInfo>& portInfos,
      std::unique_ptr<::std::vector<::std::int32_t>> portIds) override;

  bool verifyPortLedStatus(int portId, bool status) override;
  bool verifyPGSettings(int portId, bool pfcEnabled) override;
  void getAggPortInfo(
      ::std::vector<::facebook::fboss::utility::AggPortInfo>& aggPortInfos,
      std::unique_ptr<::std::vector<::std::int32_t>> aggPortIds) override;
  int getNumAggPorts() override;
  bool verifyPktFromAggPort(int aggPortId) override;

  void triggerParityError() override;

  int32_t getEgressSharedPoolLimitBytes() override;

  void printDiagCmd(std::unique_ptr<::std::string>) override;

  void updateFlowletStats() override;

  cfg::SwitchingMode getFwdSwitchingMode(
      std::unique_ptr<state::RouteNextHopEntry> routeNextHopEntry) override;

  bool getPtpTcEnabled() override;

  void clearInterfacePhyCounters(
      std::unique_ptr<::std::vector<::std::int32_t>> portIds) override;

  // udf related APIs
  bool validateUdfConfig(
      std::unique_ptr<::std::string> udfGroupName,
      std::unique_ptr<::std::string> udfPackeMatchName) override;
  bool validateRemoveUdfGroup(
      std::unique_ptr<::std::string> udfGroupName,
      int udfGroupId) override;
  bool validateRemoveUdfPacketMatcher(
      std::unique_ptr<::std::string> udfPackeMatchName,
      int32_t udfPacketMatcherId) override;
  int32_t getHwUdfGroupId(std::unique_ptr<::std::string> udfGroupName) override;

  int32_t getHwUdfPacketMatcherId(
      std::unique_ptr<::std::string> udfPackeMatchName) override;
  bool validateUdfAclRoceOpcodeConfig(
      std::unique_ptr<::facebook::fboss::state::SwitchState> curState) override;

  bool validateUdfIdsInQset(int aclGroupId, bool isSet) override;

  int32_t getNumTeFlowEntries() override;
  bool checkSwHwTeFlowMatch(
      std::unique_ptr<::facebook::fboss::state::TeFlowEntryFields>
          flowEntryFields) override;

  /**
   * @brief Verifies that ECMP groups are correctly configured for flowlet
   * switching on BCM hardware
   *
   * This function validates that ECMP (Equal Cost Multi-Path) groups have the
   * proper flowlet switching configuration applied, including dynamic mode,
   * age, and size parameters. It also checks that ECMP members have the correct
   * status for flowlet operation.
   *
   * @param ip CIDR network prefix to identify the route for verification
   * @param settings Switch settings containing flowlet switching configuration
   * @param flowletEnable Boolean indicating whether flowlet switching should be
   * enabled
   * @return true if ECMP flowlet configuration verification passes, false
   * otherwise
   */
  bool verifyEcmpForFlowletSwitchingHandler(
      std::unique_ptr<CIDRNetwork> ip,
      std::unique_ptr<::facebook::fboss::state::SwitchSettingsFields> settings,
      bool flowletEnable) override;

  /**
   * @brief Verifies that port-specific flowlet configuration parameters are
   * correctly applied to BCM hardware egress objects
   *
   * @param prefix CIDR network prefix to identify the route for verification
   * @param cfg Port flowlet configuration containing scaling factor, load
   * weight, and queue weight parameters
   * @param flowletEnable Boolean indicating whether flowlet switching should be
   * enabled
   * @return true if port flowlet configuration verification passes, false
   * otherwise
   */
  bool verifyPortFlowletConfig(
      std::unique_ptr<CIDRNetwork> prefix,
      std::unique_ptr<cfg::PortFlowletConfig> cfg,
      bool flowletEnable) override;

  bool validateFlowSetTable(const bool expectFlowsetSizeZero) override;

 private:
  HwSwitch* hwSwitch_;
};

std::shared_ptr<HwTestThriftHandler> createHwTestThriftHandler(HwSwitch* hw);
} // namespace facebook::fboss::utility
