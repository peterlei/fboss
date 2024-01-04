/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "fboss/agent/ResourceAccountant.h"

DEFINE_int32(
    ecmp_resource_percentage,
    75,
    "Percentage of ECMP resources (out of 100) allowed to use before ResourceAccountant rejects the update.");

namespace {
constexpr auto kHundredPercentage = 100;
}

namespace facebook::fboss {

ResourceAccountant::ResourceAccountant(const HwAsicTable* asicTable)
    : asicTable_(asicTable) {
  CHECK_EQ(
      asicTable->isFeatureSupportedOnAnyAsic(
          HwAsic::Feature::WEIGHTED_NEXTHOPGROUP_MEMBER),
      asicTable->isFeatureSupportedOnAllAsic(
          HwAsic::Feature::WEIGHTED_NEXTHOPGROUP_MEMBER));
  nativeWeightedEcmp_ = asicTable->isFeatureSupportedOnAllAsic(
      HwAsic::Feature::WEIGHTED_NEXTHOPGROUP_MEMBER);
}

int ResourceAccountant::getMemberCountForEcmpGroup(
    const RouteNextHopEntry& fwd) const {
  if (nativeWeightedEcmp_) {
    // TODO: Compute different table usage for different ASICs (e.g. TH4)
    return fwd.getNextHopSet().size();
  }
  auto totalWeight = 0;
  for (const auto& nhop : fwd.normalizedNextHops()) {
    totalWeight += nhop.weight() ? nhop.weight() : 1;
  }
  return totalWeight;
}

bool ResourceAccountant::checkEcmpResource(bool intermediateState) const {
  // There are two checks needed for ECMP resource:
  // 1) Post each route add/update, check if intermediate state exceeds HW
  // limit. 2) Post entire state update, check if total usage is lower than
  // ecmp_resource_percentage.
  uint32_t resourcePercentage =
      intermediateState ? kHundredPercentage : FLAGS_ecmp_resource_percentage;

  for (const auto& [_, hwAsic] : asicTable_->getHwAsics()) {
    const auto ecmpGroupLimit = hwAsic->getMaxEcmpGroups();
    const auto ecmpMemberLimit = hwAsic->getMaxEcmpMembers();
    if ((ecmpGroupLimit.has_value() &&
         ecmpGroupRefMap_.size() >
             (ecmpGroupLimit.value() * resourcePercentage) /
                 kHundredPercentage) ||
        (ecmpMemberLimit.has_value() &&
         ecmpMemberUsage_ > (ecmpMemberLimit.value() * resourcePercentage) /
                 kHundredPercentage)) {
      return false;
    }
  }
  return true;
}

template <typename AddrT>
bool ResourceAccountant::checkAndUpdateEcmpResource(
    const std::shared_ptr<Route<AddrT>>& route,
    bool add) {
  const auto& fwd = route->getForwardInfo();

  // Forwarding to nextHops and more than one nextHop - use ECMP
  if (fwd.getAction() == RouteForwardAction::NEXTHOPS &&
      fwd.getNextHopSet().size() > 1) {
    const auto& nhSet = fwd.getNextHopSet();
    if (auto it = ecmpGroupRefMap_.find(nhSet); it != ecmpGroupRefMap_.end()) {
      it->second = it->second + (add ? 1 : -1);
      CHECK(it->second >= 0);
      if (!add && it->second == 0) {
        ecmpGroupRefMap_.erase(it);
        ecmpMemberUsage_ -= getMemberCountForEcmpGroup(fwd);
      }
      return true;
    }
    // ECMP group does not exists in hw - Check if any usage exceeds ASIC limit
    CHECK(add);
    ecmpGroupRefMap_[nhSet] = 1;
    ecmpMemberUsage_ += getMemberCountForEcmpGroup(fwd);
    return checkEcmpResource(true /* intermediateState */);
  }
  return true;
}

template bool
ResourceAccountant::checkAndUpdateEcmpResource<folly::IPAddressV6>(
    const std::shared_ptr<Route<folly::IPAddressV6>>& route,
    bool add);

template bool
ResourceAccountant::checkAndUpdateEcmpResource<folly::IPAddressV4>(
    const std::shared_ptr<Route<folly::IPAddressV4>>& route,
    bool add);

} // namespace facebook::fboss
