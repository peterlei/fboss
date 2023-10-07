/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "fboss/agent/hw/sai/switch/SaiRouterInterfaceManager.h"

#include "fboss/agent/FbossError.h"
#include "fboss/agent/hw/sai/store/SaiStore.h"
#include "fboss/agent/hw/sai/switch/SaiManagerTable.h"
#include "fboss/agent/hw/sai/switch/SaiSwitchManager.h"
#include "fboss/agent/hw/sai/switch/SaiSystemPortManager.h"
#include "fboss/agent/hw/sai/switch/SaiVirtualRouterManager.h"
#include "fboss/agent/hw/sai/switch/SaiVlanManager.h"

#include <folly/logging/xlog.h>

namespace facebook::fboss {

SaiRouterInterfaceManager::SaiRouterInterfaceManager(
    SaiStore* saiStore,
    SaiManagerTable* managerTable,
    const SaiPlatform* platform)
    : saiStore_(saiStore), managerTable_(managerTable), platform_(platform) {}

RouterInterfaceSaiId SaiRouterInterfaceManager::addOrUpdateVlanRouterInterface(
    const std::shared_ptr<Interface>& swInterface,
    bool isLocal) {
  CHECK(swInterface->getType() == cfg::InterfaceType::VLAN);
  // compute the virtual router id for this router interface
  RouterID routerId = swInterface->getRouterID();
  SaiVirtualRouterHandle* virtualRouterHandle =
      managerTable_->virtualRouterManager().getVirtualRouterHandle(routerId);
  if (!virtualRouterHandle) {
    throw FbossError("No virtual router for RouterID ", routerId);
  }
  VirtualRouterSaiId saiVirtualRouterId{
      virtualRouterHandle->virtualRouter->adapterKey()};
  SaiVlanRouterInterfaceTraits::Attributes::VirtualRouterId
      virtualRouterIdAttribute{saiVirtualRouterId};

  SaiVlanRouterInterfaceTraits::Attributes::Type typeAttribute{
      SAI_ROUTER_INTERFACE_TYPE_VLAN};

  // compute the VLAN sai id for this router interface
  VlanID swVlanId = swInterface->getVlanID();
  SaiVlanHandle* saiVlanHandle =
      managerTable_->vlanManager().getVlanHandle(swVlanId);
  if (!saiVlanHandle) {
    throw FbossError(
        "Failed to add router interface: no sai vlan for VlanID ", swVlanId);
  }
  SaiVlanRouterInterfaceTraits::Attributes::VlanId vlanIdAttribute{
      saiVlanHandle->vlan->adapterKey()};

  // get the src mac for this router interface
  folly::MacAddress srcMac = swInterface->getMac();
  SaiVlanRouterInterfaceTraits::Attributes::SrcMac srcMacAttribute{srcMac};

  // get MTU
  SaiVlanRouterInterfaceTraits::Attributes::Mtu mtuAttribute{
      static_cast<uint32_t>(swInterface->getMtu())};

  // create the router interface
  SaiVlanRouterInterfaceTraits::CreateAttributes attributes{
      virtualRouterIdAttribute,
      typeAttribute,
      vlanIdAttribute,
      srcMacAttribute,
      mtuAttribute};
  SaiVlanRouterInterfaceTraits::AdapterHostKey k{
      virtualRouterIdAttribute,
      vlanIdAttribute,
  };
  auto& store = saiStore_->get<SaiVlanRouterInterfaceTraits>();
  std::shared_ptr<SaiVlanRouterInterface> vlanRouterInterface =
      store.setObject(k, attributes, swInterface->getID());
  auto vlanRouterInterfaceHandle = std::make_unique<SaiRouterInterfaceHandle>();
  vlanRouterInterfaceHandle->routerInterface = vlanRouterInterface;
  vlanRouterInterfaceHandle->setLocal(isLocal);

  if (isLocal) {
    // create the ToMe routes for this (local) router interface
    auto toMeRoutes =
        managerTable_->routeManager().makeInterfaceToMeRoutes(swInterface);
    vlanRouterInterfaceHandle->toMeRoutes = std::move(toMeRoutes);
  }

  handles_[swInterface->getID()] = std::move(vlanRouterInterfaceHandle);
  return vlanRouterInterface->adapterKey();
}

RouterInterfaceSaiId SaiRouterInterfaceManager::addOrUpdatePortRouterInterface(
    const std::shared_ptr<Interface>& swInterface,
    bool isLocal) {
  CHECK(swInterface->getType() == cfg::InterfaceType::SYSTEM_PORT);
  // compute the virtual router id for this router interface
  RouterID routerId = swInterface->getRouterID();
  SaiVirtualRouterHandle* virtualRouterHandle =
      managerTable_->virtualRouterManager().getVirtualRouterHandle(routerId);
  if (!virtualRouterHandle) {
    throw FbossError("No virtual router for RouterID ", routerId);
  }
  VirtualRouterSaiId saiVirtualRouterId{
      virtualRouterHandle->virtualRouter->adapterKey()};
  SaiPortRouterInterfaceTraits::Attributes::VirtualRouterId
      virtualRouterIdAttribute{saiVirtualRouterId};

  SaiPortRouterInterfaceTraits::Attributes::Type typeAttribute{
      SAI_ROUTER_INTERFACE_TYPE_PORT};

  // compute the system port sai id for this router interface
  SystemPortID swSystemPortId{swInterface->getID()};
  SaiSystemPortHandle* saiSystemPortHandle =
      managerTable_->systemPortManager().getSystemPortHandle(swSystemPortId);
  if (!saiSystemPortHandle) {
    throw FbossError(
        "Failed to add router interface: no sai system port for ID ",
        swSystemPortId);
  }
  SaiPortRouterInterfaceTraits::Attributes::PortId portIdAttribute{
      saiSystemPortHandle->systemPort->adapterKey()};

  // get the src mac for this router interface
  folly::MacAddress srcMac = swInterface->getMac();
  SaiPortRouterInterfaceTraits::Attributes::SrcMac srcMacAttribute{srcMac};

  // get MTU
  SaiPortRouterInterfaceTraits::Attributes::Mtu mtuAttribute{
      static_cast<uint32_t>(swInterface->getMtu())};

  // create the router interface
  SaiPortRouterInterfaceTraits::CreateAttributes attributes{
      virtualRouterIdAttribute,
      typeAttribute,
      portIdAttribute,
      srcMacAttribute,
      mtuAttribute};
  SaiPortRouterInterfaceTraits::AdapterHostKey k{
      virtualRouterIdAttribute,
      portIdAttribute,
  };
  auto& store = saiStore_->get<SaiPortRouterInterfaceTraits>();
  std::shared_ptr<SaiPortRouterInterface> portRouterInterface =
      store.setObject(k, attributes, swInterface->getID());
  auto portRouterInterfaceHandle = std::make_unique<SaiRouterInterfaceHandle>();
  portRouterInterfaceHandle->routerInterface = portRouterInterface;
  portRouterInterfaceHandle->setLocal(isLocal);

  if (isLocal) {
    // create the ToMe routes for this (local) router interface
    auto toMeRoutes =
        managerTable_->routeManager().makeInterfaceToMeRoutes(swInterface);
    portRouterInterfaceHandle->toMeRoutes = std::move(toMeRoutes);
  }

  handles_[swInterface->getID()] = std::move(portRouterInterfaceHandle);
  return portRouterInterface->adapterKey();
}

void SaiRouterInterfaceManager::removeRouterInterface(
    const std::shared_ptr<Interface>& swInterface) {
  auto swId = swInterface->getID();
  auto itr = handles_.find(swId);
  if (itr == handles_.end()) {
    throw FbossError("Failed to remove non-existent router interface: ", swId);
  }
  handles_.erase(itr);
}

RouterInterfaceSaiId SaiRouterInterfaceManager::addOrUpdateRouterInterface(
    const std::shared_ptr<Interface>& swInterface,
    bool isLocal) {
  switch (swInterface->getType()) {
    case cfg::InterfaceType::VLAN:
      return addOrUpdateVlanRouterInterface(swInterface, isLocal);
    case cfg::InterfaceType::SYSTEM_PORT:
      return addOrUpdatePortRouterInterface(swInterface, isLocal);
  }
  // Should never get here
  throw FbossError("Unknown interface type: ", swInterface->getType());
}

RouterInterfaceSaiId SaiRouterInterfaceManager::addRouterInterface(
    const std::shared_ptr<Interface>& swInterface,
    bool isLocal) {
  // check if the router interface already exists
  InterfaceID swId(swInterface->getID());
  auto handle = getRouterInterfaceHandle(swId);
  if (handle) {
    throw FbossError(
        "Attempted to add duplicate router interface with InterfaceID ", swId);
  }
  return addOrUpdateRouterInterface(swInterface, isLocal);
}

void SaiRouterInterfaceManager::changeRouterInterface(
    const std::shared_ptr<Interface>& oldInterface,
    const std::shared_ptr<Interface>& newInterface,
    bool isLocal) {
  CHECK_EQ(oldInterface->getID(), newInterface->getID());
  InterfaceID swId(newInterface->getID());
  auto handle = getRouterInterfaceHandle(swId);
  if (!handle) {
    throw FbossError(
        "Attempted to change non existent router interface with InterfaceID ",
        swId);
  }
  addOrUpdateRouterInterface(newInterface, isLocal);
}

SaiRouterInterfaceHandle* SaiRouterInterfaceManager::getRouterInterfaceHandle(
    const InterfaceID& swId) {
  return getRouterInterfaceHandleImpl(swId);
}

const SaiRouterInterfaceHandle*
SaiRouterInterfaceManager::getRouterInterfaceHandle(
    const InterfaceID& swId) const {
  return getRouterInterfaceHandleImpl(swId);
}

SaiRouterInterfaceHandle*
SaiRouterInterfaceManager::getRouterInterfaceHandleImpl(
    const InterfaceID& swId) const {
  auto itr = handles_.find(swId);
  if (itr == handles_.end()) {
    return nullptr;
  }
  if (!itr->second) {
    XLOG(FATAL) << "Invalid null router interface for InterfaceID " << swId;
  }
  return itr->second.get();
}

} // namespace facebook::fboss
