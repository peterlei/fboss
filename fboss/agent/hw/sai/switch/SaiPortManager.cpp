/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "fboss/agent/hw/sai/switch/SaiPortManager.h"

#include "fboss/agent/FbossError.h"
#include "fboss/agent/hw/CounterUtils.h"
#include "fboss/agent/hw/HwPortFb303Stats.h"
#include "fboss/agent/hw/StatsConstants.h"
#include "fboss/agent/hw/gen-cpp2/hardware_stats_constants.h"
#include "fboss/agent/hw/sai/store/SaiStore.h"
#include "fboss/agent/hw/sai/switch/ConcurrentIndices.h"
#include "fboss/agent/hw/sai/switch/SaiBridgeManager.h"
#include "fboss/agent/hw/sai/switch/SaiDebugCounterManager.h"
#include "fboss/agent/hw/sai/switch/SaiMacsecManager.h"
#include "fboss/agent/hw/sai/switch/SaiManagerTable.h"
#include "fboss/agent/hw/sai/switch/SaiPortUtils.h"
#include "fboss/agent/hw/sai/switch/SaiQueueManager.h"
#include "fboss/agent/hw/sai/switch/SaiSwitch.h"
#include "fboss/agent/hw/sai/switch/SaiSwitchManager.h"
#include "fboss/agent/hw/switch_asics/HwAsic.h"
#include "fboss/agent/platforms/sai/SaiPlatform.h"

#include "fboss/lib/phy/PhyUtils.h"
#include "fboss/lib/phy/gen-cpp2/phy_types.h"
#include "fboss/qsfp_service/if/gen-cpp2/transceiver_types.h"

#include <folly/logging/xlog.h>

#include <chrono>

#include <fmt/ranges.h>

#if defined(BRCM_SAI_SDK_DNX)
#ifndef IS_OSS_BRCM_SAI
#include <experimental/saiportextensions.h>
#else
#include <saiportextensions.h>
#endif
#endif

DEFINE_bool(
    sai_configure_six_tap,
    false,
    "Flag to indicate whether to program six tap attributes in sai");

DEFINE_int32(
    fec_counters_update_interval_s,
    10,
    "Interval in seconds for reading fec counters");

using namespace std::chrono;

