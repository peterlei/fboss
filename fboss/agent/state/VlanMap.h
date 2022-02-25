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

#include <string>
#include "fboss/agent/gen-cpp2/switch_state_types.h"
#include "fboss/agent/state/NodeMap.h"
#include "fboss/agent/state/Thrifty.h"
#include "fboss/agent/types.h"

namespace facebook::fboss {

class SwitchState;
class Vlan;
typedef NodeMapTraits<
    VlanID,
    Vlan,
    NodeMapNoExtraFields,
    std::map<VlanID, std::shared_ptr<Vlan>>>
    VlanMapTraits;

struct VlanMapThriftTraits
    : public ThriftyNodeMapTraits<int16_t, state::VlanFields> {
  static inline const std::string& getThriftKeyName() {
    static const std::string _key = "vlanId";
    return _key;
  }
};

/*
 * A container for the set of VLANs.
 */
class VlanMap
    : public ThriftyNodeMapT<VlanMap, VlanMapTraits, VlanMapThriftTraits> {
 public:
  VlanMap();
  ~VlanMap() override;

  VlanMap* modify(std::shared_ptr<SwitchState>* state);

  /*
   * Get the specified Vlan.
   *
   * Throws an FbossError if the VLAN does not exist.
   */
  const std::shared_ptr<Vlan>& getVlan(VlanID id) const {
    return getNode(id);
  }

  /*
   * Get the specified Vlan by name. Throw FbossError if Vlan
   * does not exist.
   * Note that this requires a linear traversal of all Vlans
   *
   * TODO: Maintain a second index by name. Typically we have only a
   * handful of Vlans, so this traversal should be fairly cheap. We
   * may optimize it further in the future though.
   */
  const std::shared_ptr<Vlan>& getVlanSlow(const std::string& name) const;

  /*
   * Get the specified Vlan.
   *
   * Returns null if the VLAN does not exist.
   */
  std::shared_ptr<Vlan> getVlanIf(VlanID id) const {
    return getNodeIf(id);
  }

  /*
   * Get the specified Vlan by name. Returns null if Vlan
   * does not exist.
   * Note that this requires a linear traversal of all Vlans
   *
   * TODO: Maintain a second index by name. Typically we have only a
   * handful of Vlans, so this traversal should be fairly cheap. We
   * may optimize it further in the future though.
   */
  std::shared_ptr<Vlan> getVlanSlowIf(const std::string& name) const;

  /*
   * The following functions modify the static state.
   * These should only be called on unpublished objects which are only visible
   * to a single thread.
   */

  void addVlan(const std::shared_ptr<Vlan>& vlan);

  void updateVlan(const std::shared_ptr<Vlan>& vlan);

 private:
  // Inherit the constructors required for clone()
  using ThriftyNodeMapT::ThriftyNodeMapT;
  friend class CloneAllocator;
};

} // namespace facebook::fboss
