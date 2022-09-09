/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "fboss/agent/hw/sai/switch/SaiQueueManager.h"
#include "fboss/agent/platforms/sai/SaiPlatform.h"

#include "fboss/agent/hw/sai/store/SaiStore.h"
#include "fboss/agent/hw/sai/switch/SaiManagerTable.h"
#include "fboss/agent/hw/sai/switch/SaiPortManager.h"
#include "fboss/agent/hw/sai/switch/SaiSwitchManager.h"
#include "fboss/agent/hw/switch_asics/HwAsic.h"
#include "fboss/lib/TupleUtils.h"

namespace facebook::fboss {

namespace {

void fillHwQueueStats(
    uint8_t queueId,
    const folly::F14FastMap<sai_stat_id_t, uint64_t>& counterId2Value,
    HwPortStats& hwPortStats) {
  for (auto counterIdAndValue : counterId2Value) {
    auto [counterId, value] = counterIdAndValue;
    switch (counterId) {
      case SAI_QUEUE_STAT_PACKETS:
        hwPortStats.queueOutPackets_()[queueId] = value;
        break;
      case SAI_QUEUE_STAT_BYTES:
        hwPortStats.queueOutBytes_()[queueId] = value;
        break;
      case SAI_QUEUE_STAT_DROPPED_BYTES:
        hwPortStats.queueOutDiscardBytes_()[queueId] = value;
        break;
      case SAI_QUEUE_STAT_DROPPED_PACKETS:
        hwPortStats.queueOutDiscardPackets_()[queueId] = value;
        /*
         * Out congestion packets on a port is a sum of all queue
         * out discards on a port
         */
        hwPortStats.outCongestionDiscardPkts_() =
            *hwPortStats.outCongestionDiscardPkts_() + value;
        break;
      case SAI_QUEUE_STAT_WATERMARK_BYTES:
        hwPortStats.queueWatermarkBytes_()[queueId] = value;
        break;
      case SAI_QUEUE_STAT_WRED_DROPPED_PACKETS:
        hwPortStats.queueWredDroppedPackets_()[queueId] = value;
        break;
      default:
        throw FbossError("Got unexpected queue counter id: ", counterId);
    }
  }
}
} // namespace

namespace detail {

SaiQueueTraits::CreateAttributes makeQueueAttributes(
    PortSaiId portSaiId,
    const PortQueue& portQueue) {
  sai_queue_type_t type;
  switch (portQueue.getStreamType()) {
    case cfg::StreamType::UNICAST:
      type = SAI_QUEUE_TYPE_UNICAST;
      break;
    case cfg::StreamType::MULTICAST:
      type = SAI_QUEUE_TYPE_MULTICAST;
      break;
    case cfg::StreamType::ALL:
      type = SAI_QUEUE_TYPE_ALL;
      break;
    case cfg::StreamType::FABRIC_TX:
      type = SAI_QUEUE_TYPE_FABRIC_TX;
      break;
  }
  return SaiQueueTraits::CreateAttributes{
      type,
      portSaiId,
      portQueue.getID(),
      portSaiId,
      std::nullopt,
      std::nullopt,
      std::nullopt};
}

SaiQueueConfig makeSaiQueueConfig(
    const SaiQueueTraits::AdapterHostKey& adapterHostKey) {
  auto queueIndex = std::get<SaiQueueTraits::Attributes::Index>(adapterHostKey);
  auto queueType = std::get<SaiQueueTraits::Attributes::Type>(adapterHostKey);
  cfg::StreamType streamType;
  switch (queueType.value()) {
    case SAI_QUEUE_TYPE_UNICAST:
      streamType = cfg::StreamType::UNICAST;
      break;
    case SAI_QUEUE_TYPE_MULTICAST:
      streamType = cfg::StreamType::MULTICAST;
      break;
    case SAI_QUEUE_TYPE_ALL:
      streamType = cfg::StreamType::ALL;
      break;
    case SAI_QUEUE_TYPE_FABRIC_TX:
      streamType = cfg::StreamType::FABRIC_TX;
      break;
    default:
      throw FbossError("Unhandled SAI queue type: ", queueType.value());
  }
  return std::make_pair(queueIndex.value(), streamType);
}
} // namespace detail

void SaiQueueHandle::resetQueue() {
  /*
   * Queues cannot be deleted as it is owned by the adapter but all
   * the queue attributes has to be reset to default. As a
   * temporary solution, resetting the queue attributes to their
   * defaults. For long term, this will be removed and resetting the
   * objects will be part of SaiObject.
   */

  queue->setOptionalAttribute(
      SaiQueueTraits::Attributes::SchedulerProfileId{SAI_NULL_OBJECT_ID});
  if (wredProfile) {
    queue->setOptionalAttribute(
        SaiQueueTraits::Attributes::WredProfileId{SAI_NULL_OBJECT_ID});
  }
  if (bufferProfile) {
    queue->setOptionalAttribute(
        SaiQueueTraits::Attributes::BufferProfileId{SAI_NULL_OBJECT_ID});
  }
}

SaiQueueHandle::~SaiQueueHandle() {
  resetQueue();
}

SaiQueueManager::SaiQueueManager(
    SaiStore* saiStore,
    SaiManagerTable* managerTable,
    const SaiPlatform* platform)
    : saiStore_(saiStore), managerTable_(managerTable), platform_(platform) {}

void SaiQueueManager::changeQueue(
    SaiQueueHandle* queueHandle,
    const PortQueue& newPortQueue) {
  CHECK(queueHandle);
  std::shared_ptr<SaiScheduler> newScheduler;
  if (newPortQueue.getScheduling() != cfg::QueueScheduling::INTERNAL) {
    newScheduler =
        managerTable_->schedulerManager().createScheduler(newPortQueue);
  }
  if (newScheduler != queueHandle->scheduler) {
    queueHandle->queue->setOptionalAttribute(
        SaiQueueTraits::Attributes::SchedulerProfileId(
            newScheduler ? newScheduler->adapterKey() : SAI_NULL_OBJECT_ID));
    // Update scheduler reference after we have set the queue
    // scheduler attribute, else if this is the last queue
    // referring to this scheduler, we will try to delete it
    // before we have updated the SAI reference.
    queueHandle->scheduler = newScheduler;
  }
  if (platform_->getAsic()->isSupported(HwAsic::Feature::SAI_ECN_WRED)) {
    auto newWredProfile =
        managerTable_->wredManager().getOrCreateProfile(newPortQueue);
    if (newWredProfile != queueHandle->wredProfile) {
      queueHandle->queue->setOptionalAttribute(
          SaiQueueTraits::Attributes::WredProfileId(
              newWredProfile ? newWredProfile->adapterKey()
                             : SAI_NULL_OBJECT_ID));
      queueHandle->wredProfile = newWredProfile;
    }
  }
  if (platform_->getAsic()->isSupported(HwAsic::Feature::BUFFER_POOL)) {
    auto newBufferProfile =
        managerTable_->bufferManager().getOrCreateProfile(newPortQueue);
    if (newBufferProfile != queueHandle->bufferProfile) {
      queueHandle->queue->setOptionalAttribute(
          SaiQueueTraits::Attributes::BufferProfileId(
              newBufferProfile ? newBufferProfile->adapterKey()
                               : SAI_NULL_OBJECT_ID));
      queueHandle->bufferProfile = newBufferProfile;
    }
  }
}

void SaiQueueManager::ensurePortQueueConfig(
    PortSaiId portSaiId,
    const SaiQueueHandles& queueHandles,
    const QueueConfig& queues) {
  for (const auto& portQueue : queues) {
    SaiQueueTraits::CreateAttributes attributes =
        detail::makeQueueAttributes(portSaiId, *portQueue);
    SaiQueueTraits::AdapterHostKey adapterHostKey = tupleProjection<
        SaiQueueTraits::CreateAttributes,
        SaiQueueTraits::AdapterHostKey>(attributes);
    auto& store = saiStore_->get<SaiQueueTraits>();
    auto queue = store.get(adapterHostKey);
    if (!queue) {
      throw FbossError(
          "failed to find queue in store for queue id: ", portQueue->getID());
    }
    SaiQueueConfig queueConfig =
        std::make_pair(portQueue->getID(), portQueue->getStreamType());
    auto queueHandleEntry = queueHandles.find(queueConfig);
    if (queueHandleEntry == queueHandles.end()) {
      throw FbossError(
          "failed to find queue handle for queue id: ", (*portQueue).getID());
    }
    changeQueue(queueHandleEntry->second.get(), *portQueue);
  }
}

SaiQueueHandles SaiQueueManager::loadQueues(
    PortSaiId portSaiId,
    const std::vector<QueueSaiId>& queueSaiIds) {
  SaiQueueHandles queueHandles;
  auto& store = saiStore_->get<SaiQueueTraits>();
  for (auto queueSaiId : queueSaiIds) {
    auto queueHandle = std::make_unique<SaiQueueHandle>(
        store.loadObjectOwnedByAdapter(queueSaiId));
    store.loadObjectOwnedByAdapter(SaiQueueTraits::AdapterKey{queueSaiId});
    auto saiQueueConfig =
        detail::makeSaiQueueConfig(queueHandle->queue->adapterHostKey());
    queueHandles[saiQueueConfig] = std::move(queueHandle);
  }
  return queueHandles;
}

const std::vector<sai_stat_id_t>&
SaiQueueManager::supportedNonWatermarkCounterIdsRead(int queueType) const {
  static std::vector<sai_stat_id_t> nonWredCounterIds(
      SaiQueueTraits::NonWatermarkCounterIdsToRead.begin(),
      SaiQueueTraits::NonWatermarkCounterIdsToRead.end());
  static std::vector<sai_stat_id_t> wredCounterIds{};
  if (wredCounterIds.size() == 0) {
    std::set_union(
        SaiQueueTraits::NonWatermarkCounterIdsToRead.begin(),
        SaiQueueTraits::NonWatermarkCounterIdsToRead.end(),
        SaiQueueTraits::NonWatermarkWredCounterIdsToRead.begin(),
        SaiQueueTraits::NonWatermarkWredCounterIdsToRead.end(),
        std::back_inserter(wredCounterIds));
  }

  /*
   * Per-queue WRED discard counters need to be fetched for platforms
   * supporting this feature and for non-multicast queues in Broadcom
   * platforms.
   */
  if (((queueType == SAI_QUEUE_TYPE_MULTICAST) &&
       (platform_->getAsic()->getAsicVendor() ==
        HwAsic::AsicVendor::ASIC_VENDOR_BCM)) ||
      !platform_->getAsic()->isSupported(HwAsic::Feature::SAI_ECN_WRED)) {
    return nonWredCounterIds;
  }

  return wredCounterIds;
}

void SaiQueueManager::updateStats(
    const std::vector<SaiQueueHandle*>& queueHandles,
    HwPortStats& hwPortStats,
    bool updateWatermarks) {
  hwPortStats.outCongestionDiscardPkts_() = 0;
  static std::vector<sai_stat_id_t> nonWatermarkStatsReadAndClear(
      SaiQueueTraits::NonWatermarkCounterIdsToReadAndClear.begin(),
      SaiQueueTraits::NonWatermarkCounterIdsToReadAndClear.end());
  static std::vector<sai_stat_id_t> watermarkStatsReadAndClear(
      SaiQueueTraits::WatermarkCounterIdsToReadAndClear.begin(),
      SaiQueueTraits::WatermarkCounterIdsToReadAndClear.end());
  for (auto queueHandle : queueHandles) {
    /*
     * The WRED_DROPPED_PACKETS counter is needed only for non-CPU
     * ports and on platform supporting ECN/WRED, which is taken
     * care of in the API supportedNonWatermarkCounterIdsRead().
     * Hence, not using queueHandle->queue->updateStats() directly.
     */
    auto queueType = SaiApiTable::getInstance()->queueApi().getAttribute(
        queueHandle->queue->adapterKey(), SaiQueueTraits::Attributes::Type{});
    queueHandle->queue->updateStats(
        supportedNonWatermarkCounterIdsRead(queueType), SAI_STATS_MODE_READ);
    queueHandle->queue->updateStats(
        nonWatermarkStatsReadAndClear, SAI_STATS_MODE_READ_AND_CLEAR);
    if (updateWatermarks) {
      queueHandle->queue->updateStats(
          watermarkStatsReadAndClear, SAI_STATS_MODE_READ_AND_CLEAR);
    }
    const auto& counters = queueHandle->queue->getStats();
    auto queueId = SaiApiTable::getInstance()->queueApi().getAttribute(
        queueHandle->queue->adapterKey(), SaiQueueTraits::Attributes::Index{});
    fillHwQueueStats(queueId, counters, hwPortStats);
  }
}

void SaiQueueManager::getStats(
    SaiQueueHandles& queueHandles,
    HwPortStats& hwPortStats) {
  for (auto& queueHandle : queueHandles) {
    const auto& counters = queueHandle.second->queue->getStats();
    fillHwQueueStats(queueHandle.first.first, counters, hwPortStats);
  }
}

QueueConfig SaiQueueManager::getQueueSettings(
    const SaiQueueHandles& queueHandles) const {
  QueueConfig queueConfig;
  for (auto& queueHandle : queueHandles) {
    auto portQueue = std::make_shared<PortQueue>(queueHandle.first.first);
    portQueue->setStreamType(queueHandle.first.second);
    managerTable_->schedulerManager().fillSchedulerSettings(
        queueHandle.second->scheduler.get(), portQueue.get());
    queueConfig.push_back(std::move(portQueue));
  }
  return queueConfig;
}

} // namespace facebook::fboss