namespace facebook::fboss {
namespace {

void setUninitializedStatsToZero(long& counter) {
  counter =
      counter == hardware_stats_constants::STAT_UNINITIALIZED() ? 0 : counter;
}

uint16_t getPriorityFromPfcPktCounterId(sai_stat_id_t counterId) {
  switch (counterId) {
    case SAI_PORT_STAT_PFC_0_RX_PKTS:
    case SAI_PORT_STAT_PFC_0_TX_PKTS:
    case SAI_PORT_STAT_PFC_0_ON2OFF_RX_PKTS:
      return 0;
    case SAI_PORT_STAT_PFC_1_RX_PKTS:
    case SAI_PORT_STAT_PFC_1_TX_PKTS:
    case SAI_PORT_STAT_PFC_1_ON2OFF_RX_PKTS:
      return 1;
    case SAI_PORT_STAT_PFC_2_RX_PKTS:
    case SAI_PORT_STAT_PFC_2_TX_PKTS:
    case SAI_PORT_STAT_PFC_2_ON2OFF_RX_PKTS:
      return 2;
    case SAI_PORT_STAT_PFC_3_RX_PKTS:
    case SAI_PORT_STAT_PFC_3_TX_PKTS:
    case SAI_PORT_STAT_PFC_3_ON2OFF_RX_PKTS:
      return 3;
    case SAI_PORT_STAT_PFC_4_RX_PKTS:
    case SAI_PORT_STAT_PFC_4_TX_PKTS:
    case SAI_PORT_STAT_PFC_4_ON2OFF_RX_PKTS:
      return 4;
    case SAI_PORT_STAT_PFC_5_RX_PKTS:
    case SAI_PORT_STAT_PFC_5_TX_PKTS:
    case SAI_PORT_STAT_PFC_5_ON2OFF_RX_PKTS:
      return 5;
    case SAI_PORT_STAT_PFC_6_RX_PKTS:
    case SAI_PORT_STAT_PFC_6_TX_PKTS:
    case SAI_PORT_STAT_PFC_6_ON2OFF_RX_PKTS:
      return 6;
    case SAI_PORT_STAT_PFC_7_RX_PKTS:
    case SAI_PORT_STAT_PFC_7_TX_PKTS:
    case SAI_PORT_STAT_PFC_7_ON2OFF_RX_PKTS:
      return 7;
    default:
      break;
  }
  throw FbossError("Got unexpected port counter id: ", counterId);
}

#if SAI_API_VERSION >= SAI_VERSION(1, 11, 0)
uint16_t getFecSymbolCountFromCounterId(sai_stat_id_t counterId) {
  if (counterId < SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S0 ||
      counterId > SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S16) {
    throw FbossError("Got unexpected FEC codeword counter id: ", counterId);
  }
  return counterId - SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S0;
}
#endif

void fillHwPortStats(
    const folly::F14FastMap<sai_stat_id_t, uint64_t>& counterId2Value,
    const SaiDebugCounterManager& debugCounterManager,
    HwPortStats& hwPortStats,
    const SaiPlatform* platform,
    const cfg::PortType& portType,
    bool updateFecStats) {
  // TODO fill these in when we have debug counter support in SAI
  hwPortStats.inDstNullDiscards_() = 0;
  bool isEtherStatsSupported =
      platform->getAsic()->isSupported(HwAsic::Feature::SAI_PORT_ETHER_STATS);
  auto updateInUcastPkts = [&hwPortStats, &portType](uint64_t value) {
    if (portType == cfg::PortType::RECYCLE_PORT ||
        portType == cfg::PortType::EVENTOR_PORT) {
      // RECYCLE/EVENTOR port ucast pkts is clear on read on all
      // platforms that have rcy ports
      setUninitializedStatsToZero(*hwPortStats.inUnicastPkts_());
      hwPortStats.inUnicastPkts_() = *hwPortStats.inUnicastPkts_() + value;
    } else {
      hwPortStats.inUnicastPkts_() = value;
    }
  };
  auto updateOutUcastPkts = [&hwPortStats, &portType](uint64_t value) {
    if (portType == cfg::PortType::EVENTOR_PORT) {
      // EVENTOR port ucast pkts is clear on read
      setUninitializedStatsToZero(*hwPortStats.outUnicastPkts_());
      hwPortStats.outUnicastPkts_() = *hwPortStats.outUnicastPkts_() + value;
    } else {
      hwPortStats.outUnicastPkts_() = value;
    }
  };
  for (auto counterIdAndValue : counterId2Value) {
    auto [counterId, value] = counterIdAndValue;
    switch (counterId) {
      case SAI_PORT_STAT_IF_IN_OCTETS:
        hwPortStats.inBytes_() = value;
        break;
      case SAI_PORT_STAT_IF_IN_UCAST_PKTS:
        if (!isEtherStatsSupported) {
          // when ether stats is supported, skip updating as ether counterpart
          // will populate these stats
          updateInUcastPkts(value);
        }
        break;
      case SAI_PORT_STAT_ETHER_STATS_RX_NO_ERRORS:
        if (isEtherStatsSupported) {
          updateInUcastPkts(value);
        }
        break;
      case SAI_PORT_STAT_IF_IN_MULTICAST_PKTS:
        hwPortStats.inMulticastPkts_() = value;
        break;
      case SAI_PORT_STAT_IF_IN_BROADCAST_PKTS:
        hwPortStats.inBroadcastPkts_() = value;
        break;
      case SAI_PORT_STAT_IF_IN_DISCARDS:
        // Fill into inDiscards raw, we will then compute
        // inDiscards by subtracting dst null and in pause
        // discards from these
        hwPortStats.inDiscardsRaw_() = value;
        break;
      case SAI_PORT_STAT_IF_IN_ERRORS:
        hwPortStats.inErrors_() = value;
        break;
      case SAI_PORT_STAT_PAUSE_RX_PKTS:
        hwPortStats.inPause_() = value;
        break;
      case SAI_PORT_STAT_IF_OUT_OCTETS:
        hwPortStats.outBytes_() = value;
        break;
      case SAI_PORT_STAT_IF_OUT_UCAST_PKTS:
        if (!isEtherStatsSupported) {
          // when port ether stats is supported, skip updating as ether
          // counterpart stats will populate them
          updateOutUcastPkts(value);
        }
        break;
      case SAI_PORT_STAT_ETHER_STATS_TX_NO_ERRORS:
        if (isEtherStatsSupported) {
          // when port ether stats is supported, update
          updateOutUcastPkts(value);
        }
        break;
      case SAI_PORT_STAT_IF_OUT_MULTICAST_PKTS:
        hwPortStats.outMulticastPkts_() = value;
        break;
      case SAI_PORT_STAT_IF_OUT_BROADCAST_PKTS:
        hwPortStats.outBroadcastPkts_() = value;
        break;
      case SAI_PORT_STAT_IF_OUT_DISCARDS:
        hwPortStats.outDiscards_() = value;
        break;
      case SAI_PORT_STAT_IF_OUT_ERRORS:
        hwPortStats.outErrors_() = value;
        break;
      case SAI_PORT_STAT_PAUSE_TX_PKTS:
        hwPortStats.outPause_() = value;
        break;
      case SAI_PORT_STAT_ECN_MARKED_PACKETS:
        hwPortStats.outEcnCounter_() = value;
        break;
      case SAI_PORT_STAT_WRED_DROPPED_PACKETS:
        hwPortStats.wredDroppedPackets_() = value;
        break;
      case SAI_PORT_STAT_IN_DROPPED_PKTS:
        hwPortStats.inCongestionDiscards_() = value;
        break;
      case SAI_PORT_STAT_IF_IN_FEC_CORRECTABLE_FRAMES:
        if (updateFecStats) {
          // SDK provides clear-on-read counter but we store it as a monotonic
          // counter
          hwPortStats.fecCorrectableErrors() =
              *hwPortStats.fecCorrectableErrors() + value;
        }
        break;
      case SAI_PORT_STAT_IF_IN_FEC_NOT_CORRECTABLE_FRAMES:
        if (updateFecStats) {
          // SDK provides clear-on-read counter but we store it as a monotonic
          // counter
          hwPortStats.fecUncorrectableErrors() =
              *hwPortStats.fecUncorrectableErrors() + value;
        }
        break;
#if SAI_API_VERSION >= SAI_VERSION(1, 13, 0)
      case SAI_PORT_STAT_IF_IN_FEC_CORRECTED_BITS:
        if (updateFecStats) {
          hwPortStats.fecCorrectedBits_() = value;
        }
        break;
#endif
      case SAI_PORT_STAT_PFC_0_RX_PKTS:
      case SAI_PORT_STAT_PFC_1_RX_PKTS:
      case SAI_PORT_STAT_PFC_2_RX_PKTS:
      case SAI_PORT_STAT_PFC_3_RX_PKTS:
      case SAI_PORT_STAT_PFC_4_RX_PKTS:
      case SAI_PORT_STAT_PFC_5_RX_PKTS:
      case SAI_PORT_STAT_PFC_6_RX_PKTS:
      case SAI_PORT_STAT_PFC_7_RX_PKTS: {
        auto priority = getPriorityFromPfcPktCounterId(counterId);
        hwPortStats.inPfc_()[priority] = value;
        break;
      }
      case SAI_PORT_STAT_PFC_0_TX_PKTS:
      case SAI_PORT_STAT_PFC_1_TX_PKTS:
      case SAI_PORT_STAT_PFC_2_TX_PKTS:
      case SAI_PORT_STAT_PFC_3_TX_PKTS:
      case SAI_PORT_STAT_PFC_4_TX_PKTS:
      case SAI_PORT_STAT_PFC_5_TX_PKTS:
      case SAI_PORT_STAT_PFC_6_TX_PKTS:
      case SAI_PORT_STAT_PFC_7_TX_PKTS: {
        auto priority = getPriorityFromPfcPktCounterId(counterId);
        hwPortStats.outPfc_()[priority] = value;
        break;
      }
      case SAI_PORT_STAT_PFC_0_ON2OFF_RX_PKTS:
      case SAI_PORT_STAT_PFC_1_ON2OFF_RX_PKTS:
      case SAI_PORT_STAT_PFC_2_ON2OFF_RX_PKTS:
      case SAI_PORT_STAT_PFC_3_ON2OFF_RX_PKTS:
      case SAI_PORT_STAT_PFC_4_ON2OFF_RX_PKTS:
      case SAI_PORT_STAT_PFC_5_ON2OFF_RX_PKTS:
      case SAI_PORT_STAT_PFC_6_ON2OFF_RX_PKTS:
      case SAI_PORT_STAT_PFC_7_ON2OFF_RX_PKTS: {
        auto priority = getPriorityFromPfcPktCounterId(counterId);
        hwPortStats.inPfcXon_()[priority] = value;
        break;
      }
#if SAI_API_VERSION >= SAI_VERSION(1, 11, 0)
      case SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S0:
      case SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S1:
      case SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S2:
      case SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S3:
      case SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S4:
      case SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S5:
      case SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S6:
      case SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S7:
      case SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S8:
      case SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S9:
      case SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S10:
      case SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S11:
      case SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S12:
      case SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S13:
      case SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S14:
      case SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S15:
      case SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S16: {
        if (updateFecStats) {
          auto symbolCount = getFecSymbolCountFromCounterId(counterId);
          hwPortStats.fecCodewords_()[symbolCount] = value;
        }
        break;
      }
#endif
      case SAI_PORT_STAT_OUT_CONFIGURED_DROP_REASONS_0_DROPPED_PKTS:
        hwPortStats.pqpErrorEgressDroppedPackets_() = value;
        break;
#if defined(BRCM_SAI_SDK_DNX_GTE_11_0)
      case SAI_PORT_STAT_IF_IN_LINK_DOWN_CELL_DROP:
        // FABRIC link down cell drop is a clear on read counter from SAI
        // POV, so doing the counter accumulation here.
        hwPortStats.fabricLinkDownDroppedCells_() =
            hwPortStats.fabricLinkDownDroppedCells_().value_or(0) + value;
        break;
#endif
#if defined(BRCM_SAI_SDK_DNX_GTE_12_0)
      case SAI_PORT_STAT_FAST_LLFC_TRIGGER_STATUS:
        hwPortStats.linkLayerFlowControlWatermark_() = value;
        break;
#endif
      default:
        auto configuredDebugCounters =
            debugCounterManager.getConfiguredDebugStatIds();
        if (configuredDebugCounters.find(counterId) ==
            configuredDebugCounters.end()) {
          throw FbossError("Got unexpected port counter id: ", counterId);
        }
        if (counterId ==
            debugCounterManager.getPortL3BlackHoleCounterStatId()) {
          hwPortStats.inDstNullDiscards_() = value;
        } else if (
            counterId ==
            debugCounterManager.getMPLSLookupFailedCounterStatId()) {
          hwPortStats.inLabelMissDiscards_() = value;
        } else if (counterId == debugCounterManager.getAclDropCounterStatId()) {
          hwPortStats.inAclDiscards_() = value;
        } else if (
            counterId == debugCounterManager.getTrapDropCounterStatId()) {
          hwPortStats.inTrapDiscards_() = value;
        } else if (
            counterId == debugCounterManager.getEgressForwardingDropStatId()) {
          hwPortStats.outForwardingDiscards_() = value;
        } else {
          XLOG(FATAL)
              << " Should never get here, check configured debugCounterStatIds";
        }
        break;
    }
  }
}

phy::InterfaceType fromSaiInterfaceType(
    sai_port_interface_type_t saiInterfaceType) {
  switch (saiInterfaceType) {
    case SAI_PORT_INTERFACE_TYPE_CR:
      return phy::InterfaceType::CR;
    case SAI_PORT_INTERFACE_TYPE_CR2:
      return phy::InterfaceType::CR2;
    case SAI_PORT_INTERFACE_TYPE_CR4:
      return phy::InterfaceType::CR4;
    case SAI_PORT_INTERFACE_TYPE_SR:
      return phy::InterfaceType::SR;
    case SAI_PORT_INTERFACE_TYPE_SR4:
      return phy::InterfaceType::SR4;
    case SAI_PORT_INTERFACE_TYPE_KR:
      return phy::InterfaceType::KR;
    case SAI_PORT_INTERFACE_TYPE_KR4:
      return phy::InterfaceType::KR4;
    case SAI_PORT_INTERFACE_TYPE_CAUI:
      return phy::InterfaceType::CAUI;
    case SAI_PORT_INTERFACE_TYPE_GMII:
      return phy::InterfaceType::GMII;
    case SAI_PORT_INTERFACE_TYPE_SFI:
      return phy::InterfaceType::SFI;
    case SAI_PORT_INTERFACE_TYPE_XLAUI:
      return phy::InterfaceType::XLAUI;
    case SAI_PORT_INTERFACE_TYPE_KR2:
      return phy::InterfaceType::KR2;
    case SAI_PORT_INTERFACE_TYPE_CAUI4:
      return phy::InterfaceType::CAUI4;
    case SAI_PORT_INTERFACE_TYPE_SR2:
      return phy::InterfaceType::SR2;
#if SAI_API_VERSION >= SAI_VERSION(1, 10, 0)
    case SAI_PORT_INTERFACE_TYPE_SR8:
      return phy::InterfaceType::SR8;
    case SAI_PORT_INTERFACE_TYPE_CR8:
      return phy::InterfaceType::CR8;
#endif

    // Don't seem to currently have an equivalent fboss interface type
    case SAI_PORT_INTERFACE_TYPE_NONE:
    case SAI_PORT_INTERFACE_TYPE_LR:
    case SAI_PORT_INTERFACE_TYPE_XAUI:
    case SAI_PORT_INTERFACE_TYPE_XFI:
    case SAI_PORT_INTERFACE_TYPE_XGMII:
    case SAI_PORT_INTERFACE_TYPE_MAX:
    case SAI_PORT_INTERFACE_TYPE_LR4:
    default:
      XLOG(WARNING) << "Failed to convert sai interface type"
                    << static_cast<int>(saiInterfaceType);
      return phy::InterfaceType::NONE;
  }
}

TransmitterTechnology fromSaiMediaType(sai_port_media_type_t saiMediaType) {
  switch (saiMediaType) {
    case SAI_PORT_MEDIA_TYPE_UNKNOWN:
      return TransmitterTechnology::UNKNOWN;
    case SAI_PORT_MEDIA_TYPE_FIBER:
      return TransmitterTechnology::OPTICAL;
    case SAI_PORT_MEDIA_TYPE_COPPER:
      return TransmitterTechnology::COPPER;
    case SAI_PORT_MEDIA_TYPE_BACKPLANE:
      return TransmitterTechnology::BACKPLANE;
    case SAI_PORT_MEDIA_TYPE_NOT_PRESENT:
    default:
      XLOG(WARNING) << "Failed to convert sai media type "
                    << static_cast<int>(saiMediaType) << ". Default to UNKNOWN";
      return TransmitterTechnology::UNKNOWN;
  }
}
/*
 * Get the worst case optics delay we have assumed
 * in our buffer calculations.
 * Any delay reported by the ASIC can not factor in the
 * optics delay. So we factor in a max optics delay
 * in our calculations.
 * The actual optics delay maybe less, which means
 * we will report shorter cable lens. But short of getting
 * real-time optics delay, this is the best agent can do.
 */
int getWorstCaseAssumedOpticsDelayNS(const HwAsic& asic) {
  switch (asic.getAsicType()) {
    case cfg::AsicType::ASIC_TYPE_FAKE:
    case cfg::AsicType::ASIC_TYPE_MOCK:
    case cfg::AsicType::ASIC_TYPE_TRIDENT2:
    case cfg::AsicType::ASIC_TYPE_TOMAHAWK:
    case cfg::AsicType::ASIC_TYPE_TOMAHAWK3:
    case cfg::AsicType::ASIC_TYPE_TOMAHAWK4:
    case cfg::AsicType::ASIC_TYPE_TOMAHAWK5:
    case cfg::AsicType::ASIC_TYPE_TOMAHAWK6:
    case cfg::AsicType::ASIC_TYPE_ELBERT_8DD:
    case cfg::AsicType::ASIC_TYPE_EBRO:
    case cfg::AsicType::ASIC_TYPE_YUBA:
    case cfg::AsicType::ASIC_TYPE_CHENAB:
    case cfg::AsicType::ASIC_TYPE_JERICHO2:
    case cfg::AsicType::ASIC_TYPE_RAMON:
    case cfg::AsicType::ASIC_TYPE_GARONNE:
    case cfg::AsicType::ASIC_TYPE_SANDIA_PHY:
      break;
    case cfg::AsicType::ASIC_TYPE_JERICHO3:
    case cfg::AsicType::ASIC_TYPE_RAMON3:
      // For J3-R3, we measured max optics delay to
      // be 110ns.
      // TODO: get optics type info from qsfp svc and export a
      // accurate number for optics delay based on type
      return 110;
  }
  throw FbossError(
      "Optics delay not supported on asic type: ", asic.getAsicType());
}
} // namespace

void SaiPortHandle::resetQueues() {
  for (auto& cfgAndQueue : queues) {
    cfgAndQueue.second->resetQueue();
  }
}

SaiPortManager::SaiPortManager(
    SaiStore* saiStore,
    SaiManagerTable* managerTable,
    SaiPlatform* platform,
    ConcurrentIndices* concurrentIndices)
    : saiStore_(saiStore),
      managerTable_(managerTable),
      platform_(platform),
      removePortsAtExit_(platform_->getAsic()->isSupported(
          HwAsic::Feature::REMOVE_PORTS_FOR_COLDBOOT)),
      concurrentIndices_(concurrentIndices),
      hwLaneListIsPmdLaneList_(true),
      tcToQueueMapAllowedOnPort_(true),
      globalQosMapSupported_(
          managerTable_->switchManager().isGlobalQoSMapSupported()) {
#if defined(BRCM_SAI_SDK_XGS)
  if (platform_->getAsic()->isSupported(
          HwAsic::Feature::SAI_PORT_GET_PMD_LANES)) {
    auto& portStore = saiStore_->get<SaiPortTraits>();
    for (auto& iter : portStore.objects()) {
      auto saiPort = iter.second.lock();
      auto portSaiId = saiPort->adapterKey();
      auto pmdLanes = SaiApiTable::getInstance()->portApi().getAttribute(
          portSaiId, SaiPortTraits::Attributes::SerdesLaneList{});
      auto hwLanes = GET_ATTR(Port, HwLaneList, saiPort->adapterHostKey());
      if (pmdLanes.size() != hwLanes.size()) {
        hwLaneListIsPmdLaneList_ = false;
      }
    }
  }
  XLOG(DBG2) << "HwLaneList means pmd lane list or not: "
             << hwLaneListIsPmdLaneList_;
#endif
}

SaiPortHandle::~SaiPortHandle() {
  if (ingressSamplePacket) {
    port->setOptionalAttribute(
        SaiPortTraits::Attributes::IngressSamplePacketEnable{
            SAI_NULL_OBJECT_ID});
  }
  if (egressSamplePacket) {
    port->setOptionalAttribute(
        SaiPortTraits::Attributes::EgressSamplePacketEnable{
            SAI_NULL_OBJECT_ID});
  }
}

SaiPortManager::~SaiPortManager() {
  clearQosPolicy();
  releasePortPfcBuffers();
  releasePorts();
}

void SaiPortManager::resetSamplePacket(SaiPortHandle* portHandle) {
  if (portHandle->ingressSamplePacket) {
    portHandle->port->setOptionalAttribute(
        SaiPortTraits::Attributes::IngressSamplePacketEnable{
            SAI_NULL_OBJECT_ID});
  }
  if (portHandle->egressSamplePacket) {
    portHandle->port->setOptionalAttribute(
        SaiPortTraits::Attributes::EgressSamplePacketEnable{
            SAI_NULL_OBJECT_ID});
  }
}

void SaiPortManager::releasePortPfcBuffers() {
  for (const auto& handle : handles_) {
    const auto& saiPortHandle = handle.second;
    removeIngressPriorityGroupMappings(saiPortHandle.get());
  }
}

void SaiPortManager::releasePorts() {
  if (!removePortsAtExit_) {
    for (const auto& handle : handles_) {
      const auto& saiPortHandle = handle.second;
      resetSamplePacket(saiPortHandle.get());
      saiPortHandle->port->release();
    }
    for (const auto& handle : removedHandles_) {
      const auto& saiPortHandle = handle.second;
      resetSamplePacket(saiPortHandle.get());
      saiPortHandle->port->release();
    }
  }
}

void SaiPortManager::loadPortQueues(const Port& swPort) {
  if (swPort.getPortType() == cfg::PortType::FABRIC_PORT &&
      !platform_->getAsic()->isSupported(HwAsic::Feature::FABRIC_TX_QUEUES)) {
    return;
  }
  SaiPortHandle* portHandle = getPortHandle(swPort.getID());
  CHECK(portHandle)
      << " Port handle must be created before loading queues for port "
      << swPort.getID();
  const auto& saiPort = portHandle->port;
  std::vector<sai_object_id_t> queueList;
  queueList.resize(1);
  SaiPortTraits::Attributes::QosQueueList queueListAttribute{queueList};
  auto queueSaiIdList = SaiApiTable::getInstance()->portApi().getAttribute(
      portHandle->port->adapterKey(), queueListAttribute);
  if (queueSaiIdList.size() == 0) {
    throw FbossError("no queues exist for port ");
  }
  std::vector<QueueSaiId> queueSaiIds;
  queueSaiIds.reserve(queueSaiIdList.size());
  std::transform(
      queueSaiIdList.begin(),
      queueSaiIdList.end(),
      std::back_inserter(queueSaiIds),
      [](sai_object_id_t queueId) -> QueueSaiId {
        return QueueSaiId(queueId);
      });
  portHandle->queues = managerTable_->queueManager().loadQueues(queueSaiIds);

  auto asic = platform_->getAsic();
  QueueConfig updatedPortQueue;
  for (auto portQueue : std::as_const(*swPort.getPortQueues())) {
    auto queueKey =
        std::make_pair(portQueue->getID(), portQueue->getStreamType());
    const auto& configuredQueue = portHandle->queues[queueKey];
    portHandle->configuredQueues.push_back(configuredQueue.get());
    // TODO(zecheng): Modifying switch state in hw switch is generally bad
    // practice. Need to refactor to avoid it.
    auto clonedPortQueue = portQueue->clone();
    if (platform_->getAsic()->isSupported(HwAsic::Feature::BUFFER_POOL)) {
      if (auto reservedBytes = portQueue->getReservedBytes()) {
        clonedPortQueue->setReservedBytes(*reservedBytes);
      } else if (
          auto defaultReservedBytes = asic->getDefaultReservedBytes(
              portQueue->getStreamType(), swPort.getPortType())) {
        clonedPortQueue->setReservedBytes(*defaultReservedBytes);
      }

      if (auto scalingFactor = portQueue->getScalingFactor()) {
        clonedPortQueue->setScalingFactor(*scalingFactor);
      } else if (
          auto defaultScalingFactor = asic->getDefaultScalingFactor(
              portQueue->getStreamType(), false /* not cpu port*/)) {
        clonedPortQueue->setScalingFactor(*defaultScalingFactor);
      }
    } else if (portQueue->getReservedBytes() || portQueue->getScalingFactor()) {
      throw FbossError("Reserved bytes, scaling factor setting not supported");
    }
    auto pitr = portStats_.find(swPort.getID());

    if (pitr != portStats_.end()) {
      auto queueName = portQueue->getName()
          ? *portQueue->getName()
          : folly::to<std::string>("queue", portQueue->getID());
      // Port stats map is sparse, since we don't maintain/publish stats
      // for disabled ports
      pitr->second->queueChanged(portQueue->getID(), queueName);
    }
    updatedPortQueue.push_back(clonedPortQueue);
  }
  managerTable_->queueManager().ensurePortQueueConfig(
      saiPort->adapterKey(), portHandle->queues, updatedPortQueue, &swPort);
}

void SaiPortManager::addNode(const std::shared_ptr<Port>& swPort) {
  bool samplingMirror = swPort->getSampleDestination().has_value() &&
      swPort->getSampleDestination() == cfg::SampleDestination::MIRROR;
  /*
   * Based on sample destination, configure sflow based mirroring
   * or regular mirror on port attribute
   */
  if (samplingMirror) {
    if (swPort->getIngressMirror().has_value()) {
      programSamplingMirror(
          swPort->getID(),
          MirrorDirection::INGRESS,
          MirrorAction::START,
          swPort->getIngressMirror());
    }
    if (swPort->getEgressMirror().has_value()) {
      programSamplingMirror(
          swPort->getID(),
          MirrorDirection::EGRESS,
          MirrorAction::START,
          swPort->getEgressMirror());
    }
  } else {
    if (swPort->getIngressMirror().has_value()) {
      programMirror(
          swPort->getID(),
          MirrorDirection::INGRESS,
          MirrorAction::START,
          swPort->getIngressMirror());
    }
    if (swPort->getEgressMirror().has_value()) {
      programMirror(
          swPort->getID(),
          MirrorDirection::EGRESS,
          MirrorAction::START,
          swPort->getEgressMirror());
    }
  }
}

void SaiPortManager::programPfc(
    const std::shared_ptr<Port>& swPort,
    sai_uint8_t txPfc,
    sai_uint8_t rxPfc) {
  auto portHandle = getPortHandle(swPort->getID());

  auto pfcInfo = getPfcAttributes(txPfc, rxPfc);

  portHandle->port->setOptionalAttribute(
      SaiPortTraits::Attributes::PriorityFlowControlMode{*pfcInfo.pfcMode});
  if (*pfcInfo.pfcMode == SAI_PORT_PRIORITY_FLOW_CONTROL_MODE_COMBINED) {
    portHandle->port->setOptionalAttribute(
        SaiPortTraits::Attributes::PriorityFlowControl{*pfcInfo.pfcTxRx});
    XLOG(DBG3) << "Successfully enabled PFC on " << swPort->getName()
               << ", TX/RX=" << std::hex << *pfcInfo.pfcTxRx;
#if not defined(TAJO_SDK)
  } else {
    portHandle->port->setOptionalAttribute(
        SaiPortTraits::Attributes::PriorityFlowControlRx{*pfcInfo.pfcRx});
    portHandle->port->setOptionalAttribute(
        SaiPortTraits::Attributes::PriorityFlowControlTx{*pfcInfo.pfcTx});
    XLOG(DBG3) << "Successfully enabled PFC on " << swPort->getName()
               << ", TX=" << std::hex << *pfcInfo.pfcTx << ", Rx=" << std::hex
               << *pfcInfo.pfcRx;
#endif
  }
}

std::pair<sai_uint8_t, sai_uint8_t> SaiPortManager::preparePfcConfigs(
    const std::shared_ptr<Port>& swPort) const {
  auto pfc = swPort->getPfc();
  sai_uint8_t txPfc = 0;
  sai_uint8_t rxPfc = 0;

  if (pfc.has_value()) {
    sai_uint8_t enabledPriorities = 0; // Bitmap of enabled PFC priorities
    for (auto pri : swPort->getPfcPriorities()) {
      enabledPriorities |= (1 << static_cast<PfcPriority>(pri));
    }
    // PFC is enabled for priorities specified in PG configs
    txPfc = (*pfc->tx()) ? enabledPriorities : 0;
    rxPfc = (*pfc->rx()) ? enabledPriorities : 0;
  }
  return std::pair(txPfc, rxPfc);
}

SaiPortPfcInfo SaiPortManager::getPfcAttributes(
    sai_uint8_t txPfc,
    sai_uint8_t rxPfc) const {
  if (txPfc == rxPfc) {
    // Set the pfcTxRx to be txPfc as txPfc == rxPfc
    return SaiPortPfcInfo(
        SAI_PORT_PRIORITY_FLOW_CONTROL_MODE_COMBINED,
        std::nullopt,
        std::nullopt,
        txPfc);
  } else {
#if defined(TAJO_SDK)
    // PFC tx enabled / rx disabled and vice versa is unsupported
    // in the current TAJO implementation, tracked via WDG400C-448!
    throw FbossError("PFC TX and RX configured differently is unsupported!");
#else
    return SaiPortPfcInfo(
        SAI_PORT_PRIORITY_FLOW_CONTROL_MODE_SEPARATE,
        txPfc,
        rxPfc,
        std::nullopt);
#endif
  }
}

SaiPortPfcInfo SaiPortManager::getPortPfcAttributes(
    const std::shared_ptr<Port>& swPort) const {
  if (!swPort->getPfc().has_value()) {
    return SaiPortPfcInfo();
  }
  sai_uint8_t txPfc, rxPfc;
  std::tie(txPfc, rxPfc) = preparePfcConfigs(swPort);
  return getPfcAttributes(txPfc, rxPfc);
}

std::vector<sai_map_t> SaiPortManager::preparePfcDeadlockQueueTimers(
    std::vector<PfcPriority>& enabledPfcPriorities,
    uint32_t timerVal) {
  std::vector<sai_map_t> mapToValueList(
      cfg::switch_config_constants::PFC_PRIORITY_VALUE_MAX() + 1);
  for (int pri = 0;
       pri <= cfg::switch_config_constants::PFC_PRIORITY_VALUE_MAX();
       pri++) {
    // Quite a few ASICs have the timer supported at port level and hence
    // setting the same timer value on all priorities irrespective of if
    // PFC is enabled for that priority or not to simplify the SAI/SDK
    // side handling.
    sai_map_t mapping{};
    mapping.key = pri;
    mapping.value = timerVal;
    mapToValueList.at(pri) = mapping;
  }
  return mapToValueList;
}

void SaiPortManager::programPfcWatchdogTimers(
    const std::shared_ptr<Port>& swPort,
    std::vector<PfcPriority>& enabledPfcPriorities) {
  auto portHandle = getPortHandle(swPort->getID());
  CHECK(portHandle);
#if SAI_API_VERSION >= SAI_VERSION(1, 10, 2)
  CHECK(swPort->getPfc()->watchdog().has_value());
  uint32_t recoveryTimeMsecs =
      *swPort->getPfc()->watchdog()->recoveryTimeMsecs();
  uint32_t detectionTimeMsecs =
      *swPort->getPfc()->watchdog()->detectionTimeMsecs();
  // Set deadlock detection timer interval for PFC queues
  auto pfcDldTimerMap =
      preparePfcDeadlockQueueTimers(enabledPfcPriorities, detectionTimeMsecs);
  portHandle->port->setOptionalAttribute(
      SaiPortTraits::Attributes::PfcTcDldInterval{pfcDldTimerMap});
  // Set deadlock recovery timer interval for PFC queues
  auto pfcDlrTimerMap =
      preparePfcDeadlockQueueTimers(enabledPfcPriorities, recoveryTimeMsecs);
  portHandle->port->setOptionalAttribute(
      SaiPortTraits::Attributes::PfcTcDlrInterval{pfcDlrTimerMap});
  XLOG(DBG3) << "PFC WD timer programmed for " << swPort->getName();
#endif
}

void SaiPortManager::programPfcWatchdogPerQueueEnable(
    const std::shared_ptr<Port>& swPort,
    std::vector<PfcPriority>& enabledPfcPriorities,
    const bool portPfcWdEnabled) {
  // Enabled PFC priorities cannot be changed without a cold boot
  // and hence in this flow, just take care of a case where PFC
  // WD is being enabled or disabled for queues.
  for (auto pfcPri : enabledPfcPriorities) {
    // Assume 1:1 mapping b/w pfcPriority and queueId
    auto queueHandle =
        getQueueHandle(swPort->getID(), static_cast<uint8_t>(pfcPri));
    managerTable_->queueManager().queuePfcDeadlockDetectionRecoveryEnable(
        queueHandle, portPfcWdEnabled);
  }
  auto pfcWdEnabledStatus = portPfcWdEnabled ? "enabled" : "disabled";
  XLOG(DBG3) << "PFC WD " << pfcWdEnabledStatus << " on queues for "
             << swPort->getName();
}

void SaiPortManager::addPfc(const std::shared_ptr<Port>& swPort) {
  if (swPort->getPfc().has_value()) {
    // PFC is enabled for all priorities on a port
    sai_uint8_t txPfc, rxPfc;
    std::tie(txPfc, rxPfc) = preparePfcConfigs(swPort);
    programPfc(swPort, txPfc, rxPfc);
    // Add PFC WD
    addPfcWatchdog(swPort);
  }
}

void SaiPortManager::changePfc(
    const std::shared_ptr<Port>& oldPort,
    const std::shared_ptr<Port>& newPort) {
  if (oldPort->getPfc() != newPort->getPfc()) {
    sai_uint8_t oldTxPfc, oldRxPfc;
    sai_uint8_t newTxPfc, newRxPfc;
    std::tie(oldTxPfc, oldRxPfc) = preparePfcConfigs(oldPort);
    std::tie(newTxPfc, newRxPfc) = preparePfcConfigs(newPort);
    if (oldTxPfc != newTxPfc || oldRxPfc != newRxPfc) {
      programPfc(newPort, newTxPfc, newRxPfc);
    } else {
      XLOG(DBG4) << "PFC enabled setting unchanged for " << newPort->getName();
    }
    changePfcWatchdog(oldPort, newPort);
  } else {
    XLOG(DBG4) << "PFC setting unchanged for " << newPort->getName();
  }
}

void SaiPortManager::removePfc(const std::shared_ptr<Port>& swPort) {
  if (swPort->getPfc().has_value()) {
    // PFC WD to be removed first
    removePfcWatchdog(swPort);
    sai_uint8_t txPfc = 0, rxPfc = 0;
    programPfc(swPort, txPfc, rxPfc);
  }
}

void SaiPortManager::addPfcWatchdog(const std::shared_ptr<Port>& swPort) {
  if (swPort->getPfc()->watchdog().has_value()) {
    auto pfcEnabledPriorities = swPort->getPfcPriorities();
    programPfcWatchdogTimers(swPort, pfcEnabledPriorities);
  }
}

void SaiPortManager::changePfcWatchdog(
    const std::shared_ptr<Port>& oldPort,
    const std::shared_ptr<Port>& newPort) {
  std::optional<cfg::PfcWatchdog> oldPfcWd, newPfcWd;
  if (oldPort->getPfc() && oldPort->getPfc()->watchdog()) {
    oldPfcWd = *oldPort->getPfc()->watchdog();
  }
  if (newPort->getPfc() && newPort->getPfc()->watchdog()) {
    newPfcWd = *newPort->getPfc()->watchdog();
  }
  // Some asics (e.g. TH4/5) don't allow setting timer to zero when disabling
  // watchdog, just so set the per-queue enable flag.
  if (oldPfcWd.has_value() && !newPfcWd.has_value()) {
    auto pfcEnabledPriorities = oldPort->getPfcPriorities();
    programPfcWatchdogPerQueueEnable(
        oldPort, pfcEnabledPriorities, false /* wdEnabled */);
  } else if (
      newPfcWd.has_value() &&
      (!oldPfcWd.has_value() || newPfcWd.value() != oldPfcWd.value())) {
    auto pfcEnabledPriorities = newPort->getPfcPriorities();
    programPfcWatchdogTimers(newPort, pfcEnabledPriorities);
  } else {
    XLOG(DBG4) << "PFC watchdog setting unchanged for " << newPort->getName();
  }
}

void SaiPortManager::removePfcWatchdog(const std::shared_ptr<Port>& swPort) {
  if (swPort->getPfc()->watchdog()) {
    auto pfcEnabledPriorities = swPort->getPfcPriorities();
    programPfcWatchdogPerQueueEnable(
        swPort, pfcEnabledPriorities, false /* wdEnabled */);
  }
}

void SaiPortManager::removePfcBuffers(const std::shared_ptr<Port>& swPort) {
  removeIngressPriorityGroupMappings(getPortHandle(swPort->getID()));
}

void SaiPortManager::removeIngressPriorityGroupMappings(
    SaiPortHandle* portHandle) {
  // Unset the bufferProfile applied per IngressPriorityGroup
  for (const auto& ipgIndexInfo : portHandle->configuredIngressPriorityGroups) {
    const auto& ipgInfo = ipgIndexInfo.second;
    managerTable_->bufferManager().setIngressPriorityGroupBufferProfile(
        ipgInfo.pgHandle->ingressPriorityGroup, std::nullptr_t());
  }
  portHandle->configuredIngressPriorityGroups.clear();
}

cfg::PortType SaiPortManager::getPortType(PortID portId) const {
  auto pitr = port2PortType_.find(portId);
  CHECK(pitr != port2PortType_.end());
  return pitr->second;
}

void SaiPortManager::setPortType(PortID port, cfg::PortType type) {
  port2PortType_[port] = type;
  // If Port type changed, supported stats need to be updated
  port2SupportedStats_.clear();
  XLOG(DBG2) << " Port : " << port << " type set to : "
             << apache::thrift::TEnumTraits<cfg::PortType>::findName(type);
}

std::vector<IngressPriorityGroupSaiId>
SaiPortManager::getIngressPriorityGroupSaiIds(
    const std::shared_ptr<Port>& swPort) {
  SaiPortHandle* portHandle = getPortHandle(swPort->getID());
  auto portId = portHandle->port->adapterKey();
  auto numPgsPerPort = SaiApiTable::getInstance()->portApi().getAttribute(
      portId, SaiPortTraits::Attributes::NumberOfIngressPriorityGroups{});
  std::vector<sai_object_id_t> ingressPriorityGroupSaiIds{numPgsPerPort};
  SaiPortTraits::Attributes::IngressPriorityGroupList
      ingressPriorityGroupAttribute{ingressPriorityGroupSaiIds};
  auto ingressPgIds = SaiApiTable::getInstance()->portApi().getAttribute(
      portId, ingressPriorityGroupAttribute);
  std::vector<IngressPriorityGroupSaiId> ingressPgSaiIds{numPgsPerPort};
  for (int pgId = 0; pgId < numPgsPerPort; pgId++) {
    ingressPgSaiIds.at(pgId) =
        static_cast<IngressPriorityGroupSaiId>(ingressPgIds.at(pgId));
  }
  return ingressPgSaiIds;
}

void SaiPortManager::changePfcBuffers(
    std::shared_ptr<Port> oldPort,
    std::shared_ptr<Port> newPort) {
  if (!platform_->getAsic()->isSupported(HwAsic::Feature::BUFFER_POOL)) {
    return;
  }
  SaiPortHandle* portHandle = getPortHandle(newPort->getID());
  auto& configuredIpgs = portHandle->configuredIngressPriorityGroups;

  const auto& newPortPgCfgs = newPort->getPortPgConfigs();
  std::set<int> programmedPgIds;
  if (newPortPgCfgs) {
    const auto& ingressPgSaiIds = getIngressPriorityGroupSaiIds(newPort);
    auto ingressPriorityGroupHandles =
        managerTable_->bufferManager().loadIngressPriorityGroups(
            ingressPgSaiIds);
    for (const auto& portPgCfg : *newPortPgCfgs) {
      // THRIFT_COPY
      auto portPgCfgThrift = portPgCfg->toThrift();
      auto pgId = *portPgCfgThrift.id();
      auto bufferProfile =
          managerTable_->bufferManager().getOrCreateIngressProfile(
              portPgCfgThrift);
      managerTable_->bufferManager().setIngressPriorityGroupBufferProfile(
          ingressPriorityGroupHandles[pgId]->ingressPriorityGroup,
          bufferProfile);
      // Keep track of ingressPriorityGroupHandle and bufferProfile per PG ID
      configuredIpgs[static_cast<IngressPriorityGroupID>(pgId)] =
          SaiIngressPriorityGroupHandleAndProfile{
              std::move(ingressPriorityGroupHandles[pgId]), bufferProfile};
      programmedPgIds.insert(pgId);
    }
  }

  // Delete removed buffer profiles.
  if (oldPort != nullptr) {
    const auto& oldPortPgCfgs = oldPort->getPortPgConfigs();
    if (oldPortPgCfgs) {
      for (const auto& portPgCfg : *oldPortPgCfgs) {
        // THRIFT_COPY
        auto portPgCfgThrift = portPgCfg->toThrift();
        auto pgId = *portPgCfgThrift.id();
        if (programmedPgIds.find(pgId) == programmedPgIds.end()) {
          auto ipgInfo =
              configuredIpgs.find(static_cast<IngressPriorityGroupID>(pgId));
          if (ipgInfo != configuredIpgs.end()) {
            managerTable_->bufferManager().setIngressPriorityGroupBufferProfile(
                ipgInfo->second.pgHandle->ingressPriorityGroup,
                std::nullptr_t());
            configuredIpgs.erase(ipgInfo);
          }
        }
      }
    }
  }
}

prbs::InterfacePrbsState SaiPortManager::getPortPrbsState(PortID portId) {
  prbs::InterfacePrbsState portPrbsState;
  if (!platform_->getAsic()->isSupported(HwAsic::Feature::SAI_PRBS)) {
    return portPrbsState;
  }
  auto* handle = getPortHandleImpl(PortID(portId));
  auto prbsPolynomial = SaiApiTable::getInstance()->portApi().getAttribute(
      handle->port->adapterKey(), SaiPortTraits::Attributes::PrbsPolynomial{});
  auto prbsConfig = SaiApiTable::getInstance()->portApi().getAttribute(
      handle->port->adapterKey(), SaiPortTraits::Attributes::PrbsConfig{});
  portPrbsState.polynomial() =
      static_cast<prbs::PrbsPolynomial>(prbsPolynomial);
  portPrbsState.generatorEnabled() =
      (prbsConfig == SAI_PORT_PRBS_CONFIG_ENABLE_TX_RX);
  portPrbsState.checkerEnabled() =
      (prbsConfig == SAI_PORT_PRBS_CONFIG_ENABLE_TX_RX);
  return portPrbsState;
}

sai_port_prbs_config_t SaiPortManager::getSaiPortPrbsConfig(
    bool enabled) const {
  if (enabled) {
    return SAI_PORT_PRBS_CONFIG_ENABLE_TX_RX;
  } else {
    return SAI_PORT_PRBS_CONFIG_DISABLE;
  }
}

double SaiPortManager::calculateRate(uint32_t speed) {
  auto rateGb = speed / kSpeedConversionFactor;
  return rateGb * kRateConversionFactor;
}

void SaiPortManager::updatePrbsStatsEntryRate(
    const std::shared_ptr<Port>& swPort) {
  if (!platform_->getAsic()->isSupported(HwAsic::Feature::SAI_PRBS)) {
    return;
  }
  auto portID = swPort->getID();
  auto rate = calculateRate(static_cast<int>(swPort->getSpeed()));
  auto portAsicPrbsStatsItr = portAsicPrbsStats_.find(portID);
  if (portAsicPrbsStatsItr == portAsicPrbsStats_.end()) {
    throw FbossError(
        "Asic prbs lane error map not initialized for port ", portID);
  }
  auto& prbsStatsTable = portAsicPrbsStatsItr->second;
  for (auto& prbsStatsEntry : prbsStatsTable) {
    prbsStatsEntry.setRate(rate);
  }
}

void SaiPortManager::initAsicPrbsStats(const std::shared_ptr<Port>& swPort) {
  if (!platform_->getAsic()->isSupported(HwAsic::Feature::SAI_PRBS)) {
    return;
  }
  auto portId = swPort->getID();
  XLOG(DBG2) << "ASIC PRBS enabled with polynomial set to "
             << swPort->getAsicPrbs().polynominal().value() << " for port "
             << portId;
  auto speed = static_cast<int>(swPort->getSpeed());
  auto rate = calculateRate(speed);
  auto prbsStatsTable = PrbsStatsTable();
  // Dump cumulative PRBS stats on first PrbsStatsEntry because there is no
  // per-lane PRBS counter available in SAI.
  prbsStatsTable.push_back(PrbsStatsEntry(portId, rate));
  portAsicPrbsStats_[portId] = std::move(prbsStatsTable);
#if SAI_API_VERSION >= SAI_VERSION(1, 8, 1) && defined(BRCM_SAI_SDK_XGS_AND_DNX)
  // Trigger initial read of PrbsRxState to help clear any initial lock
  // losses
  auto portHandle = getPortHandle(portId);
  SaiApiTable::getInstance()->portApi().getAttribute(
      portHandle->port->adapterKey(), SaiPortTraits::Attributes::PrbsRxState{});
#endif
}

PortSaiId SaiPortManager::addPort(const std::shared_ptr<Port>& swPort) {
  setPortType(swPort->getID(), swPort->getPortType());
  auto portSaiId = addPortImpl(swPort);
  concurrentIndices_->portSaiId2PortInfo.emplace(
      portSaiId,
      ConcurrentIndices::PortInfo{swPort->getID(), swPort->getPortType()});
  concurrentIndices_->portSaiIds.emplace(swPort->getID(), portSaiId);
  concurrentIndices_->vlanIds.emplace(
      PortDescriptorSaiId(portSaiId), swPort->getIngressVlan());
  auto platformPort = platform_->getPlatformPort(swPort->getID());
  if (swPort->getPortType() == cfg::PortType::RECYCLE_PORT &&
      platformPort->getScope() == cfg::Scope::GLOBAL) {
    // If Recycle port is present in the config, we expect:
    //  - the config must have exactly one global recycle port,
    //  - that recycle port must be used by CPU port
    // Otherwise, fail check.
    // In future, if we need to support multiple global recycle ports, we would
    // need to invent some way to determiine which of the recycle ports
    // corresponds to the CPU port.
    // Assert that the recycle port we are setting is either the same or unset
    // from before. During rollback, we do set recycle port again, but it should
    // have the same value
    CHECK_EQ(
        managerTable_->switchManager().getCpuRecyclePort().value_or(portSaiId),
        portSaiId);
    managerTable_->switchManager().setCpuRecyclePort(portSaiId);
  }

  XLOG(DBG2) << "added port " << swPort->getID() << " with vlan "
             << swPort->getIngressVlan();

  return portSaiId;
}

void SaiPortManager::changePort(
    const std::shared_ptr<Port>& oldPort,
    const std::shared_ptr<Port>& newPort) {
  if (oldPort->getPortType() != newPort->getPortType()) {
    setPortType(newPort->getID(), newPort->getPortType());
  }
  changePortImpl(oldPort, newPort);
}

void SaiPortManager::resetCableLength(PortID portId) {
  auto portStatItr = portStats_.find(portId);
  if (portStatItr == portStats_.end()) {
    // No stats exist, nothing to do
    return;
  }
  auto curPortStats = portStatItr->second->portStats();
  if (!curPortStats.cableLengthMeters().has_value()) {
    // Value not set, return
    return;
  }
  curPortStats.cableLengthMeters().reset();
  auto now = duration_cast<seconds>(system_clock::now().time_since_epoch());
  portStatItr->second->updateStats(curPortStats, now);
}

void SaiPortManager::addSamplePacket(const std::shared_ptr<Port>& swPort) {
  if (swPort->getSflowIngressRate()) {
    programSampling(
        swPort->getID(),
        SamplePacketDirection::INGRESS,
        SamplePacketAction::START,
        swPort->getSflowIngressRate(),
        swPort->getSampleDestination());
  }
  if (swPort->getSflowEgressRate()) {
    programSampling(
        swPort->getID(),
        SamplePacketDirection::EGRESS,
        SamplePacketAction::START,
        swPort->getSflowEgressRate(),
        swPort->getSampleDestination());
  }
}

void SaiPortManager::removeMirror(const std::shared_ptr<Port>& swPort) {
  SaiPortHandle* portHandle = getPortHandle(swPort->getID());
  if (swPort->getIngressMirror().has_value()) {
    if (portHandle->mirrorInfo.isMirrorSampled()) {
      programSamplingMirror(
          swPort->getID(),
          MirrorDirection::INGRESS,
          MirrorAction::STOP,
          swPort->getIngressMirror());
    } else {
      programMirror(
          swPort->getID(),
          MirrorDirection::INGRESS,
          MirrorAction::STOP,
          swPort->getIngressMirror());
    }
  }

  if (swPort->getEgressMirror().has_value()) {
    if (portHandle->mirrorInfo.isMirrorSampled()) {
      programSamplingMirror(
          swPort->getID(),
          MirrorDirection::EGRESS,
          MirrorAction::STOP,
          swPort->getEgressMirror());
    } else {
      programMirror(
          swPort->getID(),
          MirrorDirection::EGRESS,
          MirrorAction::STOP,
          swPort->getEgressMirror());
    }
  }
}

void SaiPortManager::removeSamplePacket(const std::shared_ptr<Port>& swPort) {
  if (swPort->getSflowIngressRate()) {
    programSampling(
        swPort->getID(),
        SamplePacketDirection::INGRESS,
        SamplePacketAction::STOP,
        swPort->getSflowIngressRate(),
        swPort->getSampleDestination());
  }
  if (swPort->getSflowEgressRate()) {
    programSampling(
        swPort->getID(),
        SamplePacketDirection::EGRESS,
        SamplePacketAction::STOP,
        swPort->getSflowEgressRate(),
        swPort->getSampleDestination());
  }
}

void SaiPortManager::removePort(const std::shared_ptr<Port>& swPort) {
  auto swId = swPort->getID();
  auto itr = handles_.find(swId);
  if (itr == handles_.end()) {
    throw FbossError("Attempted to remove non-existent port: ", swId);
  }

  removeMirror(swPort);
  removeSamplePacket(swPort);
  removePfcBuffers(swPort);
  removePfc(swPort);
  clearQosPolicy(swId);

  concurrentIndices_->portSaiId2PortInfo.erase(itr->second->port->adapterKey());
  concurrentIndices_->portSaiIds.erase(swId);
  concurrentIndices_->vlanIds.erase(
      PortDescriptorSaiId(itr->second->port->adapterKey()));
  addRemovedHandle(itr->first);
  handles_.erase(itr);
  portStats_.erase(swId);
  port2SupportedStats_.erase(swId);
  port2PortType_.erase(swId);
  auto portAsicPrbsStatsItr = portAsicPrbsStats_.find(swId);
  if (portAsicPrbsStatsItr != portAsicPrbsStats_.end()) {
    portAsicPrbsStats_.erase(swId);
  }
  // TODO: do FDB entries associated with this port need to be removed
  // now?
  XLOG(DBG2) << "removed port " << swPort->getID() << " with vlan "
             << swPort->getIngressVlan();
}

void SaiPortManager::changeQueue(
    const std::shared_ptr<Port>& swPort,
    const QueueConfig& oldQueueConfig,
    const QueueConfig& newQueueConfig) {
  if (swPort->getPortType() == cfg::PortType::FABRIC_PORT &&
      !platform_->getAsic()->isSupported(HwAsic::Feature::FABRIC_TX_QUEUES)) {
    return;
  }
  auto swId = swPort->getID();
  SaiPortHandle* portHandle = getPortHandle(swId);
  if (!portHandle) {
    throw FbossError("Attempted to change non-existent port ");
  }
  auto pitr = portStats_.find(swId);
  portHandle->configuredQueues.clear();
  const auto asic = platform_->getAsic();
  for (auto newPortQueue : std::as_const(newQueueConfig)) {
    // Queue create or update
    SaiQueueConfig saiQueueConfig =
        std::make_pair(newPortQueue->getID(), newPortQueue->getStreamType());
    auto queueHandle = getQueueHandle(swId, saiQueueConfig);
    if (!queueHandle) {
      throw FbossError(
          "unable to change non-existent ",
          apache::thrift::util::enumNameSafe(newPortQueue->getStreamType()),
          " queue ",
          newPortQueue->getID(),
          " of port ",
          swId);
    }
    // TODO(zecheng): Modifying switch state in hw switch is generally bad
    // practice. Need to refactor to avoid it.
    auto portQueue = newPortQueue->clone();
    if (platform_->getAsic()->isSupported(HwAsic::Feature::BUFFER_POOL) &&
        (SAI_QUEUE_TYPE_FABRIC_TX !=
         SaiApiTable::getInstance()->queueApi().getAttribute(
             queueHandle->queue->adapterKey(),
             SaiQueueTraits::Attributes::Type{}))) {
      if (auto reservedBytes = newPortQueue->getReservedBytes()) {
        portQueue->setReservedBytes(*reservedBytes);
      } else if (
          auto defaultReservedBytes = asic->getDefaultReservedBytes(
              newPortQueue->getStreamType(), swPort->getPortType())) {
        portQueue->setReservedBytes(*defaultReservedBytes);
      }
      if (auto scalingFactor = newPortQueue->getScalingFactor()) {
        portQueue->setScalingFactor(*scalingFactor);
      } else if (
          auto defaultScalingFactor = asic->getDefaultScalingFactor(
              newPortQueue->getStreamType(), false /* not cpu port */)) {
        portQueue->setScalingFactor(*defaultScalingFactor);
      }
    } else if (
        newPortQueue->getReservedBytes() || newPortQueue->getScalingFactor()) {
      throw FbossError("Reserved bytes, scaling factor setting not supported");
    }
    managerTable_->queueManager().changeQueue(
        queueHandle, *portQueue, swPort.get(), swPort->getPortType());
    auto queueName = newPortQueue->getName()
        ? *newPortQueue->getName()
        : folly::to<std::string>("queue", newPortQueue->getID());
    if (pitr != portStats_.end()) {
      // Port stats map is sparse, since we don't maintain/publish stats
      // for disabled ports
      pitr->second->queueChanged(newPortQueue->getID(), queueName);
    }
    portHandle->configuredQueues.push_back(queueHandle);
  }

  for (auto oldPortQueue : std::as_const(oldQueueConfig)) {
    auto portQueueIter = std::find_if(
        newQueueConfig.begin(),
        newQueueConfig.end(),
        [&](const std::shared_ptr<PortQueue> portQueue) {
          return portQueue->getID() == oldPortQueue->getID();
        });
    // Queue Remove
    if (portQueueIter == newQueueConfig.end()) {
      SaiQueueConfig saiQueueConfig =
          std::make_pair(oldPortQueue->getID(), oldPortQueue->getStreamType());
      portHandle->queues.erase(saiQueueConfig);
      if (pitr != portStats_.end()) {
        // Port stats map is sparse, since we don't maintain/publish stats
        // for disabled ports
        pitr->second->queueRemoved(oldPortQueue->getID());
      }
    }
  }
}

void SaiPortManager::changeSamplePacket(
    const std::shared_ptr<Port>& oldPort,
    const std::shared_ptr<Port>& newPort) {
  if (newPort->getSflowIngressRate() != oldPort->getSflowIngressRate()) {
    if (newPort->getSflowIngressRate() != 0) {
      programSampling(
          newPort->getID(),
          SamplePacketDirection::INGRESS,
          SamplePacketAction::START,
          newPort->getSflowIngressRate(),
          newPort->getSampleDestination());
    } else {
      programSampling(
          newPort->getID(),
          SamplePacketDirection::INGRESS,
          SamplePacketAction::STOP,
          newPort->getSflowIngressRate(),
          newPort->getSampleDestination());
    }
  }

  if (newPort->getSflowEgressRate() != oldPort->getSflowEgressRate()) {
    if (newPort->getSflowEgressRate() != 0) {
      programSampling(
          newPort->getID(),
          SamplePacketDirection::EGRESS,
          SamplePacketAction::START,
          newPort->getSflowEgressRate(),
          newPort->getSampleDestination());
    } else {
      programSampling(
          newPort->getID(),
          SamplePacketDirection::EGRESS,
          SamplePacketAction::STOP,
          newPort->getSflowEgressRate(),
          newPort->getSampleDestination());
    }
  }
}

void SaiPortManager::changeMirror(
    const std::shared_ptr<Port>& oldPort,
    const std::shared_ptr<Port>& newPort) {
  bool oldSamplingMirror = oldPort->getSampleDestination().has_value() &&
      oldPort->getSampleDestination().value() == cfg::SampleDestination::MIRROR;
  bool newSamplingMirror = newPort->getSampleDestination().has_value() &&
      newPort->getSampleDestination().value() == cfg::SampleDestination::MIRROR;

  /*
   * If there is any update to the mirror session on a port, detach the
   * current mirror session from the port and re-attach the new mirror session
   * if valid
   */
  auto programMirroring =
      [this, oldPort, newPort, oldSamplingMirror, newSamplingMirror](
          MirrorDirection direction) {
        auto oldMirrorId = direction == MirrorDirection::INGRESS
            ? oldPort->getIngressMirror()
            : oldPort->getEgressMirror();
        auto newMirrorId = direction == MirrorDirection::INGRESS
            ? newPort->getIngressMirror()
            : newPort->getEgressMirror();
        if (oldSamplingMirror) {
          programSamplingMirror(
              newPort->getID(), direction, MirrorAction::STOP, oldMirrorId);
        } else {
          programMirror(
              newPort->getID(), direction, MirrorAction::STOP, oldMirrorId);
        }
        if (newMirrorId.has_value()) {
          if (newSamplingMirror) {
            programSamplingMirror(
                newPort->getID(), direction, MirrorAction::START, newMirrorId);
          } else {
            programMirror(
                newPort->getID(), direction, MirrorAction::START, newMirrorId);
          }
        }
      };

  if (newPort->getIngressMirror() != oldPort->getIngressMirror()) {
    programMirroring(MirrorDirection::INGRESS);
  }

  if (newPort->getEgressMirror() != oldPort->getEgressMirror()) {
    programMirroring(MirrorDirection::EGRESS);
  }

  SaiPortHandle* portHandle = getPortHandle(newPort->getID());
  SaiPortMirrorInfo mirrorInfo{
      newPort->getIngressMirror(),
      newPort->getEgressMirror(),
      newSamplingMirror};
  portHandle->mirrorInfo = mirrorInfo;
}

bool SaiPortManager::createOnlyAttributeChanged(
    const SaiPortTraits::CreateAttributes& oldAttributes,
    const SaiPortTraits::CreateAttributes& newAttributes) {
  // The SAI_PORT_SPEED_CHANGE is the feature which allows port speed change
  // with a delete and recreate. This feature however is not support for
  // TAJO SDK 1.42.8 and hence would need to fall back to the SAI port speed
  // change API to achieve the same. However, the SAI api based speed change
  // is possible only when the number of lanes are not changing, so the current
  // use case of 100G <-> 200G would work, but not potential future use cases
  // like 200G <-> 400G and 400G <-> 100G.  The SAI_PORT_SPEED_CHANGE will be
  // supported in future releases like TAJO SDK 24.4.90 and should be enabled
  // once validated.
  return (std::get<SaiPortTraits::Attributes::HwLaneList>(oldAttributes) !=
          std::get<SaiPortTraits::Attributes::HwLaneList>(newAttributes)) ||
      (platform_->getAsic()->isSupported(
           HwAsic::Feature::SAI_PORT_SPEED_CHANGE) &&
       (std::get<SaiPortTraits::Attributes::Speed>(oldAttributes) !=
        std::get<SaiPortTraits::Attributes::Speed>(newAttributes)));
}

bool SaiPortManager::createOnlyAttributeChanged(
    const std::shared_ptr<Port>& oldPort,
    const std::shared_ptr<Port>& newPort) {
  return createOnlyAttributeChanged(
      attributesFromSwPort(oldPort), attributesFromSwPort(newPort));
}

cfg::PortType SaiPortManager::derivePortTypeOfLogicalPort(
    PortSaiId portSaiId) const {
  // TODO: An extension attribute has been added for MANAGEMENT port type,
  // however, its available in 11.0 onwards and addresses the needs on J3.
  // MANAGEMENT+INTERFACE are reported as LOGICAL ports on rest of the SAI
  // SDK.  Using the below logic as per suggestion in CS00012332892 to
  // differentiate MANAGEMENT from INTERFACE ports, needed for TH5. Eventual
  // goal is to use a new port type in SAI to identify management ports.
  if (platform_->getAsic()->isSupported(HwAsic::Feature::MANAGEMENT_PORT) &&
      platform_->getAsic()->isSupported(HwAsic::Feature::PFC)) {
    auto numIngressPriorities =
        SaiApiTable::getInstance()->portApi().getAttribute(
            portSaiId,
            SaiPortTraits::Attributes::NumberOfIngressPriorityGroups{});
    if (!numIngressPriorities) {
      return cfg::PortType::MANAGEMENT_PORT;
    }
  }
  return cfg::PortType::INTERFACE_PORT;
}

std::shared_ptr<Port> SaiPortManager::swPortFromAttributes(
    SaiPortTraits::CreateAttributes attributes,
    PortSaiId portSaiId,
    cfg::SwitchType switchType) const {
  auto speed = static_cast<cfg::PortSpeed>(GET_ATTR(Port, Speed, attributes));
#if defined(BRCM_SAI_SDK_XGS)
  std::vector<uint32_t> lanes;
  if (hwLaneListIsPmdLaneList_) {
    lanes = GET_ATTR(Port, HwLaneList, attributes);
  } else {
    lanes = SaiApiTable::getInstance()->portApi().getAttribute(
        portSaiId, SaiPortTraits::Attributes::SerdesLaneList{});
  }
#else
  auto lanes = GET_ATTR(Port, HwLaneList, attributes);
#endif
  SaiPortTraits::Attributes::Type portType = SAI_PORT_TYPE_LOGICAL;
  if (switchType == cfg::SwitchType::FABRIC ||
      switchType == cfg::SwitchType::VOQ) {
    portType = SaiApiTable::getInstance()->portApi().getAttribute(
        portSaiId, SaiPortTraits::Attributes::Type{});
  }
  auto [portID, allProfiles] =
      platform_->findPortIDAndProfiles(speed, lanes, portSaiId);
  if (allProfiles.empty()) {
    throw FbossError(
        "No port profiles found for port ",
        portID,
        ", speed ",
        (int)speed,
        ", lanes ",
        folly::join(",", lanes));
  }
  // We don't have enough information here to match the SDK's state with one
  // of the supported profiles for this platform. We ideally need medium as
  // well, but SDK could default to any medium and then the platform may not
  // necessarily support a profileID that matches SDK's speed + lanes +
  // medium. Therefore, we'll just pick the first profile in the list and
  // update the SAI store later with properties (speed, lanes, medium, fec
  // etc) of this profile. Later at applyThriftConfig time, we'll find out
  // exactly which profile should be used and then update SAI with the new
  // profile if different than what we picked here
  auto profileID = allProfiles[0];

  state::PortFields portFields;
  portFields.portId() = portID;
  portFields.portName() = folly::to<std::string>(portID);
  auto port = std::make_shared<Port>(std::move(portFields));

  switch (portType.value()) {
    case SAI_PORT_TYPE_LOGICAL:
      port->setPortType(derivePortTypeOfLogicalPort(portSaiId));
      break;
#if defined(BRCM_SAI_SDK_DNX_GTE_11_0)
    case SAI_PORT_TYPE_MGMT:
      port->setPortType(cfg::PortType::MANAGEMENT_PORT);
      break;
#endif
#if defined(BRCM_SAI_SDK_DNX_GTE_11_0)
    case SAI_PORT_TYPE_EVENTOR:
      port->setPortType(cfg::PortType::EVENTOR_PORT);
      break;
#endif
    case SAI_PORT_TYPE_FABRIC:
      port->setPortType(cfg::PortType::FABRIC_PORT);
      break;
#if SAI_API_VERSION >= SAI_VERSION(1, 10, 0)
    case SAI_PORT_TYPE_RECYCLE:
      port->setPortType(cfg::PortType::RECYCLE_PORT);
      break;
#endif
    case SAI_PORT_TYPE_CPU:
      XLOG(FATAL) << " Unexpected port type, CPU";
      break;
  }
  // speed, hw lane list, fec mode
  port->setProfileId(profileID);
  PlatformPortProfileConfigMatcher matcher{port->getProfileID(), portID};
  if (auto profileConfig = platform_->getPortProfileConfig(matcher)) {
    port->setProfileConfig(*profileConfig->iphy());
  } else {
    throw FbossError(
        "No port profile config found with matcher:", matcher.toString());
  }
  port->resetPinConfigs(
      platform_->getPlatformMapping()->getPortIphyPinConfigs(matcher));
  port->setSpeed(speed);

  // admin state
  bool isEnabled = GET_OPT_ATTR(Port, AdminState, attributes);
  port->setAdminState(
      isEnabled ? cfg::PortState::ENABLED : cfg::PortState::DISABLED);

  // flow control mode
  cfg::PortPause pause;
  auto globalFlowControlMode =
      GET_OPT_ATTR(Port, GlobalFlowControlMode, attributes);
  pause.rx() =
      (globalFlowControlMode == SAI_PORT_FLOW_CONTROL_MODE_BOTH_ENABLE ||
       globalFlowControlMode == SAI_PORT_FLOW_CONTROL_MODE_RX_ONLY);
  pause.tx() =
      (globalFlowControlMode == SAI_PORT_FLOW_CONTROL_MODE_BOTH_ENABLE ||
       globalFlowControlMode == SAI_PORT_FLOW_CONTROL_MODE_TX_ONLY);
  port->setPause(pause);

  // vlan id
  auto vlan = GET_OPT_ATTR(Port, PortVlanId, attributes);
  port->setIngressVlan(static_cast<VlanID>(vlan));

#if SAI_API_VERSION >= SAI_VERSION(1, 12, 0)
  auto lbMode = GET_OPT_ATTR(Port, PortLoopbackMode, attributes);
  port->setLoopbackMode(utility::getCfgPortLoopbackMode(
      static_cast<sai_port_loopback_mode_t>(lbMode)));
#else
  auto ilbMode = GET_OPT_ATTR(Port, InternalLoopbackMode, attributes);
  port->setLoopbackMode(utility::getCfgPortInternalLoopbackMode(
      static_cast<sai_port_internal_loopback_mode_t>(ilbMode)));
#endif

  // TODO: support Preemphasis once it is also used

  // mtu
  port->setMaxFrameSize(GET_OPT_ATTR(Port, Mtu, attributes));

  // asic prbs
  phy::PortPrbsState prbsState;
  auto prbsConfig = GET_OPT_ATTR(Port, PrbsConfig, attributes);
  prbsState.enabled() = (prbsConfig == SAI_PORT_PRBS_CONFIG_ENABLE_TX_RX);
  auto prbsPolynomial = GET_OPT_ATTR(Port, PrbsPolynomial, attributes);
  prbsState.polynominal() = prbsPolynomial;
  port->setAsicPrbs(prbsState);

#if defined(BRCM_SAI_SDK_GTE_12_0) && defined(BRCM_SAI_SDK_DNX)
  auto reachabilityGroupId = GET_OPT_ATTR(Port, ReachabilityGroup, attributes);
  if (reachabilityGroupId > 0) {
    port->setReachabilityGroupId(reachabilityGroupId);
  }
#endif
  port->setScope(platform_->getPlatformMapping()->getPortScope(port->getID()));

#if defined(BRCM_SAI_SDK_DNX_GTE_11_0)
  if (port->getPortType() == cfg::PortType::INTERFACE_PORT ||
      port->getPortType() == cfg::PortType::MANAGEMENT_PORT) {
    auto shelEnable = GET_OPT_ATTR(Port, ShelEnable, attributes);
    port->setSelfHealingECMPLagEnable(shelEnable);
  }
#endif

#if defined(BRCM_SAI_SDK_DNX_GTE_11_7)
  if (platform_->getAsic()->isSupported(
          HwAsic::Feature::FEC_ERROR_DETECT_ENABLE)) {
    auto fecErrorDetectEnable =
        GET_OPT_ATTR(Port, FecErrorDetectEnable, attributes);
    if (fecErrorDetectEnable) {
      port->setFecErrorDetectEnable(fecErrorDetectEnable);
    }
  }
#endif

  return port;
}

// private const getter for use by const and non-const getters
SaiPortHandle* SaiPortManager::getPortHandleImpl(PortID swId) const {
  auto itr = handles_.find(swId);
  if (itr == handles_.end()) {
    return nullptr;
  }
  if (!itr->second.get()) {
    XLOG(FATAL) << "Invalid null SaiPortHandle for " << swId;
  }
  return itr->second.get();
}

const SaiPortHandle* SaiPortManager::getPortHandle(PortID swId) const {
  return getPortHandleImpl(swId);
}

SaiPortHandle* SaiPortManager::getPortHandle(PortID swId) {
  return getPortHandleImpl(swId);
}

// private const getter for use by const and non-const getters
SaiQueueHandle* SaiPortManager::getQueueHandleImpl(
    PortID swId,
    const SaiQueueConfig& saiQueueConfig) const {
  auto portHandle = getPortHandleImpl(swId);
  if (!portHandle) {
    XLOG(FATAL) << "Invalid null SaiPortHandle for " << swId;
  }
  auto itr = portHandle->queues.find(saiQueueConfig);
  if (itr == portHandle->queues.end()) {
    return nullptr;
  }
  if (!itr->second.get()) {
    XLOG(FATAL) << "Invalid null SaiQueueHandle for " << swId;
  }
  return itr->second.get();
}

const SaiQueueHandle* SaiPortManager::getQueueHandle(
    PortID swId,
    const SaiQueueConfig& saiQueueConfig) const {
  return getQueueHandleImpl(swId, saiQueueConfig);
}

SaiQueueHandle* SaiPortManager::getQueueHandle(
    PortID swId,
    const SaiQueueConfig& saiQueueConfig) {
  return getQueueHandleImpl(swId, saiQueueConfig);
}

SaiQueueHandle* SaiPortManager::getQueueHandle(PortID swId, uint8_t queueId)
    const {
  auto portHandle = getPortHandleImpl(swId);
  if (!portHandle) {
    XLOG(FATAL) << "Invalid null SaiPortHandle for " << swId;
  }
  for (const auto& queue : portHandle->queues) {
    if (queue.first.first == queueId) {
      return queue.second.get();
    }
  }
  XLOG(FATAL) << "Invalid queue ID " << queueId << " for port " << swId;
}

bool SaiPortManager::fecStatsSupported(PortID portId) const {
  if (platform_->getAsic()->getAsicType() ==
          cfg::AsicType::ASIC_TYPE_TOMAHAWK5 &&
      getPortType(portId) == cfg::PortType::MANAGEMENT_PORT) {
    // TODO(daiweix): follow up why not supported on TH5 mgmt port, e.g.
    // SAI_PORT_STAT_IF_IN_FEC_CORRECTABLE_FRAMES
    return false;
  }
  return platform_->getAsic()->isSupported(HwAsic::Feature::SAI_FEC_COUNTERS) &&
      utility::isReedSolomonFec(getFECMode(portId));
}

bool SaiPortManager::fecCorrectedBitsSupported(PortID portId) const {
  if (platform_->getAsic()->getAsicType() ==
          cfg::AsicType::ASIC_TYPE_TOMAHAWK5 &&
      getPortType(portId) == cfg::PortType::MANAGEMENT_PORT) {
    // TODO(daiweix): follow up why not supported on TH5 mgmt port, e.g.
    // SAI_PORT_STAT_IF_IN_FEC_CORRECTED_BITS
    return false;
  }
  if (platform_->getAsic()->isSupported(
          HwAsic::Feature::SAI_FEC_CORRECTED_BITS) &&
      utility::isReedSolomonFec(getFECMode(portId))) {
#if SAI_API_VERSION >= SAI_VERSION(1, 13, 0)
    return true;
#endif
  }
  return false;
}

bool SaiPortManager::rxFrequencyRPMSupported() const {
#if defined(BRCM_SAI_SDK_GTE_10_0)
  return platform_->getAsic()->isSupported(HwAsic::Feature::RX_FREQUENCY_PPM);
#else
  return false;
#endif
}

bool SaiPortManager::rxSerdesParametersSupported() const {
#if defined(SAI_VERSION_13_0_EA_ODP) || defined(SAI_VERSION_13_0_EA_DNX_ODP)
  return platform_->getAsic()->isSupported(
      HwAsic::Feature::RX_SERDES_PARAMETERS);
#else
  return false;
#endif
}

bool SaiPortManager::rxSNRSupported() const {
#if defined(BRCM_SAI_SDK_GTE_10_0)
  return platform_->getAsic()->isSupported(HwAsic::Feature::RX_SNR);
#else
  return false;
#endif
}

bool SaiPortManager::fecCodewordsStatsSupported(PortID portId) const {
#if defined(BRCM_SAI_SDK_GTE_10_0) || defined(BRCM_SAI_SDK_DNX_GTE_11_0) || \
    defined(TAJO_SDK_GTE_24_8_3001)
  return platform_->getAsic()->isSupported(
             HwAsic::Feature::SAI_FEC_CODEWORDS_STATS) &&
      utility::isReedSolomonFec(getFECMode(portId)) &&
      getPortType(portId) == cfg::PortType::INTERFACE_PORT;
#else
  return false;
#endif
}

std::vector<PortID> SaiPortManager::getFabricReachabilityForSwitch(
    const SwitchID& switchId) const {
  std::vector<PortID> reachablePorts;
  const auto& portApi = SaiApiTable::getInstance()->portApi();
  for (const auto& [portId, handle] : handles_) {
    if (getPortType(portId) == cfg::PortType::FABRIC_PORT) {
      sai_fabric_port_reachability_t reachability;
      reachability.switch_id = switchId;
      auto attr = SaiPortTraits::Attributes::FabricReachability{reachability};
      if (portApi.getAttribute(handle->port->adapterKey(), attr).reachable) {
        reachablePorts.push_back(portId);
      }
    }
  }
  return reachablePorts;
}

std::optional<FabricEndpoint> SaiPortManager::getFabricConnectivity(
    const PortID& portId,
    const SaiPortHandle* portHandle) const {
  if (getPortType(portId) != cfg::PortType::FABRIC_PORT) {
    return std::nullopt;
  }

  FabricEndpoint endpoint;
  auto saiPortId = portHandle->port->adapterKey();
  endpoint.isAttached() = SaiApiTable::getInstance()->portApi().getAttribute(
      saiPortId, SaiPortTraits::Attributes::FabricAttached{});
  if (*endpoint.isAttached()) {
    auto swId = SaiApiTable::getInstance()->portApi().getAttribute(
        saiPortId, SaiPortTraits::Attributes::FabricAttachedSwitchId{});
    auto swType = SaiApiTable::getInstance()->portApi().getAttribute(
        saiPortId, SaiPortTraits::Attributes::FabricAttachedSwitchType{});
    auto endpointPortId = SaiApiTable::getInstance()->portApi().getAttribute(
        saiPortId, SaiPortTraits::Attributes::FabricAttachedPortIndex{});
    endpoint.switchId() = swId;
    endpoint.portId() = endpointPortId;
    switch (swType) {
      case SAI_SWITCH_TYPE_VOQ:
        endpoint.switchType() = cfg::SwitchType::VOQ;
        break;
      case SAI_SWITCH_TYPE_FABRIC:
        endpoint.switchType() = cfg::SwitchType::FABRIC;
        break;
      default:
        XLOG(ERR) << " Unexpected switch type value: " << swType;
        break;
    }
  }
  return endpoint;
}

std::map<PortID, FabricEndpoint> SaiPortManager::getFabricConnectivity() const {
  std::map<PortID, FabricEndpoint> port2FabricEndpoint;
  for (const auto& portIdAndHandle : handles_) {
    if (auto endpoint = getFabricConnectivity(
            portIdAndHandle.first, portIdAndHandle.second.get())) {
      port2FabricEndpoint.insert({PortID(portIdAndHandle.first), *endpoint});
    }
  }
  return port2FabricEndpoint;
}

std::optional<FabricEndpoint> SaiPortManager::getFabricConnectivity(
    const PortID& portId) const {
  std::optional<FabricEndpoint> endpoint = std::nullopt;
  auto handlesItr = handles_.find(portId);
  if (handlesItr != handles_.end()) {
    endpoint = getFabricConnectivity(portId, handlesItr->second.get());
  }
  return endpoint;
}

std::vector<phy::PrbsLaneStats> SaiPortManager::getPortAsicPrbsStats(
    PortID portId) {
  std::vector<phy::PrbsLaneStats> prbsStats;
  if (!platform_->getAsic()->isSupported(HwAsic::Feature::SAI_PRBS)) {
    return prbsStats;
  }
#if SAI_API_VERSION >= SAI_VERSION(1, 8, 1)
  auto portAsicPrbsStatsItr = portAsicPrbsStats_.find(portId);
  if (portAsicPrbsStatsItr == portAsicPrbsStats_.end()) {
    throw FbossError(
        "Asic prbs lane error map not initialized for port ", portId);
  }
  auto& prbsStatsTable = portAsicPrbsStatsItr->second;
  for (const auto& prbsStatsEntry : prbsStatsTable) {
    prbsStats.push_back(prbsStatsEntry.getPrbsStats());
  }
#endif
  return prbsStats;
}

void SaiPortManager::clearPortAsicPrbsStats(PortID portId) {
#if SAI_API_VERSION >= SAI_VERSION(1, 8, 1)
  auto portAsicPrbsStatsItr = portAsicPrbsStats_.find(portId);
  if (portAsicPrbsStatsItr == portAsicPrbsStats_.end()) {
    throw FbossError(
        "Asic prbs lane error map not initialized for port ", portId);
  }
  auto& prbsStatsTable = portAsicPrbsStatsItr->second;
  auto& prbsStatsEntry = prbsStatsTable.front();
  prbsStatsEntry.clearPrbsStats();
  auto* handle = getPortHandleImpl(PortID(portId));
  // Read PrbsRxState when PRBS stats are cleared to reset start point for next
  // read.
  SaiApiTable::getInstance()->portApi().getAttribute(
      handle->port->adapterKey(), SaiPortTraits::Attributes::PrbsRxState{});
#endif
}

void SaiPortManager::updatePrbsStats(PortID portId) {
  if (!platform_->getAsic()->isSupported(HwAsic::Feature::SAI_PRBS)) {
    return;
  }
#if SAI_API_VERSION >= SAI_VERSION(1, 8, 1)
  auto* handle = getPortHandleImpl(PortID(portId));
  auto prbsConfig = GET_OPT_ATTR(Port, PrbsConfig, handle->port->attributes());
  if (prbsConfig == SAI_PORT_PRBS_CONFIG_DISABLE) {
    return;
  }
  auto prbsRxState = SaiApiTable::getInstance()->portApi().getAttribute(
      handle->port->adapterKey(), SaiPortTraits::Attributes::PrbsRxState{});
  auto portAsicPrbsStatsItr = portAsicPrbsStats_.find(portId);
  if (portAsicPrbsStatsItr == portAsicPrbsStats_.end()) {
    throw FbossError(
        "Asic prbs lane error map not initialized for port ", portId);
  }
  auto& prbsStatsTable = portAsicPrbsStatsItr->second;
  // Dump cumulative PRBS stats on first PrbsStatsEntry because there is no
  // per-lane PRBS counter available in SAI.
  auto& prbsStatsEntry = prbsStatsTable.front();
  switch (prbsRxState.rx_status) {
    case SAI_PORT_PRBS_RX_STATUS_OK:
      prbsStatsEntry.handleOk();
      break;
    case SAI_PORT_PRBS_RX_STATUS_LOCK_WITH_ERRORS:
      prbsStatsEntry.handleLockWithErrors(prbsRxState.error_count);
      break;
    case SAI_PORT_PRBS_RX_STATUS_NOT_LOCKED:
      prbsStatsEntry.handleNotLocked();
      break;
    case SAI_PORT_PRBS_RX_STATUS_LOST_LOCK:
      prbsStatsEntry.handleLossOfLock();
      break;
  }
#endif
}

void SaiPortManager::updateStats(
    PortID portId,
    bool updateWatermarks,
    bool updateCableLengths) {
  auto handlesItr = handles_.find(portId);
  if (handlesItr == handles_.end()) {
    return;
  }
  auto portType = getPortType(portId);
  auto now = duration_cast<seconds>(system_clock::now().time_since_epoch());
  auto* handle = handlesItr->second.get();
  auto portStatItr = portStats_.find(portId);
  if (portStatItr == portStats_.end()) {
    // We don't maintain port stats for disabled ports.
    return;
  }
  const auto& prevPortStats = portStatItr->second->portStats();
  HwPortStats curPortStats{prevPortStats};
  // All stats start with a unitialized (-1) value. If there are no in
  // discards (first collection) we will just report that -1 as the monotonic
  // counter. Instead set it to 0 if uninintialized
  setUninitializedStatsToZero(*curPortStats.inCongestionDiscards_());
  setUninitializedStatsToZero(*curPortStats.inDiscards_());
  setUninitializedStatsToZero(*curPortStats.fecCorrectableErrors());
  setUninitializedStatsToZero(*curPortStats.fecUncorrectableErrors());
  // For fabric ports the following counters would never be collected
  // Set them to 0
  setUninitializedStatsToZero(*curPortStats.inDstNullDiscards_());
  setUninitializedStatsToZero(*curPortStats.inDiscardsRaw_());
  setUninitializedStatsToZero(*curPortStats.inPause_());

  curPortStats.timestamp_() = now.count();
  handle->port->updateStats(supportedStats(portId), SAI_STATS_MODE_READ);
#if defined(BRCM_SAI_SDK_DNX_GTE_12_0)
  if (updateWatermarks &&
      platform_->getAsic()->isSupported(HwAsic::Feature::FAST_LLFC_COUNTER)) {
    handle->port->updateStats(
        {SAI_PORT_STAT_FAST_LLFC_TRIGGER_STATUS},
        SAI_STATS_MODE_READ_AND_CLEAR);
  }
#endif

  bool updateFecStats = false;
  auto lastFecReadTimeIt = lastFecCounterReadTime_.find(portId);
  if (lastFecReadTimeIt == lastFecCounterReadTime_.end() ||
      (now.count() - lastFecReadTimeIt->second) >=
          FLAGS_fec_counters_update_interval_s) {
    lastFecCounterReadTime_[portId] = now.count();
    updateFecStats = true;
    if (fecStatsSupported(portId)) {
      handle->port->updateStats(
          {SAI_PORT_STAT_IF_IN_FEC_CORRECTABLE_FRAMES,
           SAI_PORT_STAT_IF_IN_FEC_NOT_CORRECTABLE_FRAMES},
          SAI_STATS_MODE_READ_AND_CLEAR);
    }
#if SAI_API_VERSION >= SAI_VERSION(1, 13, 0)
    if (fecCorrectedBitsSupported(portId)) {
      handle->port->updateStats(
          {SAI_PORT_STAT_IF_IN_FEC_CORRECTED_BITS}, SAI_STATS_MODE_READ);
    }
#endif
#if SAI_API_VERSION >= SAI_VERSION(1, 11, 0)
    if (fecCodewordsStatsSupported(portId)) {
      // maxFecCounterId should ideally be derived from SAI attribute
      // SAI_PORT_ATTR_MAX_FEC_SYMBOL_ERRORS_DETECTABLE but this attribute isn't
      // supported yet
      sai_stat_id_t maxFecCounterId = getFECMode(portId) == phy::FecMode::RS528
          ? SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S8
          : SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S15;
      std::vector<sai_stat_id_t> fecCodewordsToRead;
      for (int counterId = SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S0;
           counterId <= (int)maxFecCounterId;
           counterId++) {
        fecCodewordsToRead.push_back(static_cast<sai_stat_id_t>(counterId));
      }
      handle->port->updateStats(fecCodewordsToRead, SAI_STATS_MODE_READ);
    }
#endif
  }
  const auto& counters = handle->port->getStats();
  fillHwPortStats(
      counters,
      managerTable_->debugCounterManager(),
      curPortStats,
      platform_,
      portType,
      updateFecStats);
  std::vector<utility::CounterPrevAndCur> toSubtractFromInDiscardsRaw = {
      {*prevPortStats.inDstNullDiscards_(),
       *curPortStats.inDstNullDiscards_()}};
  if (platform_->getAsic()->isSupported(
          HwAsic::Feature::IN_PAUSE_INCREMENTS_DISCARDS)) {
    toSubtractFromInDiscardsRaw.emplace_back(
        *prevPortStats.inPause_(), *curPortStats.inPause_());
  }
  for (auto& [priority, current] : *curPortStats.inPfc_()) {
    if (current > 0) {
      toSubtractFromInDiscardsRaw.emplace_back(
          folly::get_default(*prevPortStats.inPfc_(), priority, 0), current);
    }
  }
  *curPortStats.inDiscards_() += utility::subtractIncrements(
      {*prevPortStats.inDiscardsRaw_(), *curPortStats.inDiscardsRaw_()},
      toSubtractFromInDiscardsRaw);
  managerTable_->queueManager().updateStats(
      handle->configuredQueues, curPortStats, updateWatermarks);
  managerTable_->macsecManager().updateStats(portId, curPortStats);
  managerTable_->bufferManager().updateIngressPriorityGroupStats(
      portId, curPortStats, updateWatermarks);
  auto logicalPortId = platform_->getPlatformPort(portId)->getHwLogicalPortId();
  if (logicalPortId) {
    curPortStats.logicalPortId() = *logicalPortId;
  }

  const auto& asic = platform_->getAsic();
  if (updateCableLengths && isPortUp(portId) &&
      portType == cfg::PortType::FABRIC_PORT &&
      asic->isSupported(HwAsic::Feature::CABLE_PROPOGATION_DELAY)) {
    bool cableLenAvailableOnPort = true;
    if (asic->getSwitchType() == cfg::SwitchType::FABRIC &&
        asic->getFabricNodeRole() == HwAsic::FabricNodeRole::DUAL_STAGE_L1) {
      cableLenAvailableOnPort =
          asic->getL1FabricPortsToConnectToL2().contains(portId);
    }

    /*
    ** Cable length collection is expensive, taking upto 50ms per
    ** port. Cable length can really only change in face of recabling. So
    ** we optimize as follows
    ** - Reset cable len on port down event
    ** - Collect cable len only for up ports that don't have cable len set.

    ** The reason for resetting on port down event and not on stats
    collection
    ** round is that stats collection is periodic. So consider a port
    getting
    ** recabled, if it got recabled and came up within our stats collection
    ** interval, we would not recollect cable len until next warm/cold boot.
    ** Reason for not collecting cable len stat on port Up and doing it
    ** in periodic stat collection is that we may need to try multiple times
    ** since when port comes up, not everything  is synchronized immediately
    */
    if (cableLenAvailableOnPort &&
        !curPortStats.cableLengthMeters().has_value()) {
      try {
        int32_t cablePropogationDelayNS =
            SaiApiTable::getInstance()->portApi().getAttribute(
                handle->port->adapterKey(),
                SaiPortTraits::Attributes::CablePropogationDelayNS{});
        cablePropogationDelayNS = std::max(
            cablePropogationDelayNS -
                getWorstCaseAssumedOpticsDelayNS(*platform_->getAsic()),
            0);
        // In fiber it takes about 5ns for light to travel 1 meter
        curPortStats.cableLengthMeters() =
            std::ceil(cablePropogationDelayNS / 5.0);
      } catch (const SaiApiError& e) {
        XLOG(ERR) << "Failed to get cable propogation delay for port " << portId
                  << ": " << e.what();
      }
    }
  }

  if (portType == cfg::PortType::FABRIC_PORT &&
      platform_->getAsic()->isSupported(HwAsic::Feature::DATA_CELL_FILTER)) {
    std::optional<SaiPortTraits::Attributes::FabricDataCellsFilterStatus>
        attrT = SaiPortTraits::Attributes::FabricDataCellsFilterStatus{};

    auto dataCelllsFilterOn =
        SaiApiTable::getInstance()->portApi().getAttribute(
            handle->port->adapterKey(), attrT);
    if (dataCelllsFilterOn.has_value() && dataCelllsFilterOn.value() == true) {
      curPortStats.dataCellsFilterOn() = true;
    } else {
      curPortStats.dataCellsFilterOn() = false;
    }
  }
  portStats_[portId]->updateStats(curPortStats, now);
  auto lastPrbsRxStateReadTimeIt = lastPrbsRxStateReadTime_.find(portId);
  if (lastPrbsRxStateReadTimeIt == lastPrbsRxStateReadTime_.end() ||
      (now.count() - lastPrbsRxStateReadTimeIt->second) >=
          FLAGS_prbs_update_interval_s) {
    lastPrbsRxStateReadTime_[portId] = now.count();
    updatePrbsStats(portId);
  }
}

const std::vector<sai_stat_id_t>& SaiPortManager::supportedStats(PortID port) {
  auto itr = port2SupportedStats_.find(port);
  if (itr != port2SupportedStats_.end()) {
    return itr->second;
  }
  fillInSupportedStats(port);
  return port2SupportedStats_.find(port)->second;
}

std::map<PortID, HwPortStats> SaiPortManager::getPortStats() const {
  std::map<PortID, HwPortStats> portStats;
  for (const auto& handle : handles_) {
    auto portStatItr = portStats_.find(handle.first);
    if (portStatItr == portStats_.end()) {
      // We don't maintain port stats for disabled ports.
      continue;
    }
    HwPortStats hwPortStats{portStatItr->second->portStats()};
    portStats.emplace(handle.first, hwPortStats);
  }
  return portStats;
}

void SaiPortManager::clearStats(PortID port) {
  auto portHandle = getPortHandle(PortID(port));
  if (!portHandle) {
    return;
  }
  auto statsToClear = supportedStats(port);
  // Debug counters are implemented differently than regular port counters
  // and not all implementations support clearing them. For our use case
  // it doesn't particularly matter if we can't clear them. So prune the
  // debug counter clear for now.
  auto skipClear =
      managerTable_->debugCounterManager().getConfiguredDebugStatIds();
  statsToClear.erase(
      std::remove_if(
          statsToClear.begin(),
          statsToClear.end(),
          [&skipClear](auto counterId) {
            return skipClear.find(counterId) != skipClear.end();
          }),
      statsToClear.end());
  portHandle->port->clearStats(statsToClear);
  managerTable_->queueManager().clearStats(portHandle->configuredQueues);
}

void SaiPortManager::clearInterfacePhyCounters(const PortID& portId) {
  auto portStatItr = portStats_.find(portId);
  if (portStatItr == portStats_.end()) {
    return;
  }

  // Clear accumulated FEC counters
  auto curPortStats = portStatItr->second->portStats();
  curPortStats.fecCorrectableErrors() = 0;
  curPortStats.fecUncorrectableErrors() = 0;

  portStatItr->second->clearStat(kFecCorrectable());
  portStatItr->second->clearStat(kFecUncorrectable());

  auto now = duration_cast<seconds>(system_clock::now().time_since_epoch());
  portStatItr->second->updateStats(curPortStats, now);
}

const HwPortFb303Stats* SaiPortManager::getLastPortStat(PortID port) const {
  auto pitr = portStats_.find(port);
  return pitr != portStats_.end() ? pitr->second.get() : nullptr;
}

cfg::PortSpeed SaiPortManager::getMaxSpeed(PortID port) const {
  // TODO (srikrishnagopu): Use the read-only attribute
  // SAI_PORT_ATTR_SUPPORTED_SPEED to query the list of supported speeds
  // and return the maximum supported speed.
  return platform_->getPortMaxSpeed(port);
}

std::shared_ptr<MultiSwitchPortMap> SaiPortManager::reconstructPortsFromStore(
    cfg::SwitchType switchType) const {
  auto* scopeResolver = platform_->scopeResolver();
  auto& portStore = saiStore_->get<SaiPortTraits>();
  auto portMap = std::make_shared<MultiSwitchPortMap>();
  for (auto& iter : portStore.objects()) {
    auto saiPort = iter.second.lock();
    auto port = swPortFromAttributes(
        saiPort->attributes(), saiPort->adapterKey(), switchType);
    portMap->addNode(port, scopeResolver->scope(port));
  }
  return portMap;
}

void SaiPortManager::setQosMapsOnPort(
    PortID portID,
    std::vector<std::pair<sai_qos_map_type_t, QosMapSaiId>>& qosMaps) {
  auto portHandle = getPortHandle(portID);
  auto portType = getPortType(portID);
  auto& port = portHandle->port;
  auto isPfcSupported =
      (portType != cfg::PortType::RECYCLE_PORT &&
       portType != cfg::PortType::EVENTOR_PORT &&
       portType != cfg::PortType::MANAGEMENT_PORT);
  for (auto qosMapTypeToSaiId : qosMaps) {
    auto mapping = qosMapTypeToSaiId.second;
    switch (qosMapTypeToSaiId.first) {
      case SAI_QOS_MAP_TYPE_DSCP_TO_TC:
        port->setOptionalAttribute(
            SaiPortTraits::Attributes::QosDscpToTcMap{mapping});
        break;
      case SAI_QOS_MAP_TYPE_TC_TO_QUEUE:
        /*
         * On certain platforms, applying TC to QUEUE mapping on front panel
         * port will be applied on system port by the underlying SDK.
         * It can applied in either of them - Front panel port or on system
         * port. We decided to go with system port for two reasons 1) Remote
         * system port on a local device also need to be applied with this TC
         * to Queue Map 2) Cleaner approach to have the separation of applying
         * TC to Queue map on all system ports in VOQ mode
         */
        if (tcToQueueMapAllowedOnPort_) {
          port->setOptionalAttribute(
              SaiPortTraits::Attributes::QosTcToQueueMap{mapping});
        }
        break;
      case SAI_QOS_MAP_TYPE_TC_TO_PRIORITY_GROUP:
        if (isPfcSupported) {
          port->setOptionalAttribute(
              SaiPortTraits::Attributes::QosTcToPriorityGroupMap{mapping});
        }
        break;
      case SAI_QOS_MAP_TYPE_PFC_PRIORITY_TO_QUEUE:
        if (isPfcSupported) {
          port->setOptionalAttribute(
              SaiPortTraits::Attributes::QosPfcPriorityToQueueMap{mapping});
        }
        break;
      default:
        throw FbossError("Unhandled qos map ", qosMapTypeToSaiId.first);
    }
  }
}

std::vector<std::pair<sai_qos_map_type_t, QosMapSaiId>>
SaiPortManager::getNullSaiIdsForQosMaps() {
  std::vector<std::pair<sai_qos_map_type_t, QosMapSaiId>> qosMaps{};
  auto nullObjId = QosMapSaiId(SAI_NULL_OBJECT_ID);
  if (!globalQosMapSupported_) {
    qosMaps.push_back({SAI_QOS_MAP_TYPE_DSCP_TO_TC, nullObjId});
    qosMaps.push_back({SAI_QOS_MAP_TYPE_TC_TO_QUEUE, nullObjId});
  }

  auto qosMapHandle = managerTable_->qosMapManager().getQosMap();
  if (qosMapHandle) {
    if (qosMapHandle->tcToPgMap) {
      qosMaps.push_back({SAI_QOS_MAP_TYPE_TC_TO_PRIORITY_GROUP, nullObjId});
    }
    if (qosMapHandle->pfcPriorityToQueueMap) {
      qosMaps.push_back({SAI_QOS_MAP_TYPE_PFC_PRIORITY_TO_QUEUE, nullObjId});
    }
  }

  return qosMaps;
}

std::vector<std::pair<sai_qos_map_type_t, QosMapSaiId>>
SaiPortManager::getSaiIdsForQosMaps(const SaiQosMapHandle* qosMapHandle) {
  std::vector<std::pair<sai_qos_map_type_t, QosMapSaiId>> qosMaps{};
  if (!globalQosMapSupported_) {
    qosMaps.push_back(
        {SAI_QOS_MAP_TYPE_DSCP_TO_TC, qosMapHandle->dscpToTcMap->adapterKey()});
    qosMaps.push_back(
        {SAI_QOS_MAP_TYPE_TC_TO_QUEUE,
         qosMapHandle->tcToQueueMap->adapterKey()});
  }
  if (qosMapHandle->tcToPgMap) {
    qosMaps.push_back(
        {SAI_QOS_MAP_TYPE_TC_TO_PRIORITY_GROUP,
         qosMapHandle->tcToPgMap->adapterKey()});
  }
  if (qosMapHandle->pfcPriorityToQueueMap) {
    qosMaps.push_back(
        {SAI_QOS_MAP_TYPE_PFC_PRIORITY_TO_QUEUE,
         qosMapHandle->pfcPriorityToQueueMap->adapterKey()});
  }
  return qosMaps;
}

void SaiPortManager::setQosPolicy(
    PortID portID,
    const std::optional<std::string>& qosPolicy) {
  if (getPortType(portID) == cfg::PortType::FABRIC_PORT) {
    return;
  }
  XLOG(DBG2) << "set QoS policy " << (qosPolicy ? qosPolicy.value() : "null")
             << " for port " << portID;
  auto qosMapHandle = managerTable_->qosMapManager().getQosMap(qosPolicy);
  if (!qosMapHandle) {
    if (qosPolicy) {
      throw FbossError(
          "QosMap handle is null for QoS policy: ", qosPolicy.value());
    }
    XLOG(DBG2)
        << "skip programming QoS policy on port " << portID
        << " because applied QoS policy is null and default QoS policy is absent";
    return;
  }
  auto qosMaps = getSaiIdsForQosMaps(qosMapHandle);
  auto handle = getPortHandle(portID);
  if (!globalQosMapSupported_) {
    handle->dscpToTcQosMap = qosMapHandle->dscpToTcMap;
    handle->tcToQueueQosMap = qosMapHandle->tcToQueueMap;
    handle->qosPolicy = qosMapHandle->name;
  }
  setQosMapsOnPort(portID, qosMaps);
}

void SaiPortManager::setQosPolicy(const std::shared_ptr<QosPolicy>& qosPolicy) {
  for (const auto& portIdAndHandle : handles_) {
    if (portIdAndHandle.second->qosPolicy == qosPolicy->getName()) {
      setQosPolicy(portIdAndHandle.first, qosPolicy->getName());
    }
  }
}

void SaiPortManager::clearQosPolicy(PortID portID) {
  if (getPortType(portID) == cfg::PortType::FABRIC_PORT) {
    return;
  }
  auto handle = getPortHandle(portID);
  XLOG(DBG2) << "clear QoS policy "
             << (handle->qosPolicy ? handle->qosPolicy.value() : "null")
             << " for port " << portID;
  if (handle->qosPolicy) {
    auto qosMaps = getNullSaiIdsForQosMaps();
    setQosMapsOnPort(portID, qosMaps);
    handle->dscpToTcQosMap.reset();
    handle->tcToQueueQosMap.reset();
    handle->qosPolicy.reset();
  }
}

void SaiPortManager::clearQosPolicy(
    const std::shared_ptr<QosPolicy>& qosPolicy) {
  for (const auto& portIdAndHandle : handles_) {
    if (portIdAndHandle.second->qosPolicy == qosPolicy->getName()) {
      clearQosPolicy(portIdAndHandle.first);
    }
  }
}

void SaiPortManager::clearQosPolicy() {
  // clear qos policy for all ports
  auto qosMaps = getNullSaiIdsForQosMaps();
  for (const auto& portIdAndHandle : handles_) {
    clearQosPolicy(portIdAndHandle.first);
  }
}

void SaiPortManager::changeQosPolicy(
    const std::shared_ptr<Port>& oldPort,
    const std::shared_ptr<Port>& newPort) {
  if (oldPort->getQosPolicy() == newPort->getQosPolicy()) {
    return;
  }
  clearQosPolicy(oldPort->getID());
  setQosPolicy(newPort->getID(), newPort->getQosPolicy());
}

void SaiPortManager::clearArsConfig(PortID portID) {
  if (getPortType(portID) == cfg::PortType::FABRIC_PORT) {
    return;
  }
  clearPortFlowletConfig(portID);
}

void SaiPortManager::clearArsConfig() {
  for (const auto& portIdAndHandle : handles_) {
    clearArsConfig(portIdAndHandle.first);
  }
}

void SaiPortManager::setTamObject(
    PortID portId,
    std::vector<sai_object_id_t> tamObjects) {
  getPortHandle(portId)->port->setOptionalAttribute(
      SaiPortTraits::Attributes::TamObject{std::move(tamObjects)});
}

void SaiPortManager::resetTamObject(PortID portId) {
  getPortHandle(portId)->port->setOptionalAttribute(
      SaiPortTraits::Attributes::TamObject{
          std::vector<sai_object_id_t>{SAI_NULL_OBJECT_ID}});
}

void SaiPortManager::programSampling(
    PortID portId,
    SamplePacketDirection direction,
    SamplePacketAction action,
    uint64_t sampleRate,
    std::optional<cfg::SampleDestination> sampleDestination) {
  auto portType = getPortType(portId);
  if (portType != cfg::PortType::INTERFACE_PORT) {
    throw FbossError(
        "Programming Sampling is only supported for Interface Ports; PortID: ",
        portId,
        " has type",
        apache::thrift::util::enumNameSafe(portType));
  }
  auto destination = sampleDestination.has_value()
      ? sampleDestination.value()
      : cfg::SampleDestination::CPU;
  SaiPortHandle* portHandle = getPortHandle(portId);
  auto samplePacketHandle = action == SamplePacketAction::START
      ? managerTable_->samplePacketManager().getOrCreateSamplePacket(
            sampleRate, destination)
      : nullptr;
  /*
   * Create the sammple packet object, update the port sample packet
   * oid and then overwrite the sample packet object on port.
   */
  auto samplePacketAdapterKey = samplePacketHandle
      ? samplePacketHandle->adapterKey()
      : SAI_NULL_OBJECT_ID;
  if (direction == SamplePacketDirection::INGRESS) {
    portHandle->port->setOptionalAttribute(
        SaiPortTraits::Attributes::IngressSamplePacketEnable{
            SamplePacketSaiId(samplePacketAdapterKey)});
    portHandle->ingressSamplePacket = samplePacketHandle;
  } else {
    portHandle->port->setOptionalAttribute(
        SaiPortTraits::Attributes::EgressSamplePacketEnable{
            SamplePacketSaiId(samplePacketAdapterKey)});
    portHandle->egressSamplePacket = samplePacketHandle;
  }

  XLOG(DBG) << "Programming sample packet on port: " << std::hex
            << portHandle->port->adapterKey() << " action: "
            << (action == SamplePacketAction::START ? "start" : "stop")
            << " direction: "
            << (direction == SamplePacketDirection::INGRESS ? "ingress"
                                                            : "egress")
            << " sample packet oid " << std::hex << samplePacketAdapterKey
            << "destination: "
            << (destination == cfg::SampleDestination::CPU ? "cpu" : "mirror");
}

void SaiPortManager::programMirror(
    PortID portId,
    MirrorDirection direction,
    MirrorAction action,
    std::optional<std::string> mirrorId) {
  auto portType = getPortType(portId);
  if (portType != cfg::PortType::INTERFACE_PORT) {
    throw FbossError(
        "Programming mirroring is only supported for Interface Ports; PortID: ",
        portId,
        " has type",
        apache::thrift::util::enumNameSafe(portType));
  }

  auto portHandle = getPortHandle(portId);
  std::vector<sai_object_id_t> mirrorOidList{};
  if (action == MirrorAction::START) {
    auto mirrorHandle =
        managerTable_->mirrorManager().getMirrorHandle(mirrorId.value());
    if (!mirrorHandle) {
      XLOG(DBG) << "Failed to find mirror session: " << mirrorId.value();
      return;
    }
    mirrorOidList.push_back(mirrorHandle->adapterKey());
  }

  if (direction == MirrorDirection::INGRESS) {
    portHandle->port->setOptionalAttribute(
        SaiPortTraits::Attributes::IngressMirrorSession{mirrorOidList});
  } else {
    portHandle->port->setOptionalAttribute(
        SaiPortTraits::Attributes::EgressMirrorSession{mirrorOidList});
  }

  XLOG(DBG) << "Programming mirror on port: " << std::hex
            << portHandle->port->adapterKey()
            << " action: " << (action == MirrorAction::START ? "start" : "stop")
            << " direction: "
            << (direction == MirrorDirection::INGRESS ? "ingress" : "egress");
}

void SaiPortManager::programSamplingMirror(
    PortID portId,
    MirrorDirection direction,
    MirrorAction action,
    std::optional<std::string> mirrorId) {
  auto portType = getPortType(portId);
  if (portType != cfg::PortType::INTERFACE_PORT) {
    throw FbossError(
        "Programming sampling mirror is only supported for Interface Ports; PortID: ",
        portId,
        " has type",
        apache::thrift::util::enumNameSafe(portType));
  }
  auto portHandle = getPortHandle(portId);
  std::vector<sai_object_id_t> mirrorOidList{};
  if (action == MirrorAction::START) {
    auto mirrorHandle =
        managerTable_->mirrorManager().getMirrorHandle(mirrorId.value());
    if (!mirrorHandle) {
      XLOG(DBG) << "Failed to find mirror session: " << mirrorId.value();
      return;
    }
    mirrorOidList.push_back(mirrorHandle->adapterKey());
  }
  /*
   * case 1: Only mirroring:
   * Sample destination will be empty and no sample object will be created
   * INGRESS_MIRROR/EGRESS_MIRROR attribute will be set to the mirror OID
   * and INGRESS_SAMPLE_MIRROR/EGRESS_SAMPLE_MIRROR will be reset to
   * NULL object ID.
   *
   * case 2: Only sampling
   * When sample destination is CPU, then no mirroring object will be created.
   * A sample packet object will be created with destination as CPU and the
   * mirroring attributes on a port will be reset to NULL object ID.
   *
   * case 3: Mirroring and sampling
   * When sample destination is MIRROR, a sample object will be created with
   * destination as MIRROR. INGRESS_SAMPLE_MIRROR/EGRESS_SAMPLE_MIRROR will be
   * set with the right mirror OID and INGRESS_MIRROR/EGRESS_MIRROR attribute
   * will be reset to NULL object ID.
   *
   * NOTE: Some vendors do not free the SAI object unless the attributes
   * are set to NULL OID.
   */

  if (direction == MirrorDirection::INGRESS) {
    portHandle->port->setOptionalAttribute(
        SaiPortTraits::Attributes::IngressSampleMirrorSession{mirrorOidList});
  } else {
    portHandle->port->setOptionalAttribute(
        SaiPortTraits::Attributes::EgressSampleMirrorSession{mirrorOidList});
  }

  XLOG(DBG) << "Programming sampling mirror on port: " << std::hex
            << portHandle->port->adapterKey()
            << " action: " << (action == MirrorAction::START ? "start" : "stop")
            << " direction: "
            << (direction == MirrorDirection::INGRESS ? "ingress" : "egress");
}

void SaiPortManager::programMirrorOnAllPorts(
    const std::string& mirrorName,
    MirrorAction action) {
  /*
   * This is invoked by the mirror manager when a mirror session is
   * created or deleted. Based on the action and samplingMirror flag,
   * configure the right attribute.
   *
   * TODO: The ingress and egress mirror is cached in port handle
   * for this comparison. Query the mirror session ID from the port,
   * compare it with mirrorName adapterkey and update port if they
   * are the same.
   */
  for (const auto& portIdAndHandle : handles_) {
    auto& mirrorInfo = portIdAndHandle.second->mirrorInfo;
    if (mirrorInfo.getIngressMirror() == mirrorName) {
      if (mirrorInfo.isMirrorSampled()) {
        programSamplingMirror(
            portIdAndHandle.first,
            MirrorDirection::INGRESS,
            action,
            mirrorName);
      } else {
        programMirror(
            portIdAndHandle.first,
            MirrorDirection::INGRESS,
            action,
            mirrorName);
      }
    }
    if (mirrorInfo.getEgressMirror() == mirrorName) {
      if (mirrorInfo.isMirrorSampled()) {
        programSamplingMirror(
            portIdAndHandle.first, MirrorDirection::EGRESS, action, mirrorName);
      } else {
        programMirror(
            portIdAndHandle.first, MirrorDirection::EGRESS, action, mirrorName);
      }
    }
  }
}

void SaiPortManager::addBridgePort(const std::shared_ptr<Port>& port) {
  auto handle = getPortHandle(port->getID());
  CHECK(handle) << "unknown port " << port->getID();

  const auto& saiPort = handle->port;
  handle->bridgePort = managerTable_->bridgeManager().addBridgePort(
      SaiPortDescriptor(port->getID()),
      PortDescriptorSaiId(saiPort->adapterKey()));
}

void SaiPortManager::changeBridgePort(
    const std::shared_ptr<Port>& /*oldPort*/,
    const std::shared_ptr<Port>& newPort) {
  return addBridgePort(newPort);
}

bool SaiPortManager::isPortUp(PortID portID) const {
  auto handle = getPortHandle(portID);
  auto saiPortId = handle->port->adapterKey();
  // Need to get Oper State from SDK since it's not part of the create
  // attributes. Also in the event of link up/down, SDK would have the
  // accurate state.
  auto operStatus = SaiApiTable::getInstance()->portApi().getAttribute(
      saiPortId, SaiPortTraits::Attributes::OperStatus{});
  return GET_OPT_ATTR(Port, AdminState, handle->port->attributes()) &&
      utility::isPortOperUp(static_cast<sai_port_oper_status_t>(operStatus));
}

std::optional<SaiPortTraits::Attributes::PtpMode> SaiPortManager::getPtpMode()
    const {
  std::set<SaiPortTraits::Attributes::PtpMode> ptpModes;
  for (const auto& portIdAndHandle : handles_) {
    const auto ptpMode = SaiApiTable::getInstance()->portApi().getAttribute(
        portIdAndHandle.second->port->adapterKey(),
        SaiPortTraits::Attributes::PtpMode());
    ptpModes.insert(ptpMode);
  }

  if (ptpModes.size() > 1) {
    XLOG(WARN) << fmt::format(
        "all ports do not have same ptpMode: {}", ptpModes);
    return {};
  }

  // if handles_ is empty, treat as ptp disabled
  return ptpModes.empty() ? SAI_PORT_PTP_MODE_NONE : *ptpModes.begin();
}

bool SaiPortManager::isPtpTcEnabled() const {
  auto ptpMode = getPtpMode();
  return ptpMode && *ptpMode == SAI_PORT_PTP_MODE_SINGLE_STEP_TIMESTAMP;
}

void SaiPortManager::setPtpTcEnable(bool enable) {
  for (const auto& portIdAndHandle : handles_) {
    portIdAndHandle.second->port->setOptionalAttribute(
        SaiPortTraits::Attributes::PtpMode{utility::getSaiPortPtpMode(enable)});
  }
}

void SaiPortManager::programMacsec(
    const std::shared_ptr<Port>& oldPort,
    const std::shared_ptr<Port>& newPort) {
  CHECK(newPort);
  auto portId = newPort->getID();
  auto& macsecManager = managerTable_->macsecManager();

  auto oldMacsecDesired = oldPort ? oldPort->getMacsecDesired() : false;
  auto newMacsecDesired = newPort->getMacsecDesired();
  auto oldDropUnencrypted = oldPort ? oldPort->getDropUnencrypted() : false;
  auto newDropUnencrypted = newPort->getDropUnencrypted();

  // If macsecDesired changes from True to False then first we need to remove
  // any existing Rx/Tx SAK already present in this port and then later at the
  // bottom of this function, remove Macsec default config from port
  if (oldMacsecDesired && !newMacsecDesired) {
    XLOG(DBG2) << "programMacsec setting macsecDesired=false on port = "
               << newPort->getName() << ", Deleting all Rx and Tx SAK";
    CHECK(newPort->getRxSaksMap().empty());
    CHECK(!newPort->getTxSak().has_value());
  } else if (
      newMacsecDesired &&
      (!oldMacsecDesired || (oldDropUnencrypted != newDropUnencrypted))) {
    // If MacsecDesired changed to True or the dropUnencrypted value has
    // changed then configure dropUnencrypted as per the config
    macsecManager.setMacsecState(portId, true, newDropUnencrypted);
    XLOG(DBG2) << "programMacsec with macsecDesired=true on port = "
               << newPort->getName() << ", setting dropUnencrypted = "
               << (newDropUnencrypted ? "True" : "False");
  }

  // TX SAKs
  std::optional<mka::MKASak> oldTxSak =
      oldPort ? oldPort->getTxSak() : std::nullopt;
  std::optional<mka::MKASak> newTxSak = newPort->getTxSak();
  if (oldTxSak != newTxSak) {
    if (!newTxSak) {
      auto txSak = *oldTxSak;
      XLOG(DBG2) << "Deleting old Tx SAK for MAC="
                 << txSak.sci()->macAddress().value()
                 << " port=" << txSak.sci()->port().value();

      // We are about to prune MACSEC SAK/SCI, do a round of stat collection
      // to get SA, SCI counters since last stat collection. After delete,
      // we won't have access to this SAK/SCI counters
      updateStats(portId, false);
      macsecManager.deleteMacsec(
          portId, txSak, *txSak.sci(), SAI_MACSEC_DIRECTION_EGRESS);
    } else {
      // TX SAK mismatch between old and new. Reprogram SAK TX
      auto txSak = *newTxSak;
      std::optional<MacsecSASaiId> oldTxSaAdapter{std::nullopt};

      if (oldTxSak) {
        // The old Tx SAK is present and new Tx SAK needs to be added. This
        // new Tx SAK may or may not have same Sci as old one and this may or
        // may not have same AN as the old one. So delete the old Tx SAK first
        // and then add new Tx SAK
        auto oldSak = *oldTxSak;
        oldTxSaAdapter = macsecManager.getMacsecSaAdapterKey(
            portId,
            SAI_MACSEC_DIRECTION_EGRESS,
            oldSak.sci().value(),
            oldSak.assocNum().value());

        XLOG(DBG2) << "Updating Tx SAK by Deleting and Adding. MAC="
                   << oldSak.sci()->macAddress().value()
                   << " port=" << oldSak.sci()->port().value()
                   << " AN=" << oldSak.assocNum().value();
        // Delete the old Tx SAK in software first (by passing
        // skipHwUpdate=True) and then add the new Tx SAK (in software as well
        // as hardware). This new Tx SAK will replace the old SAK in hardware
        // without impacting the traffic. Later we will delete the old Tx SAK
        // SAI object from SAI driver
        macsecManager.deleteMacsec(
            portId, oldSak, *oldSak.sci(), SAI_MACSEC_DIRECTION_EGRESS, true);
      }
      XLOG(DBG2) << "Setup Egress Macsec for MAC="
                 << txSak.sci()->macAddress().value()
                 << " port=" << txSak.sci()->port().value();
      macsecManager.setupMacsec(
          portId, txSak, *txSak.sci(), SAI_MACSEC_DIRECTION_EGRESS);

      // If the old Tx SAK was deleted in software, then it would have got
      // overwritten in hardware by setupMacsec above. So now remove the old
      // Tx SAK object from SAI driver.
      if (oldTxSaAdapter.has_value()) {
        SaiApiTable::getInstance()->macsecApi().remove(oldTxSaAdapter.value());
      }
    }
  }
  // RX SAKS
  auto oldRxSaks = oldPort ? oldPort->getRxSaksMap() : PortFields::RxSaks{};
  auto newRxSaks = newPort->getRxSaksMap();
  for (const auto& keyAndSak : newRxSaks) {
    const auto& [key, sak] = keyAndSak;
    auto kitr = oldRxSaks.find(key);
    if (kitr == oldRxSaks.end() || sak != kitr->second) {
      // Either no SAK RX for this key before. Or the previous SAK with the
      // same key did not match the new SAK
      if (kitr != oldRxSaks.end()) {
        // There was a prev SAK with the same key. Delete it
        macsecManager.deleteMacsec(
            portId,
            kitr->second,
            kitr->first.sci,
            SAI_MACSEC_DIRECTION_INGRESS);
      }
      // Use the SCI from the key. Since for RX we use SCI of peer, which
      // is stored in MKASakKey
      XLOG(DBG2) << "Setup Ingress Macsec for MAC="
                 << key.sci.macAddress().value()
                 << " port=" << key.sci.port().value();
      macsecManager.setupMacsec(
          portId, sak, key.sci, SAI_MACSEC_DIRECTION_INGRESS);
    }
    // The RX SAK is already present so no need to reprogram or delete it
    oldRxSaks.erase(key);
  }
  // Erase whatever could not be found in newRxSaks
  for (const auto& keyAndSak : oldRxSaks) {
    const auto& [key, sak] = keyAndSak;
    // We are about to prune MACSEC SAK/SCI, do a round of stat collection
    // to get SA, SCI counters since last stat collection. After delete,
    // we won't have access to this SAK/SCI counters
    updateStats(portId, false);
    // Use the SCI from the key. Since for RX we use SCI of peer, which
    // is stored in MKASakKey
    XLOG(DBG2) << "Deleting old Rx SAK for MAC=" << key.sci.macAddress().value()
               << " port=" << key.sci.port().value();
    macsecManager.deleteMacsec(
        portId, sak, key.sci, SAI_MACSEC_DIRECTION_INGRESS);
  }
  // If macsecDesired changed to False then cleanup Macsec states including
  // ACL
  if (oldMacsecDesired && !newMacsecDesired) {
    macsecManager.setMacsecState(portId, false, false);
  }
}

std::vector<sai_port_lane_eye_values_t> SaiPortManager::getPortEyeValues(
    PortSaiId saiPortId) const {
  // Eye value is supported by only few Phy devices
  if (!platform_->getAsic()->isSupported(HwAsic::Feature::PORT_EYE_VALUES)) {
    return std::vector<sai_port_lane_eye_values_t>();
  }

  return SaiApiTable::getInstance()->portApi().getAttribute(
      saiPortId, SaiPortTraits::Attributes::PortEyeValues{});
}

#if SAI_API_VERSION >= SAI_VERSION(1, 13, 0)
std::vector<sai_port_frequency_offset_ppm_values_t> SaiPortManager::getRxPPM(
    PortSaiId saiPortId,
    uint8_t numPmdLanes) const {
  if (!rxFrequencyRPMSupported()) {
    return std::vector<sai_port_frequency_offset_ppm_values_t>();
  }

  return SaiApiTable::getInstance()->portApi().getAttribute(
      saiPortId,
      SaiPortTraits::Attributes::RxFrequencyPPM{
          std::vector<sai_port_frequency_offset_ppm_values_t>(numPmdLanes)});
}

std::vector<sai_port_snr_values_t> SaiPortManager::getRxSNR(
    PortSaiId saiPortId,
    uint8_t numPmdLanes) const {
  const auto portItr = concurrentIndices_->portSaiId2PortInfo.find(saiPortId);
  if (portItr == concurrentIndices_->portSaiId2PortInfo.cend()) {
    XLOG(WARNING) << "Unknown PortSaiId: " << saiPortId;
    return std::vector<sai_port_snr_values_t>();
  }
  // TH5 Management port doesn't support RX SNR
  // If we do end up with management ports supporting rxSNR we may need to
  // support per-core HwAsic::Feature definitions instead of setting them at the
  // asic level.
  auto portID = portItr->second.portID;
  if (getPortType(portID) == cfg::PortType::MANAGEMENT_PORT) {
    return std::vector<sai_port_snr_values_t>();
  }
  if (!rxSNRSupported()) {
    return std::vector<sai_port_snr_values_t>();
  }

  return SaiApiTable::getInstance()->portApi().getAttribute(
      saiPortId,
      SaiPortTraits::Attributes::RxSNR{
          std::vector<sai_port_snr_values_t>(numPmdLanes)});
}
#endif

#if SAI_API_VERSION >= SAI_VERSION(1, 10, 3) || defined(TAJO_SDK_VERSION_1_42_8)
std::vector<sai_port_lane_latch_status_t> SaiPortManager::getRxSignalDetect(
    PortSaiId saiPortId,
    uint8_t numPmdLanes,
    PortID portID) const {
  if (!platform_->getAsic()->isSupported(
          HwAsic::Feature::PMD_RX_SIGNAL_DETECT)) {
    return std::vector<sai_port_lane_latch_status_t>();
  }

  return SaiApiTable::getInstance()->portApi().getAttribute(
      saiPortId,
      SaiPortTraits::Attributes::RxSignalDetect{
          std::vector<sai_port_lane_latch_status_t>(numPmdLanes)});
}

std::vector<sai_port_lane_latch_status_t> SaiPortManager::getRxLockStatus(
    PortSaiId saiPortId,
    uint8_t numPmdLanes) const {
  if (!platform_->getAsic()->isSupported(HwAsic::Feature::PMD_RX_LOCK_STATUS)) {
    return std::vector<sai_port_lane_latch_status_t>();
  }

  return SaiApiTable::getInstance()->portApi().getAttribute(
      saiPortId,
      SaiPortTraits::Attributes::RxLockStatus{
          std::vector<sai_port_lane_latch_status_t>(numPmdLanes)});
}

std::vector<sai_port_lane_latch_status_t>
SaiPortManager::getFecAlignmentLockStatus(
    PortSaiId saiPortId,
    uint8_t numFecLanes) const {
  if (!platform_->getAsic()->isSupported(HwAsic::Feature::FEC_AM_LOCK_STATUS)) {
    return std::vector<sai_port_lane_latch_status_t>();
  }

  return SaiApiTable::getInstance()->portApi().getAttribute(
      saiPortId,
      SaiPortTraits::Attributes::FecAlignmentLock{
          std::vector<sai_port_lane_latch_status_t>(numFecLanes)});
}

std::optional<sai_latch_status_t> SaiPortManager::getPcsRxLinkStatus(
    PortSaiId saiPortId) const {
  if (!platform_->getAsic()->isSupported(HwAsic::Feature::PCS_RX_LINK_STATUS)) {
    return std::nullopt;
  }

  return SaiApiTable::getInstance()->portApi().getAttribute(
      saiPortId, SaiPortTraits::Attributes::PcsRxLinkStatus{});
}
#endif

#if SAI_API_VERSION >= SAI_VERSION(1, 10, 3)
std::optional<sai_latch_status_t> SaiPortManager::getHighCrcErrorRate(
    PortSaiId saiPortId,
    PortID swPort) const {
#if defined(BRCM_SAI_SDK_GTE_11_0)
  if (!platform_->getAsic()->isSupported(HwAsic::Feature::CRC_ERROR_DETECT) ||
      getPortType(swPort) != cfg::PortType::FABRIC_PORT) {
    // Feature is only applicable for fabric ports
    return std::nullopt;
  }
  return SaiApiTable::getInstance()->portApi().getAttribute(
      saiPortId, SaiPortTraits::Attributes::CrcErrorDetect{});
#else
  return std::nullopt;
#endif
}
#endif

void SaiPortManager::updateLeakyBucketFb303Counter(PortID portId, int value) {
  auto portStatItr = portStats_.find(portId);
  if (portStatItr == portStats_.end()) {
    throw FbossError("PortStats_ not available for : ", portId);
  }
  portStatItr->second->updateLeakyBucketFlapCnt(value);
}

std::vector<sai_port_err_status_t> SaiPortManager::getPortErrStatus(
    PortSaiId saiPortId) const {
  if (!platform_->getAsic()->isSupported(
          HwAsic::Feature::SAI_PORT_ERR_STATUS)) {
    return std::vector<sai_port_err_status_t>();
  }

  return SaiApiTable::getInstance()->portApi().getAttribute(
      saiPortId, SaiPortTraits::Attributes::PortErrStatus{});
}

phy::FecMode SaiPortManager::getFECMode(PortID portId) const {
  auto handle = getPortHandle(portId);
  auto saiFecMode = GET_OPT_ATTR(Port, FecMode, handle->port->attributes());
  auto profileID = platform_->getPort(portId)->getCurrentProfile();
  return utility::getFecModeFromSaiFecMode(
      static_cast<sai_port_fec_mode_t>(saiFecMode), profileID);
}

cfg::PortSpeed SaiPortManager::getSpeed(PortID portId) const {
  auto handle = getPortHandle(portId);
  return static_cast<cfg::PortSpeed>(
      GET_ATTR(Port, Speed, handle->port->attributes()));
}

phy::InterfaceType SaiPortManager::getInterfaceType(PortID portID) const {
  if (!platform_->getAsic()->isSupported(
          HwAsic::Feature::PORT_INTERFACE_TYPE)) {
    return phy::InterfaceType::NONE;
  }
  auto saiInterfaceType = GET_OPT_ATTR(
      Port, InterfaceType, getPortHandle(portID)->port->attributes());
  return fromSaiInterfaceType(
      static_cast<sai_port_interface_type_t>(saiInterfaceType));
}

TransmitterTechnology SaiPortManager::getMedium(PortID portID) const {
  if (!platform_->getAsic()->isSupported(HwAsic::Feature::MEDIA_TYPE)) {
    return TransmitterTechnology::UNKNOWN;
  }
  if (platform_->getAsic()->getAsicType() ==
      cfg::AsicType::ASIC_TYPE_SANDIA_PHY) {
    return TransmitterTechnology::OPTICAL;
  }
  auto saiMediaType =
      GET_OPT_ATTR(Port, MediaType, getPortHandle(portID)->port->attributes());
  return fromSaiMediaType(static_cast<sai_port_media_type_t>(saiMediaType));
}

uint8_t SaiPortManager::getNumPmdLanes(PortSaiId saiPortId) const {
#if defined(BRCM_SAI_SDK_XGS)
  std::vector<uint32_t> lanes;
  if (hwLaneListIsPmdLaneList_) {
    lanes = SaiApiTable::getInstance()->portApi().getAttribute(
        saiPortId, SaiPortTraits::Attributes::HwLaneList{});
  } else {
    lanes = SaiApiTable::getInstance()->portApi().getAttribute(
        saiPortId, SaiPortTraits::Attributes::SerdesLaneList{});
  }
#else
  auto lanes = SaiApiTable::getInstance()->portApi().getAttribute(
      saiPortId, SaiPortTraits::Attributes::HwLaneList{});
#endif
  return lanes.size();
}

void SaiPortManager::resetQueues() {
  for (auto& idAndHandle : handles_) {
    idAndHandle.second->resetQueues();
  }
}

void SaiPortManager::changeRxLaneSquelch(
    const std::shared_ptr<Port>& oldPort,
    const std::shared_ptr<Port>& newPort) {
  if (oldPort->getRxLaneSquelch() != newPort->getRxLaneSquelch()) {
    // On DNX platforms, fabric ports come up when the RX alone is UP. In
    // these platforms, setting preemphasis to 0 doesn't bring the port down
    // when there is an active remote partner. If the RX_LANE_SQUELCH_ENABLE
    // is supported, set that true which will cutoff any signal coming from
    // the remote side and bring down the local link
    if ((newPort->getPortType() == cfg::PortType::FABRIC_PORT ||
         newPort->getPortType() == cfg::PortType::MANAGEMENT_PORT ||
         newPort->getPortType() == cfg::PortType::INTERFACE_PORT) &&
        platform_->getAsic()->isSupported(
            HwAsic::Feature::RX_LANE_SQUELCH_ENABLE)) {
      auto portHandle = getPortHandle(newPort->getID());
      if (!portHandle) {
        throw FbossError(
            "Cannot set Rx Lane Squelch on non existent port: ",
            newPort->getID());
      }
      portHandle->port->setOptionalAttribute(
          SaiPortTraits::Attributes::RxLaneSquelchEnable{
              newPort->getRxLaneSquelch()});
    }
  }
}

void SaiPortManager::reloadSixTapAttributes(
    SaiPortHandle* portHandle,
    SaiPortSerdesTraits::CreateAttributes& attr) {
  const auto& portApi = SaiApiTable::getInstance()->portApi();
  auto setTxRxAttr = [](auto& attrs, auto type, const auto& val) {
    auto& attr = std::get<std::optional<std::decay_t<decltype(type)>>>(attrs);
    if (!val.empty()) {
      attr = val;
    }
  };
  auto txPre1 = portApi.getAttribute(
      portHandle->serdes->adapterKey(),
      SaiPortSerdesTraits::Attributes::TxFirPre1{});
  auto main = portApi.getAttribute(
      portHandle->serdes->adapterKey(),
      SaiPortSerdesTraits::Attributes::TxFirMain{});
  auto txPost1 = portApi.getAttribute(
      portHandle->serdes->adapterKey(),
      SaiPortSerdesTraits::Attributes::TxFirPost1{});
  setTxRxAttr(attr, SaiPortSerdesTraits::Attributes::TxFirPre1{}, txPre1);
  setTxRxAttr(attr, SaiPortSerdesTraits::Attributes::TxFirMain{}, main);
  setTxRxAttr(attr, SaiPortSerdesTraits::Attributes::TxFirPost1{}, txPost1);

  if (FLAGS_sai_configure_six_tap &&
      platform_->getAsic()->isSupported(
          HwAsic::Feature::SAI_CONFIGURE_SIX_TAP)) {
    auto txPost2 = portApi.getAttribute(
        portHandle->serdes->adapterKey(),
        SaiPortSerdesTraits::Attributes::TxFirPost2{});
    auto txPost3 = portApi.getAttribute(
        portHandle->serdes->adapterKey(),
        SaiPortSerdesTraits::Attributes::TxFirPost3{});
    auto txPre2 = portApi.getAttribute(
        portHandle->serdes->adapterKey(),
        SaiPortSerdesTraits::Attributes::TxFirPre2{});
    setTxRxAttr(attr, SaiPortSerdesTraits::Attributes::TxFirPost2{}, txPost2);
    setTxRxAttr(attr, SaiPortSerdesTraits::Attributes::TxFirPost3{}, txPost3);
    setTxRxAttr(attr, SaiPortSerdesTraits::Attributes::TxFirPre2{}, txPre2);
  }
}

void SaiPortManager::changeZeroPreemphasis(
    const std::shared_ptr<Port>& oldPort,
    const std::shared_ptr<Port>& newPort) {
  if (oldPort->getZeroPreemphasis() != newPort->getZeroPreemphasis()) {
    if (!newPort->getZeroPreemphasis()) {
      throw FbossError("Reverting zero preemphasis on port is not supported.");
    }
    auto portHandle = getPortHandle(newPort->getID());
    if (!portHandle) {
      throw FbossError(
          "Cannot set zero preemphasis on non existent port: ",
          newPort->getID());
    }

    // Check if the platform supports setting zero preemphasis.
    // TH4 and TH5 starts supporting zero preemphasis starting 11.0
#if defined(BRCM_SAI_SDK_GTE_11_0)
    bool supportsZeroPreemphasis =
        platform_->getAsic()->isSupported(
            HwAsic::Feature::PORT_SERDES_ZERO_PREEMPHASIS) ||
        platform_->getAsic()->getAsicType() ==
            cfg::AsicType::ASIC_TYPE_TOMAHAWK4 ||
        platform_->getAsic()->getAsicType() ==
            cfg::AsicType::ASIC_TYPE_TOMAHAWK5;
#else
    bool supportsZeroPreemphasis = platform_->getAsic()->isSupported(
        HwAsic::Feature::PORT_SERDES_ZERO_PREEMPHASIS);
#endif

    if (!supportsZeroPreemphasis) {
      return;
    }

    auto serDesAttributes = serdesAttributesFromSwPinConfigs(
        portHandle->port->adapterKey(),
        newPort->getPinConfigs(),
        portHandle->serdes,
        newPort->getZeroPreemphasis());
    if (platform_->isSerdesApiSupported() &&
        platform_->getAsic()->isSupported(
            HwAsic::Feature::SAI_PORT_SERDES_PROGRAMMING)) {
#ifdef TAJO_SAI_SDK
      // TAJO requires recreating serdes object to zero peremphasis.
      SaiPortSerdesTraits::AdapterHostKey serdesKey{
          portHandle->port->adapterKey()};
      auto& store = saiStore_->get<SaiPortSerdesTraits>();
      auto serdes = store.get(serdesKey);
      portHandle->serdes.reset();
      serdes.reset();
      portHandle->serdes = store.setObject(serdesKey, serDesAttributes);
#else
      // Brcm enforces main tap to be greater than all attributes.
      // Hence set other attributes first, and then set main to zero.
      auto nonZeroMainAttribute = serDesAttributes;
      auto txMain =
          std::get<std::optional<SaiPortSerdesTraits::Attributes::TxFirMain>>(
              portHandle->serdes->attributes());
      if (txMain.has_value()) {
        std::get<std::optional<SaiPortSerdesTraits::Attributes::TxFirMain>>(
            nonZeroMainAttribute) = txMain.value().value();
      } else {
        std::get<std::optional<SaiPortSerdesTraits::Attributes::TxFirMain>>(
            nonZeroMainAttribute) = std::nullopt;
      }
      portHandle->serdes->setAttributes(nonZeroMainAttribute);
      portHandle->serdes->setAttributes(serDesAttributes);
#endif
    }
  }
}

void SaiPortManager::changeTxEnable(
    const std::shared_ptr<Port>& oldPort,
    const std::shared_ptr<Port>& newPort) {
  if (oldPort->getTxEnable() != newPort->getTxEnable()) {
    auto portHandle = getPortHandle(newPort->getID());
    if (!portHandle) {
      throw FbossError(
          "Cannot change tx enable on non existent port: ", newPort->getID());
    }
    portHandle->port->setOptionalAttribute(
        SaiPortTraits::Attributes::PktTxEnable{
            newPort->getTxEnable().has_value() ? newPort->getTxEnable().value()
                                               : false});
  }
}

void SaiPortManager::addPortShelEnable(
    const std::shared_ptr<Port>& swPort) const {
#if defined(BRCM_SAI_SDK_DNX_GTE_11_0)
  if (!swPort->getSelfHealingECMPLagEnable().has_value()) {
    return;
  }
  const auto portHandle = getPortHandle(swPort->getID());
  CHECK(portHandle);

  // Load current SDK value into SaiStore - this will avoid unnecessary hw
  // writes.
  auto gotShelEnable = SaiApiTable::getInstance()->portApi().getAttribute(
      portHandle->port->adapterKey(), SaiPortTraits::Attributes::ShelEnable{});
  std::optional<SaiPortTraits::Attributes::ShelEnable> shelEnableAttr =
      gotShelEnable;
  portHandle->port->setAttribute(shelEnableAttr, true /* skipHwWrite */);

  shelEnableAttr = swPort->getSelfHealingECMPLagEnable();
  portHandle->port->setAttribute(shelEnableAttr);
#endif
}

void SaiPortManager::changePortShelEnable(
    const std::shared_ptr<Port>& oldPort,
    const std::shared_ptr<Port>& newPort) const {
#if defined(BRCM_SAI_SDK_DNX_GTE_11_0)
  if (oldPort->getSelfHealingECMPLagEnable() !=
      newPort->getSelfHealingECMPLagEnable()) {
    const auto portHandle = getPortHandle(newPort->getID());
    CHECK(portHandle);

    // Load current SDK value into SaiStore - this will avoid unnecessary hw
    // writes.
    auto gotShelEnable = SaiApiTable::getInstance()->portApi().getAttribute(
        portHandle->port->adapterKey(),
        SaiPortTraits::Attributes::ShelEnable{});
    std::optional<SaiPortTraits::Attributes::ShelEnable> shelEnableAttr =
        gotShelEnable;
    portHandle->port->setAttribute(shelEnableAttr, true /* skipHwWrite */);

    shelEnableAttr = newPort->getSelfHealingECMPLagEnable();
    portHandle->port->setAttribute(shelEnableAttr);
  }
#endif
}
/**
 * Increment the PFC counter for a given port and counter type.
 *
 * @param portId - The ID of the port for which the counter is to be
 * incremented.
 * @param counterType - The type of PFC counter to increment (DEADLOCK or
 * RECOVERY).
 */
void SaiPortManager::incrementPfcCounter(
    const PortID& portId,
    PfcCounterType counterType) {
  auto portStatItr = portStats_.find(portId);
  if (portStatItr == portStats_.end()) {
    // No stats exist, nothing to do.
    return;
  }
  auto curPortStats = portStatItr->second->portStats();

  // Increment the appropriate counter based on the counter type.
  auto now = duration_cast<seconds>(system_clock::now().time_since_epoch());
  if (counterType == PfcCounterType::DEADLOCK) {
    if (!curPortStats.pfcDeadlockDetection_().has_value()) {
      // Make sure the counter is initialized to 0 if it doesn't exist.
      curPortStats.pfcDeadlockDetection_() = 0;
      portStatItr->second->updateStats(curPortStats, now);
    }
    curPortStats.pfcDeadlockDetection_() =
        *curPortStats.pfcDeadlockDetection_() + 1;
  } else { // PfcCounterType::RECOVERY
    if (!curPortStats.pfcDeadlockRecovery_().has_value()) {
      // Make sure the counter is initialized to 0 if it doesn't exist.
      curPortStats.pfcDeadlockRecovery_() = 0;
      portStatItr->second->updateStats(curPortStats, now);
    }
    curPortStats.pfcDeadlockRecovery_() =
        *curPortStats.pfcDeadlockRecovery_() + 1;
  }

  portStatItr->second->updateStats(curPortStats, now);
}

/**
 * Increment the PFC deadlock counter for a given port.
 *
 * @param portId - The ID of the port for which the deadlock counter is to be
 * incremented.
 */
void SaiPortManager::incrementPfcDeadlockCounter(const PortID& portId) {
  incrementPfcCounter(portId, PfcCounterType::DEADLOCK);
}

/**
 * Increment the PFC recovery counter for a given port.
 *
 * @param portId - The ID of the port for which the recovery counter is to be
 * incremented.
 */
void SaiPortManager::incrementPfcRecoveryCounter(const PortID& portId) {
  incrementPfcCounter(portId, PfcCounterType::RECOVERY);
}
} // namespace facebook::fboss
