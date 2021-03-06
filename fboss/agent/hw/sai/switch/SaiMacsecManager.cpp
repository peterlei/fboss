/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "fboss/agent/hw/sai/switch/SaiMacsecManager.h"
#include "fboss/agent/hw/sai/switch/SaiManagerTable.h"
#include "fboss/agent/hw/sai/switch/SaiPortManager.h"

#include <folly/logging/xlog.h>
#include "fboss/agent/FbossError.h"

extern "C" {
#include <sai.h>
}

namespace {
constexpr sai_uint8_t kSectagOffset = 12;
constexpr sai_int32_t kReplayProtectionWindow = 100;
} // namespace

namespace facebook::fboss {

SaiMacsecManager::SaiMacsecManager(
    SaiStore* saiStore,
    SaiManagerTable* managerTable)
    : saiStore_(saiStore), managerTable_(managerTable) {}

SaiMacsecManager::~SaiMacsecManager() {}

MacsecSaiId SaiMacsecManager::addMacsec(
    sai_macsec_direction_t direction,
    bool physicalBypassEnable) {
  auto handle = getMacsecHandle(direction);
  if (handle) {
    throw FbossError(
        "Attempted to add macsec for direction that already has a macsec pipeline: ",
        direction,
        " SAI id: ",
        handle->macsec->adapterKey());
  }

  SaiMacsecTraits::CreateAttributes attributes = {
      direction, physicalBypassEnable};
  SaiMacsecTraits::AdapterHostKey key{direction};

  auto& macsecStore = saiStore_->get<SaiMacsecTraits>();
  auto saiMacsecObj = macsecStore.setObject(key, attributes);
  auto macsecHandle = std::make_unique<SaiMacsecHandle>();
  macsecHandle->macsec = std::move(saiMacsecObj);
  macsecHandles_.emplace(direction, std::move(macsecHandle));
  return macsecHandles_[direction]->macsec->adapterKey();
}

void SaiMacsecManager::removeMacsec(sai_macsec_direction_t direction) {
  auto itr = macsecHandles_.find(direction);
  if (itr == macsecHandles_.end()) {
    throw FbossError(
        "Attempted to remove non-existent macsec pipeline for direction: ",
        direction);
  }
  macsecHandles_.erase(itr);
  XLOG(INFO) << "removed macsec pipeline for direction " << direction;
}

const SaiMacsecHandle* FOLLY_NULLABLE
SaiMacsecManager::getMacsecHandle(sai_macsec_direction_t direction) const {
  return getMacsecHandleImpl(direction);
}
SaiMacsecHandle* FOLLY_NULLABLE
SaiMacsecManager::getMacsecHandle(sai_macsec_direction_t direction) {
  return getMacsecHandleImpl(direction);
}

SaiMacsecHandle* FOLLY_NULLABLE
SaiMacsecManager::getMacsecHandleImpl(sai_macsec_direction_t direction) const {
  auto itr = macsecHandles_.find(direction);
  if (itr == macsecHandles_.end()) {
    return nullptr;
  }
  if (!itr->second.get()) {
    XLOG(FATAL) << "Invalid null SaiMacsecHandle for direction " << direction;
  }
  return itr->second.get();
}

MacsecFlowSaiId SaiMacsecManager::addMacsecFlow(
    sai_macsec_direction_t direction) {
  auto flow = getMacsecFlow(direction);
  if (flow) {
    throw FbossError(
        "Attempted to add macsecFlow for direction that already exists: ",
        direction,
        " SAI id: ",
        flow->adapterKey());
  }

  auto macsecHandle = getMacsecHandle(direction);
  if (!macsecHandle) {
    throw FbossError(
        "Attempted to add macsecFlow for direction that has no macsec pipeline obj: ",
        direction);
  }

  SaiMacsecFlowTraits::CreateAttributes attributes = {
      direction,
  };
  SaiMacsecFlowTraits::AdapterHostKey key{direction};

  auto& store = saiStore_->get<SaiMacsecFlowTraits>();
  auto secureChannelObject = store.setObject(key, attributes);

  macsecHandle->flow = std::move(secureChannelObject);
  return macsecHandle->flow->adapterKey();
}

const SaiMacsecFlow* SaiMacsecManager::getMacsecFlow(
    sai_macsec_direction_t direction) const {
  return getMacsecFlowImpl(direction);
}
SaiMacsecFlow* SaiMacsecManager::getMacsecFlow(
    sai_macsec_direction_t direction) {
  return getMacsecFlowImpl(direction);
}

void SaiMacsecManager::removeMacsecFlow(sai_macsec_direction_t direction) {
  auto macsecHandle = getMacsecHandle(direction);
  if (!macsecHandle) {
    throw FbossError(
        "Attempted to remove macsecFlow for direction that has no macsec pipeline obj: ",
        direction);
  }

  if (!macsecHandle->flow) {
    throw FbossError(
        "Attempted to remove non-existent macsec flow for direction: ",
        direction);
  }
  macsecHandle->flow.reset();
  XLOG(INFO) << "removed macsec Flow for direction: " << direction;
}

SaiMacsecFlow* SaiMacsecManager::getMacsecFlowImpl(
    sai_macsec_direction_t direction) const {
  auto macsecHandle = getMacsecHandle(direction);
  if (!macsecHandle) {
    throw FbossError(
        "Attempted to get macsecFlow for direction that has no macsec pipeline obj: ",
        direction);
  }
  return macsecHandle->flow.get();
}

MacsecPortSaiId SaiMacsecManager::addMacsecPort(
    PortID linePort,
    sai_macsec_direction_t direction) {
  auto macsecPort = getMacsecPortHandle(linePort, direction);
  if (macsecPort) {
    throw FbossError(
        "Attempted to add macsecPort for port/direction that already exists: ",
        linePort,
        ",",
        direction,
        " SAI id: ",
        macsecPort->port->adapterKey());
  }
  auto macsecHandle = getMacsecHandle(direction);
  auto portHandle = managerTable_->portManager().getPortHandle(linePort);
  if (!portHandle) {
    throw FbossError(
        "Attempted to add macsecPort for linePort that doesn't exist: ",
        linePort);
  }

  SaiMacsecPortTraits::CreateAttributes attributes{
      portHandle->port->adapterKey(), direction};
  SaiMacsecPortTraits::AdapterHostKey key{linePort, direction};

  auto& store = saiStore_->get<SaiMacsecPortTraits>();
  auto saiObj = store.setObject(key, attributes);
  auto macsecPortHandle = std::make_unique<SaiMacsecPortHandle>();
  macsecPortHandle->port = std::move(saiObj);

  macsecHandle->ports.emplace(linePort, std::move(macsecPortHandle));
  return macsecHandle->ports[linePort]->port->adapterKey();
}

const SaiMacsecPortHandle* FOLLY_NULLABLE SaiMacsecManager::getMacsecPortHandle(
    PortID linePort,
    sai_macsec_direction_t direction) const {
  return getMacsecPortHandleImpl(linePort, direction);
}
SaiMacsecPortHandle* FOLLY_NULLABLE SaiMacsecManager::getMacsecPortHandle(
    PortID linePort,
    sai_macsec_direction_t direction) {
  return getMacsecPortHandleImpl(linePort, direction);
}

void SaiMacsecManager::removeMacsecPort(
    PortID linePort,
    sai_macsec_direction_t direction) {
  auto macsecHandle = getMacsecHandle(direction);
  if (!macsecHandle) {
    throw FbossError(
        "Attempted to remove macsecPort for direction that has no macsec pipeline obj ",
        direction);
  }
  auto itr = macsecHandle->ports.find(linePort);
  if (itr == macsecHandle->ports.end()) {
    throw FbossError(
        "Attempted to remove non-existent macsec port for lineport, direction: ",
        linePort,
        ", ",
        direction);
  }
  macsecHandle->ports.erase(itr);
  XLOG(INFO) << "removed macsec port for lineport, direction: " << linePort
             << ", " << direction;
}

SaiMacsecPortHandle* FOLLY_NULLABLE SaiMacsecManager::getMacsecPortHandleImpl(
    PortID linePort,
    sai_macsec_direction_t direction) const {
  auto macsecHandle = getMacsecHandle(direction);
  if (!macsecHandle) {
    throw FbossError(
        "Attempted to get macsecPort for direction that has no macsec pipeline obj ",
        direction);
  }

  auto itr = macsecHandle->ports.find(linePort);
  if (itr == macsecHandle->ports.end()) {
    return nullptr;
  }

  CHECK(itr->second.get())
      << "Invalid null SaiMacsecPortHandle for linePort, direction: "
      << linePort << ", " << direction;

  return itr->second.get();
}

MacsecSCSaiId SaiMacsecManager::addMacsecSecureChannel(
    PortID linePort,
    sai_macsec_direction_t direction,
    MacsecFlowSaiId flowId,
    MacsecSecureChannelId secureChannelId,
    bool xpn64Enable) {
  auto handle =
      getMacsecSecureChannelHandle(linePort, secureChannelId, direction);
  if (handle) {
    throw FbossError(
        "Attempted to add macsecSC for secureChannelId:direction that already exists: ",
        secureChannelId,
        ":",
        direction,
        " SAI id: ",
        handle->secureChannel->adapterKey());
  }

  auto macsecPort = getMacsecPortHandle(linePort, direction);
  if (!macsecPort) {
    throw FbossError(
        "Attempted to add macsecSC for linePort that doesn't exist: ",
        linePort);
  }

  std::optional<bool> secureChannelEnable;
  std::optional<bool> replayProtectionEnable;
  std::optional<sai_int32_t> replayProtectionWindow;

  if (direction == SAI_MACSEC_DIRECTION_EGRESS) {
    secureChannelEnable = true;
  }
  if (direction == SAI_MACSEC_DIRECTION_INGRESS) {
    replayProtectionEnable = true;
    replayProtectionWindow = kReplayProtectionWindow;
  }

  SaiMacsecSCTraits::CreateAttributes attributes {
    secureChannelId, direction, flowId,
#if SAI_API_VERSION >= SAI_VERSION(1, 7, 1)
        xpn64Enable ? SAI_MACSEC_CIPHER_SUITE_GCM_AES_XPN_256
                    : SAI_MACSEC_CIPHER_SUITE_GCM_AES_256, /* ciphersuite */
#endif
        secureChannelEnable, replayProtectionEnable, replayProtectionWindow,
        kSectagOffset
  };
  SaiMacsecSCTraits::AdapterHostKey secureChannelKey{
      secureChannelId, direction};

  auto& store = saiStore_->get<SaiMacsecSCTraits>();
  auto saiObj = store.setObject(secureChannelKey, attributes);
  auto scHandle = std::make_unique<SaiMacsecSecureChannelHandle>();
  scHandle->secureChannel = std::move(saiObj);
  macsecPort->secureChannels.emplace(secureChannelId, std::move(scHandle));
  return macsecPort->secureChannels[secureChannelId]
      ->secureChannel->adapterKey();
}

const SaiMacsecSecureChannelHandle* FOLLY_NULLABLE
SaiMacsecManager::getMacsecSecureChannelHandle(
    PortID linePort,
    MacsecSecureChannelId secureChannelId,
    sai_macsec_direction_t direction) const {
  return getMacsecSecureChannelHandleImpl(linePort, secureChannelId, direction);
}
SaiMacsecSecureChannelHandle* FOLLY_NULLABLE
SaiMacsecManager::getMacsecSecureChannelHandle(
    PortID linePort,
    MacsecSecureChannelId secureChannelId,
    sai_macsec_direction_t direction) {
  return getMacsecSecureChannelHandleImpl(linePort, secureChannelId, direction);
}

void SaiMacsecManager::removeMacsecSecureChannel(
    PortID linePort,
    MacsecSecureChannelId secureChannelId,
    sai_macsec_direction_t direction) {
  auto portHandle = getMacsecPortHandle(linePort, direction);
  if (!portHandle) {
    throw FbossError(
        "Attempted to remove SC for non-existent linePort: ", linePort);
  }
  auto itr = portHandle->secureChannels.find(secureChannelId);
  if (itr == portHandle->secureChannels.end()) {
    throw FbossError(
        "Attempted to remove non-existent macsec SC for linePort:secureChannelId:direction: ",
        linePort,
        ":",
        secureChannelId,
        ":",
        direction);
  }
  portHandle->secureChannels.erase(itr);
  XLOG(INFO) << "removed macsec SC for linePort:secureChannelId:direction: "
             << linePort << ":" << secureChannelId << ":" << direction;
}

SaiMacsecSecureChannelHandle* FOLLY_NULLABLE
SaiMacsecManager::getMacsecSecureChannelHandleImpl(
    PortID linePort,
    MacsecSecureChannelId secureChannelId,
    sai_macsec_direction_t direction) const {
  auto portHandle = getMacsecPortHandle(linePort, direction);
  if (!portHandle) {
    return nullptr;
  }
  auto itr = portHandle->secureChannels.find(secureChannelId);
  if (itr == portHandle->secureChannels.end()) {
    return nullptr;
  }
  if (!itr->second.get()) {
    XLOG(FATAL)
        << "Invalid null SaiMacsecSCHandle for secureChannelId:direction: "
        << secureChannelId << ":" << direction;
  }
  return itr->second.get();
}

} // namespace facebook::fboss
