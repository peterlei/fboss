/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "fboss/qsfp_service/module/QsfpModule.h"

#include <boost/assign.hpp>
#include <fboss/lib/phy/gen-cpp2/phy_types.h>
#include <iomanip>
#include <string>
#include "common/time/Time.h"
#include "fboss/agent/FbossError.h"
#include "fboss/lib/phy/gen-cpp2/phy_types.h"
#include "fboss/lib/usb/TransceiverI2CApi.h"
#include "fboss/qsfp_service/TransceiverManager.h"
#include "fboss/qsfp_service/if/gen-cpp2/transceiver_types.h"
#include "fboss/qsfp_service/module/TransceiverImpl.h"

#include <folly/io/IOBuf.h>
#include <folly/io/async/EventBase.h>
#include <folly/logging/xlog.h>

DEFINE_int32(
    qsfp_data_refresh_interval,
    10,
    "how often to refetch qsfp data that changes frequently");
DEFINE_int32(
    customize_interval,
    30,
    "minimum interval between customizing the same down port twice");
DEFINE_int32(
    remediate_interval,
    360,
    "seconds between running more destructive remediations on down ports");
DEFINE_int32(
    initial_remediate_interval,
    120,
    "seconds to wait before running first destructive remediations on down ports after bootup");

using folly::IOBuf;
using std::lock_guard;
using std::memcpy;
using std::mutex;

namespace facebook {
namespace fboss {

// Module state machine Timeout (seconds) for Agent to qsfp_service port status
// sync up first time
static constexpr int kStateMachineAgentPortSyncupTimeout = 120;
// Module State machine optics remediation/bringup interval (seconds)
static constexpr int kStateMachineOpticsRemediateInterval = 30;

TransceiverID QsfpModule::getID() const {
  return TransceiverID(qsfpImpl_->getNum());
}

std::string QsfpModule::getNameString() const {
  return qsfpImpl_->getName().toString();
}

// Converts power from milliwatts to decibel-milliwatts
double QsfpModule::mwToDb(double value) {
  if (value <= 0.01) {
    return -40;
  }
  return 10 * log10(value);
};

/*
 * Given a byte, extract bit fields for various alarm flags;
 * note the we might want to use the lower or the upper nibble,
 * so offset is the number of the bit to start at;  this is usually
 * 0 or 4.
 */

FlagLevels QsfpModule::getQsfpFlags(const uint8_t* data, int offset) {
  FlagLevels flags;

  CHECK_GE(offset, 0);
  CHECK_LE(offset, 4);
  flags.warn()->low() = (*data & (1 << offset));
  flags.warn()->high() = (*data & (1 << ++offset));
  flags.alarm()->low() = (*data & (1 << ++offset));
  flags.alarm()->high() = (*data & (1 << ++offset));

  return flags;
}

QsfpModule::QsfpModule(
    TransceiverManager* transceiverManager,
    std::unique_ptr<TransceiverImpl> qsfpImpl)
    : Transceiver(transceiverManager),
      qsfpImpl_(std::move(qsfpImpl)),
      snapshots_(
          TransceiverSnapshotCache(transceiverManager->getPortNames(getID()))) {
  markLastDownTime();
}

QsfpModule::~QsfpModule() {}

void QsfpModule::removeTransceiver() {
  lock_guard<std::mutex> g(qsfpModuleMutex_);
  removeTransceiverLocked();
}

void QsfpModule::removeTransceiverLocked() {
  getTransceiverManager()->updateStateBlocking(
      getID(), TransceiverStateMachineEvent::TCVR_EV_REMOVE_TRANSCEIVER);
}

void QsfpModule::getQsfpValue(
    int dataAddress,
    int offset,
    int length,
    uint8_t* data) const {
  const uint8_t* ptr = getQsfpValuePtr(dataAddress, offset, length);

  memcpy(data, ptr, length);
}

std::optional<cfg::Firmware> QsfpModule::getFirmwareFromCfg() const {
  auto qsfpCfgRaw = getTransceiverManager()->getQsfpConfig();
  if (!qsfpCfgRaw) {
    QSFP_LOG(DBG4, this) << "qsfpConfig is NULL. No Firmware to return";
    return std::nullopt;
  }

  const auto& qsfpCfg = qsfpCfgRaw->thrift;
  auto qsfpCfgFw = qsfpCfg.transceiverFirmwareVersions();
  if (!qsfpCfgFw.has_value()) {
    QSFP_LOG(DBG4, this)
        << "transceiverFirmwareVersions is NULL. No Firmware to return";
    return std::nullopt;
  }

  auto cachedTcvrInfo = getTransceiverInfo();
  auto vendor = cachedTcvrInfo.tcvrState()->vendor();
  if (!vendor.has_value()) {
    QSFP_LOG(DBG4, this) << "Vendor not set. No Firmware to return";
    return std::nullopt;
  }

  auto fwVersionInCfgIt =
      qsfpCfgFw->versionsMap()->find(vendor->get_partNumber());
  if (fwVersionInCfgIt == qsfpCfgFw->versionsMap()->end()) {
    QSFP_LOG(DBG4, this) << folly::sformat(
        "transceiverFirmwareVersions doesn't have a firmware version for part number {:s}.  No Firmware to return",
        vendor->get_partNumber());
    return std::nullopt;
  }

  return fwVersionInCfgIt->second;
}

bool QsfpModule::requiresFirmwareUpgrade() const {
  // Returns true if the current firmware revision is different than the one in
  // qsfp config
  auto cachedTcvrInfo = getTransceiverInfo();
  auto moduleStatus = cachedTcvrInfo.tcvrState()->status();
  if (!moduleStatus.has_value()) {
    QSFP_LOG(DBG4, this)
        << "moduleStatus not set. Returning false from requiresFirmwareUpgrade";
    return false;
  }

  auto fwStatus = moduleStatus->fwStatus();
  if (!fwStatus.has_value()) {
    QSFP_LOG(DBG4, this)
        << "fwStatus not set. Returning false from requiresFirmwareUpgrade";
    return false;
  }

  auto fwFromConfig = getFirmwareFromCfg();
  if (!fwFromConfig.has_value()) {
    QSFP_LOG(DBG4, this)
        << "Fw not available in config. Returning false from requiresFirmwareUpgrade";
    return false;
  }

  for (auto fwIt : *fwFromConfig->versions()) {
    if (fwIt.get_fwType() == cfg::FirmwareType::APPLICATION &&
        fwStatus->version() && fwIt.get_version() != *fwStatus->version()) {
      QSFP_LOG(INFO, this) << folly::sformat(
          "Application Version in cfg = {:s}, current operational version = {:s}. Returning true from requiresFirmwareUpgrade",
          fwIt.get_version(),
          *fwStatus->version());
      return true;
    }
    if (fwIt.get_fwType() == cfg::FirmwareType::DSP && fwStatus->dspFwVer() &&
        fwIt.get_version() != *fwStatus->dspFwVer()) {
      QSFP_LOG(INFO, this) << folly::sformat(
          "DSP Version in cfg = {:s}, current operational version = {:s}. Returning true from requiresFirmwareUpgrade",
          fwIt.get_version(),
          *fwStatus->dspFwVer());
      return true;
    }
  }

  // Versions match. No need to upgrade firmware
  return false;
}

bool QsfpModule::upgradeFirmware(const std::optional<cfg::Firmware>& fw) {
  // Always use i2cEvb to program transceivers if there's an i2cEvb
  auto i2cEvb = qsfpImpl_->getI2cEventBase();
  auto upgradeFwFn = [&, fw]() -> bool {
    lock_guard<std::mutex> g(qsfpModuleMutex_);
    return upgradeFirmwareLocked(fw);
  };

  if (!i2cEvb) {
    try {
      return upgradeFwFn();
    } catch (const std::exception& ex) {
      QSFP_LOG(DBG2, this) << "Error calling upgradeFirmwareLocked(): "
                           << ex.what();
    }
    return false;
  }

  bool fwUpgradeStatus = false;
  via(i2cEvb)
      .thenValue([&, upgradeFwFn](auto&&) mutable {
        try {
          fwUpgradeStatus = upgradeFwFn();
        } catch (const std::exception& ex) {
          QSFP_LOG(DBG2, this)
              << "Error calling upgradeFirmwareLocked(): " << ex.what();
        }
      })
      .get();
  return fwUpgradeStatus;
}

bool QsfpModule::upgradeFirmwareLocked(const std::optional<cfg::Firmware>& fw) {
  QSFP_LOG(INFO, this) << "Upgrading firmware";

  // Mark the module dirty so that we can refresh the entire cache later
  dirty_ = true;

  cfg::Firmware fwToUpgrade;
  if (fw.has_value()) {
    fwToUpgrade = fw.value();
  } else if (auto fwFromConfig = getFirmwareFromCfg()) {
    QSFP_LOG(INFO, this) << "Upgrading firmware to the one in qsfp config";
    fwToUpgrade = *fwFromConfig;
  } else {
    QSFP_LOG(ERR, this) << "No firmware version found to upgrade";
    return false;
  }

  auto cachedTcvrInfo = getTransceiverInfo();
  auto vendor = cachedTcvrInfo.tcvrState()->vendor();
  if (!vendor.has_value()) {
    QSFP_LOG(ERR, this)
        << "Vendor not set. Can't find the partnumber to upgrade";
    return false;
  }

  std::string fwStorageHandleName =
      getFwStorageHandle(vendor->get_partNumber());

  if (fwStorageHandleName.empty()) {
    QSFP_LOG(ERR, this)
        << "Can't find the fwStorage handle for this part number. Skipping fw upgrade";
    return false;
  }

  bool fwUpgradeResult = true;
  lastFwUpgradeStartTime_ = std::time(nullptr);
  for (const auto& fwVersion : *fwToUpgrade.versions()) {
    QSFP_LOG(INFO, this) << folly::sformat(
        "Upgrading module firmware. Type={:s}, Version={:s}",
        apache::thrift::util::enumNameSafe(*fwVersion.fwType()),
        *fwVersion.version());

    std::unique_ptr<FbossFirmware> fbossFw =
        getTransceiverManager()->fwStorage()->getFirmware(
            fwStorageHandleName, *fwVersion.version());
    fwUpgradeResult &= upgradeFirmwareLockedImpl(std::move(fbossFw));
  }

  // Trigger a hard reset of the transceiver to kick start the new firmware
  triggerModuleResetLocked();

  lastFwUpgradeEndTime_ = std::time(nullptr);
  auto elapsedSeconds = lastFwUpgradeEndTime_ - lastFwUpgradeStartTime_;

  QSFP_LOG(INFO, this) << "Firmware upgrade completed "
                       << (fwUpgradeResult ? "successfully" : "unsuccessfully")
                       << " in " << elapsedSeconds << " seconds. ";
  return fwUpgradeResult;
}

void QsfpModule::triggerModuleResetLocked() {
  getTransceiverManager()->getQsfpPlatformApi()->triggerQsfpHardReset(
      static_cast<unsigned int>(getID()) + 1);
  moduleResetCounter_++;
}

// Note that this needs to be called while holding the
// qsfpModuleMutex_
bool QsfpModule::cacheIsValid() const {
  return present_ && !dirty_;
}

TransceiverInfo QsfpModule::getTransceiverInfo() const {
  auto cachedInfo = info_.rlock();
  if (!cachedInfo->has_value()) {
    throw QsfpModuleError("Still populating data...");
  }
  return **cachedInfo;
}

Transceiver::TransceiverPresenceDetectionStatus QsfpModule::detectPresence() {
  lock_guard<std::mutex> g(qsfpModuleMutex_);
  return detectPresenceLocked();
}

Transceiver::TransceiverPresenceDetectionStatus
QsfpModule::detectPresenceLocked() {
  auto currentQsfpStatus = qsfpImpl_->detectTransceiver();
  bool statusChanged{false};
  if (currentQsfpStatus != present_) {
    QSFP_LOG(DBG1, this) << "QSFP status changed from "
                         << (present_ ? "PRESENT" : "NOT PRESENT") << " to "
                         << (currentQsfpStatus ? "PRESENT" : "NOT PRESENT");
    dirty_ = true;
    present_ = currentQsfpStatus;
    statusChanged = true;
    moduleResetCounter_ = 0;

    // If a transceiver went from present to missing, clear the cached data.
    if (!present_) {
      info_.wlock()->reset();
    }

    // In the case of an OBO module or an inaccessable present module,
    // we need to fill in the essential info before parsing the DOM data
    // which may not be available.
    TransceiverInfo info;

    auto& tcvrState = info.tcvrState().ensure();
    tcvrState.present() = present_;
    tcvrState.transceiver() = type();
    tcvrState.port() = qsfpImpl_->getNum();

    *info_.wlock() = info;
  }
  return {currentQsfpStatus, statusChanged};
}

unsigned int QsfpModule::numHostLanes() const {
  switch (getModuleMediaInterface()) {
    case MediaInterfaceCode::LR_10G:
    case MediaInterfaceCode::SR_10G:
    case MediaInterfaceCode::BASE_T_10G:
      return 1;
    case MediaInterfaceCode::CWDM4_100G:
    case MediaInterfaceCode::CR4_100G:
    case MediaInterfaceCode::FR1_100G:
    case MediaInterfaceCode::FR4_200G:
    case MediaInterfaceCode::CR4_200G:
      return 4;
    case MediaInterfaceCode::FR4_400G:
    case MediaInterfaceCode::LR4_400G_10KM:
    case MediaInterfaceCode::CR8_400G:
    case MediaInterfaceCode::FR4_2x400G:
      return 8;
    case MediaInterfaceCode::UNKNOWN:
      return 0;
    default:
      throw QsfpModuleError("invalid module media interface");
  }
}

unsigned int QsfpModule::numMediaLanes() const {
  switch (getModuleMediaInterface()) {
    case MediaInterfaceCode::LR_10G:
    case MediaInterfaceCode::SR_10G:
    case MediaInterfaceCode::FR1_100G:
    case MediaInterfaceCode::BASE_T_10G:
      return 1;
    case MediaInterfaceCode::CWDM4_100G:
    case MediaInterfaceCode::CR4_100G:
    case MediaInterfaceCode::FR4_200G:
    case MediaInterfaceCode::CR4_200G:
    case MediaInterfaceCode::FR4_400G:
    case MediaInterfaceCode::LR4_400G_10KM:
      return 4;
    case MediaInterfaceCode::CR8_400G:
    case MediaInterfaceCode::FR4_2x400G:
      return 8;
    case MediaInterfaceCode::UNKNOWN:
      return 0;
    default:
      throw QsfpModuleError("invalid module media interface");
  }
}

void QsfpModule::updateCachedTransceiverInfoLocked(ModuleStatus moduleStatus) {
  // Migration plan from fields in TransceiverInfo to being inside state/states:
  // 1. Populate data to old fields then populate state/states from old fields
  // 2. Migrate users to new fields.
  // 3. Populate data to new fields then populate old fields from new fields
  // guarded by a just knob.
  // 4. Use the knob to slowly turn off old fields, check that nobody dies.
  // 5. Remove old fields
  //
  // We're currently at step 1.

  TransceiverInfo info;

  auto& tcvrState = *info.tcvrState();
  auto& tcvrStats = *info.tcvrStats();

  tcvrState.present() = present_;
  tcvrState.transceiver() = type();
  tcvrState.port() = qsfpImpl_->getNum();

  if (present_) {
    auto nMediaLanes = numMediaLanes();
    tcvrState.mediaLaneSignals() = std::vector<MediaLaneSignals>(nMediaLanes);
    if (!getSignalsPerMediaLane(*tcvrState.mediaLaneSignals())) {
      tcvrState.mediaLaneSignals()->clear();
    } else {
      cacheMediaLaneSignals(*tcvrState.mediaLaneSignals());
    }

    tcvrStats.sensor() = getSensorInfo();
    tcvrState.vendor() = getVendorInfo();
    tcvrState.cable() = getCableInfo();
    if (auto threshold = getThresholdInfo()) {
      tcvrState.thresholds() = *threshold;
    }
    tcvrState.settings() = getTransceiverSettingsInfo();

    for (int i = 0; i < nMediaLanes; i++) {
      Channel chan;
      chan.channel() = i;
      tcvrStats.channels()->push_back(chan);
    }
    if (!getSensorsPerChanInfo(*tcvrStats.channels())) {
      tcvrStats.channels()->clear();
    }

    tcvrState.hostLaneSignals() = std::vector<HostLaneSignals>(numHostLanes());
    if (!getSignalsPerHostLane(*tcvrState.hostLaneSignals())) {
      tcvrState.hostLaneSignals()->clear();
    }

    if (auto transceiverStats = getTransceiverStats()) {
      tcvrStats.stats() = *transceiverStats;
    }

    tcvrState.signalFlag() = getSignalFlagInfo();
    cacheSignalFlags(*tcvrState.signalFlag());

    if (auto extSpecCompliance = getExtendedSpecificationComplianceCode()) {
      tcvrState.extendedSpecificationComplianceCode() = *extSpecCompliance;
    }
    tcvrState.transceiverManagementInterface() = managementInterface();

    tcvrState.identifier() = getIdentifier();
    auto currentStatus = getModuleStatus();
    // Use the input `moduleStatus` as the reference to update the
    // `cmisStateChanged` for currentStatus, which will be used in the
    // TransceiverInfo
    updateCmisStateChanged(currentStatus, moduleStatus);
    tcvrState.status() = currentStatus;
    cacheStatusFlags(currentStatus);

    // If the StatsPublisher thread has triggered the VDM data capture then
    // latch, read data (page 24 and 25), release latch
    if (captureVdmStats_) {
      latchAndReadVdmDataLocked();
    }

    if (auto vdmStats = getVdmDiagsStatsInfo()) {
      tcvrStats.vdmDiagsStats() = *vdmStats;

      // If the StatsPublisher thread has triggered the VDM data capture then
      // capure this data into transceiverInfo cache
      if (captureVdmStats_) {
        tcvrStats.vdmDiagsStatsForOds() = *vdmStats;
      } else {
        // If the VDM is not updated in this cycle then retain older values
        auto cachedTcvrInfo = getTransceiverInfo();
        if (cachedTcvrInfo.tcvrStats()->vdmDiagsStatsForOds()) {
          tcvrStats.vdmDiagsStatsForOds() =
              cachedTcvrInfo.tcvrStats()->vdmDiagsStatsForOds().value();
        }
      }
      captureVdmStats_ = false;
    }

    tcvrState.timeCollected() = lastRefreshTime_;
    tcvrStats.timeCollected() = lastRefreshTime_;

    tcvrStats.remediationCounter() = numRemediation_;
    tcvrState.eepromCsumValid() = verifyEepromChecksums();
    tcvrState.moduleMediaInterface() = getModuleMediaInterface();

    for (auto it : getPortNameToHostLanes()) {
      tcvrState.portNameToHostLanes()[it.first] =
          std::vector<int>(it.second.begin(), it.second.end());
    }
    tcvrStats.portNameToHostLanes() = *tcvrState.portNameToHostLanes();

    for (auto it : getPortNameToMediaLanes()) {
      tcvrState.portNameToMediaLanes()[it.first] =
          std::vector<int>(it.second.begin(), it.second.end());
    }
    tcvrStats.portNameToMediaLanes() = *tcvrState.portNameToMediaLanes();

    auto diagCapability = getDiagsCapability();
    if (diagCapability.has_value()) {
      tcvrState.diagCapability() = diagCapability.value();
    }
  }

  tcvrStats.lastFwUpgradeStartTime() = lastFwUpgradeStartTime_;
  tcvrStats.lastFwUpgradeEndTime() = lastFwUpgradeEndTime_;

  phy::LinkSnapshot snapshot;
  snapshot.transceiverInfo_ref() = info;
  snapshots_.wlock()->addSnapshot(snapshot);
  *info_.wlock() = info;
}

bool QsfpModule::customizationSupported() const {
  // Customization is allowed on present Optical modules only. We should skip
  // other types
  auto tech = getQsfpTransmitterTechnology();
  return present_ && tech == TransmitterTechnology::OPTICAL;
}

bool QsfpModule::shouldRefresh(time_t cooldown) const {
  return std::time(nullptr) - lastRefreshTime_ >= cooldown;
}

void QsfpModule::ensureOutOfReset() const {
  qsfpImpl_->ensureOutOfReset();
  QSFP_LOG(DBG3, this) << "Cleared the reset register of QSFP.";
}

void QsfpModule::cacheSignalFlags(const SignalFlags& signalflag) {
  signalFlagCache_.txLos() = *signalflag.txLos() | *signalFlagCache_.txLos();
  signalFlagCache_.rxLos() = *signalflag.rxLos() | *signalFlagCache_.rxLos();
  signalFlagCache_.txLol() = *signalflag.txLol() | *signalFlagCache_.txLol();
  signalFlagCache_.rxLol() = *signalflag.rxLol() | *signalFlagCache_.rxLol();
}

void QsfpModule::cacheStatusFlags(const ModuleStatus& status) {
  if (moduleStatusCache_.cmisStateChanged() && status.cmisStateChanged()) {
    moduleStatusCache_.cmisStateChanged() =
        *status.cmisStateChanged() | *moduleStatusCache_.cmisStateChanged();
  } else {
    moduleStatusCache_.cmisStateChanged().copy_from(status.cmisStateChanged());
  }
}

void QsfpModule::cacheMediaLaneSignals(
    const std::vector<MediaLaneSignals>& mediaSignals) {
  for (const auto& signal : mediaSignals) {
    if (mediaSignalsCache_.find(*signal.lane()) == mediaSignalsCache_.end()) {
      // Initialize all lanes to false if an entry in the cache doesn't exist
      // yet
      mediaSignalsCache_[*signal.lane()].lane() = *signal.lane();
      mediaSignalsCache_[*signal.lane()].txFault() = false;
    }
    if (auto txFault = signal.txFault()) {
      if (*txFault) {
        mediaSignalsCache_[*signal.lane()].txFault() = true;
      }
    }
  }
}

bool QsfpModule::setPortPrbs(
    phy::Side side,
    const prbs::InterfacePrbsState& prbs) {
  bool status = false;
  auto setPrbsLambda = [&status, side, prbs, this]() {
    lock_guard<std::mutex> g(qsfpModuleMutex_);
    status = setPortPrbsLocked(side, prbs);
  };
  auto i2cEvb = qsfpImpl_->getI2cEventBase();
  if (!i2cEvb) {
    // Certain platforms cannot execute multiple I2C transactions in parallel
    // and therefore don't have an I2C evb thread
    setPrbsLambda();
  } else {
    via(i2cEvb)
        .thenValue([setPrbsLambda](auto&&) mutable { setPrbsLambda(); })
        .get();
  }
  return status;
}

bool QsfpModule::setTransceiverTx(
    const std::string& portName,
    bool lineSide,
    std::optional<uint8_t> userChannelMask,
    bool enable) {
  // Lambda to call Locked function
  auto setTcvrFn = [&]() -> bool {
    lock_guard<std::mutex> g(qsfpModuleMutex_);
    return setTransceiverTxLocked(portName, lineSide, userChannelMask, enable);
  };

  auto i2cEvb = qsfpImpl_->getI2cEventBase();
  if (!i2cEvb) {
    // In non-multithreaded environment, run the function in current thread
    return setTcvrFn();
  } else {
    // In mulththreaded environment, run the function in event base thread
    return via(i2cEvb)
        .thenValue([setTcvrFn](auto&&) mutable { return setTcvrFn(); })
        .get();
  }
}

/*
 * isTransceiverFeatureSupported
 *
 * This function returns the supported status of transceiver features
 */
bool QsfpModule::isTransceiverFeatureSupported(TransceiverFeature feature) {
  auto lockedDiagsCapability = diagsCapability_.rlock();
  if (auto diagsCapability = *lockedDiagsCapability) {
    switch (feature) {
      case TransceiverFeature::NONE:
        return false;
      case TransceiverFeature::VDM:
        return diagsCapability->vdm().value();
      case TransceiverFeature::CDB:
        return diagsCapability->cdb().value();
      case TransceiverFeature::PRBS:
      case TransceiverFeature::LOOPBACK:
      case TransceiverFeature::TX_DISABLE:
        throw FbossError(
            "Line/System side info is needed to check feature support in Transceiver");
      default:
        return false;
    }
  }
  return false;
}

/*
 * isTransceiverFeatureSupported
 *
 * This function returns the supported status of transceiver features for line
 * or system side
 */
bool QsfpModule::isTransceiverFeatureSupported(
    TransceiverFeature feature,
    bool lineSide) {
  auto lockedDiagsCapability = diagsCapability_.rlock();
  if (auto diagsCapability = *lockedDiagsCapability) {
    switch (feature) {
      case TransceiverFeature::NONE:
        return false;
      case TransceiverFeature::VDM:
      case TransceiverFeature::CDB:
        throw FbossError(
            "Line/System side info is not needed to check Feature support in Transceiver");
        return diagsCapability->cdb().value();
      case TransceiverFeature::PRBS:
        return lineSide ? diagsCapability->prbsLine().value()
                        : diagsCapability->prbsSystem().value();
      case TransceiverFeature::LOOPBACK:
        return lineSide ? diagsCapability->loopbackLine().value()
                        : diagsCapability->loopbackSystem().value();
      case TransceiverFeature::TX_DISABLE:
        return lineSide ? diagsCapability->txOutputControl().value()
                        : diagsCapability->rxOutputControl().value();
      default:
        return false;
    }
  }
  return false;
}

prbs::InterfacePrbsState QsfpModule::getPortPrbsState(phy::Side side) {
  prbs::InterfacePrbsState state;
  auto getPrbsStateLambda = [&state, side, this]() {
    lock_guard<std::mutex> g(qsfpModuleMutex_);
    state = getPortPrbsStateLocked(side);
  };
  auto i2cEvb = qsfpImpl_->getI2cEventBase();
  if (!i2cEvb) {
    // Certain platforms cannot execute multiple I2C transactions in parallel
    // and therefore don't have an I2C evb thread
    getPrbsStateLambda();
  } else {
    via(i2cEvb)
        .thenValue(
            [getPrbsStateLambda](auto&&) mutable { getPrbsStateLambda(); })
        .get();
  }
  return state;
}

void QsfpModule::refresh() {
  lock_guard<std::mutex> g(qsfpModuleMutex_);
  refreshLocked();
}

folly::Future<folly::Unit> QsfpModule::futureRefresh() {
  // Always use i2cEvb to program transceivers if there's an i2cEvb
  auto i2cEvb = qsfpImpl_->getI2cEventBase();
  if (!i2cEvb) {
    try {
      refresh();
    } catch (const std::exception& ex) {
      QSFP_LOG(DBG2, this) << "Error calling refresh(): " << ex.what();
    }
    return folly::makeFuture();
  }

  return via(i2cEvb).thenValue([&](auto&&) mutable {
    try {
      this->refresh();
    } catch (const std::exception& ex) {
      QSFP_LOG(DBG2, this) << "Error calling refresh(): " << ex.what();
    }
  });
}

void QsfpModule::refreshLocked() {
  auto detectionStatus = detectPresenceLocked();

  auto willRefresh = !dirty_ && shouldRefresh(FLAGS_qsfp_data_refresh_interval);
  QSFP_LOG_IF(DBG3, dirty_, this)
      << "dirty_ = " << dirty_ << ", willRefresh = " << willRefresh
      << ", present = " << detectionStatus.present
      << ", statusChanged = " << detectionStatus.statusChanged;
  if (!dirty_ && !willRefresh) {
    return;
  }

  if (detectionStatus.statusChanged && detectionStatus.present) {
    // A new transceiver has been detected
    getTransceiverManager()->updateStateBlocking(
        getID(),
        TransceiverStateMachineEvent::TCVR_EV_EVENT_DETECT_TRANSCEIVER);
  } else if (detectionStatus.statusChanged && !detectionStatus.present) {
    // The transceiver has been removed
    getTransceiverManager()->updateStateBlocking(
        getID(), TransceiverStateMachineEvent::TCVR_EV_REMOVE_TRANSCEIVER);
  }

  ModuleStatus moduleStatus;
  // Each of the reset functions need to check whether the transceiver is
  // present or not, and then handle its own logic differently. Even though
  // the transceiver might be absent here, we'll still go through all of the
  // rest functions
  if (dirty_) {
    // make sure data is up to date before trying to customize.
    ensureOutOfReset();
    updateQsfpData(true);
    updateCmisStateChanged(moduleStatus);
    if (present_) {
      // Data has been read for the new optics
      getTransceiverManager()->updateStateBlocking(
          getID(), TransceiverStateMachineEvent::TCVR_EV_READ_EEPROM);
      // Issue an allPages=false update to pick up the new qsfp data after we
      // trigger READ_EEPROM event. Some Transceiver might pick up all the diag
      // capabilities and we can use this to make sure the current QsfpData has
      // updated pages without waiting for the next refresh
      // TODO: updateQsfpData here could be unnecessary if the read_eeprom event
      // above is a no-op. Need to figure out a way to avoid this call in that
      // case
      updateQsfpData(false);
    }
  }

  // If it's just regular refresh
  if (willRefresh) {
    updateQsfpData(false);
    updateCmisStateChanged(moduleStatus);
  }

  updateCachedTransceiverInfoLocked(moduleStatus);
  // Only update prbs stats if the transceiver is present.
  // Should have this check inside of updatePrbsStats().
  // However updatePrbsStats() is a public function and not lock safe as
  // refresh() to get the qsfpModuleMutex_ first.
  // TODO: Need to rethink whether all the following prbs stats functions should
  // get the lock of qsfpModuleMutex_ first.
  if (detectionStatus.present) {
    updatePrbsStats();
  }
}

void QsfpModule::clearTransceiverPrbsStats(phy::Side side) {
  auto systemPrbs = systemPrbsStats_.wlock();
  auto linePrbs = linePrbsStats_.wlock();

  auto clearLaneStats = [this](std::vector<phy::PrbsLaneStats>& laneStats) {
    for (auto& laneStat : laneStats) {
      laneStat.ber() = 0;
      laneStat.maxBer() = 0;
      laneStat.snr().reset();
      laneStat.maxSnr().reset();
      laneStat.numLossOfLock() = 0;
      laneStat.timeSinceLastClear() = 0;

      QSFP_LOG(INFO, this) << " Lane " << *laneStat.laneId()
                           << " ber and maxBer cleared";
    }
  };
  if (side == phy::Side::SYSTEM) {
    clearLaneStats(*systemPrbs->laneStats());
  } else {
    clearLaneStats(*linePrbs->laneStats());
  }
}

void QsfpModule::updatePrbsStats() {
  auto systemPrbs = systemPrbsStats_.wlock();
  auto linePrbs = linePrbsStats_.wlock();

  auto updatePrbsStatEntry =
      [this](const phy::PrbsStats& oldStat, phy::PrbsStats& newStat) {
        for (const auto& oldLane : *oldStat.laneStats()) {
          for (auto& newLane : *newStat.laneStats()) {
            if (*newLane.laneId() != *oldLane.laneId()) {
              continue;
            }
            // Update numLossOfLock
            if (!(*newLane.locked()) && *oldLane.locked()) {
              newLane.numLossOfLock() = *oldLane.numLossOfLock() + 1;
            } else {
              newLane.numLossOfLock() = *oldLane.numLossOfLock();
            }
            // Update maxBer only if there is a lock
            if (*newLane.locked() && *newLane.ber() > *oldLane.maxBer()) {
              newLane.maxBer() = *newLane.ber();
            } else {
              newLane.maxBer() = *oldLane.maxBer();
            }
            // Update maxSnr only if there is a lock
            if (*newLane.locked()) {
              if (newLane.snr().has_value() &&
                  (!oldLane.maxSnr().has_value() ||
                   *newLane.snr() > *oldLane.maxSnr())) {
                newLane.maxSnr() = *newLane.snr();
              }
            } else if (oldLane.maxSnr().has_value()) {
              newLane.maxSnr() = *oldLane.maxSnr();
            }

            QSFP_LOG(DBG5, this)
                << " Lane " << *newLane.laneId()
                << " Lock=" << (*newLane.locked() ? "Y" : "N")
                << " ber=" << *newLane.ber() << " maxBer=" << *newLane.maxBer()
                << " snr=" << (newLane.snr().has_value() ? *newLane.snr() : 0)
                << " maxSnr="
                << (newLane.maxSnr().has_value() ? *newLane.maxSnr() : 0);

            // Update timeSinceLastLocked
            // If previously there was no lock and now there is, update
            // timeSinceLastLocked to now
            if (!(*oldLane.locked()) && *newLane.locked()) {
              newLane.timeSinceLastLocked() = 0;
            } else {
              newLane.timeSinceLastLocked() = *oldLane.timeSinceLastLocked() +
                  (*newStat.timeCollected()) - (*oldStat.timeCollected());
            }
            if (!(*oldStat.timeCollected())) {
              // Initially timeCollected will be 0, so initialize
              // timeSinceLastClear to be 0 also
              newLane.timeSinceLastClear() = 0;
            } else {
              newLane.timeSinceLastClear() = *oldLane.timeSinceLastClear() +
                  (*newStat.timeCollected()) - (*oldStat.timeCollected());
            }
          }
        }
      };

  auto sysPrbsState = getPortPrbsStateLocked(phy::Side::SYSTEM);
  auto linePrbsState = getPortPrbsStateLocked(phy::Side::LINE);
  phy::PrbsStats stats;
  stats = getPortPrbsStatsSideLocked(
      phy::Side::SYSTEM,
      sysPrbsState.checkerEnabled().has_value() &&
          sysPrbsState.checkerEnabled().value(),
      *systemPrbs);
  updatePrbsStatEntry(*systemPrbs, stats);
  *systemPrbs = stats;

  stats = getPortPrbsStatsSideLocked(
      phy::Side::LINE,
      linePrbsState.checkerEnabled().has_value() &&
          linePrbsState.checkerEnabled().value(),
      *linePrbs);
  updatePrbsStatEntry(*linePrbs, stats);
  *linePrbs = stats;
}

bool QsfpModule::shouldRemediate() {
  // Always use i2cEvb to program transceivers if there's an i2cEvb
  auto shouldRemediateFunc = [this]() {
    lock_guard<std::mutex> g(qsfpModuleMutex_);
    return shouldRemediateLocked();
  };
  auto i2cEvb = qsfpImpl_->getI2cEventBase();
  if (!i2cEvb) {
    // Certain platforms cannot execute multiple I2C transactions in parallel
    // and therefore don't have an I2C evb thread
    return shouldRemediateFunc();
  } else {
    bool shouldRemediateResult = false;
    via(i2cEvb)
        .thenValue(
            [&shouldRemediateResult, shouldRemediateFunc](auto&&) mutable {
              shouldRemediateResult = shouldRemediateFunc();
            })
        .get();
    return shouldRemediateResult;
  }
}

bool QsfpModule::shouldRemediateLocked() {
  if (!supportRemediate()) {
    return false;
  }

  auto sysPrbsState = getPortPrbsStateLocked(phy::Side::SYSTEM);
  auto linePrbsState = getPortPrbsStateLocked(phy::Side::LINE);

  auto linePrbsEnabled =
      ((linePrbsState.generatorEnabled().has_value() &&
        linePrbsState.generatorEnabled().value()) ||
       (linePrbsState.checkerEnabled().has_value() &&
        linePrbsState.checkerEnabled().value()));
  auto sysPrbsEnabled =
      ((sysPrbsState.generatorEnabled().has_value() &&
        sysPrbsState.generatorEnabled().value()) ||
       (sysPrbsState.checkerEnabled().has_value() &&
        sysPrbsState.checkerEnabled().value()));

  if (linePrbsEnabled || sysPrbsEnabled) {
    QSFP_LOG(INFO, this)
        << "Skipping remediation because PRBS is enabled. System: "
        << sysPrbsEnabled << ", Line: " << linePrbsEnabled;
    return false;
  }

  auto now = std::time(nullptr);
  std::map<std::string, int32_t> remediatePausedInfo;
  getTransceiverManager()->getPauseRemediationUntil(
      remediatePausedInfo, nullptr);
  bool remediationEnabled = (now > remediatePausedInfo["all"]) &&
      (now > getModulePauseRemediationUntil());
  // Rather than immediately attempting to remediate a module,
  // we would like to introduce a bit delay to de-couple the consequences
  // of a remediation with the root cause that brought down the link.
  // This is an effort to help with debugging.
  // And for the first remediation, we don't want to wait for
  // `FLAGS_remediate_interval`, instead we just need to wait for
  // `FLAGS_initial_remediate_interval`. (D26014510)
  bool remediationCooled = false;
  auto lastDownTime = lastDownTime_.load();
  if (lastDownTime > lastRemediateTime_) {
    // New lastDownTime means the port just recently went down
    remediationCooled = (now - lastDownTime) > FLAGS_initial_remediate_interval;
  } else {
    remediationCooled = (now - lastRemediateTime_) > FLAGS_remediate_interval;
  }
  return remediationEnabled && remediationCooled;
}

void QsfpModule::customizeTransceiver(TransceiverPortState& portState) {
  lock_guard<std::mutex> g(qsfpModuleMutex_);
  if (present_) {
    customizeTransceiverLocked(portState);
  }
}

void QsfpModule::customizeTransceiverLocked(TransceiverPortState& portState) {
  auto& portName = portState.portName;
  auto speed = portState.speed;
  auto startHostLane = portState.startHostLane;
  QSFP_LOG(INFO, this) << folly::sformat(
      "customizeTransceiverLocked: PortName {}, Speed {}, StartHostLane {}",
      portName,
      apache::thrift::util::enumNameSafe(speed),
      startHostLane);
  /*
   * This must be called with a lock held on qsfpModuleMutex_
   */
  if (customizationSupported()) {
    TransceiverSettings settings = getTransceiverSettingsInfo();

    // We want this on regardless of speed
    setPowerOverrideIfSupportedLocked(*settings.powerControl());

    if (speed != cfg::PortSpeed::DEFAULT) {
      setCdrIfSupported(speed, *settings.cdrTx(), *settings.cdrRx());
      setRateSelectIfSupported(
          speed, *settings.rateSelect(), *settings.rateSelectSetting());
    }
  } else {
    QSFP_LOG(DBG1, this) << "Customization not supported";
  }
}

std::optional<TransceiverStats> QsfpModule::getTransceiverStats() {
  auto transceiverStats = qsfpImpl_->getTransceiverStats();
  if (!transceiverStats.has_value()) {
    return {};
  }
  return transceiverStats.value();
}

folly::Future<std::pair<int32_t, std::unique_ptr<IOBuf>>>
QsfpModule::futureReadTransceiver(TransceiverIOParameters param) {
  // Always use i2cEvb to program transceivers if there's an i2cEvb
  auto i2cEvb = qsfpImpl_->getI2cEventBase();
  auto id = getID();
  if (!i2cEvb) {
    // Certain platforms cannot execute multiple I2C transactions in parallel
    // and therefore don't have an I2C evb thread
    return std::make_pair(id, readTransceiver(param));
  }
  // As with all the other i2c transactions, run in the i2c event base thread
  return via(i2cEvb).thenValue([&, param, id](auto&&) mutable {
    return std::make_pair(id, readTransceiver(param));
  });
}

std::unique_ptr<IOBuf> QsfpModule::readTransceiver(
    TransceiverIOParameters param) {
  lock_guard<std::mutex> g(qsfpModuleMutex_);
  return readTransceiverLocked(param);
}

std::unique_ptr<IOBuf> QsfpModule::readTransceiverLocked(
    TransceiverIOParameters param) {
  /*
   * This must be called with a lock held on qsfpModuleMutex_
   */
  auto length = param.length().has_value() ? *(param.length()) : 1;
  auto iobuf = folly::IOBuf::createCombined(length);
  if (!present_) {
    return iobuf;
  }
  try {
    auto offset = *(param.offset());
    if (param.page().has_value()) {
      uint8_t page = *(param.page());
      // When the page is specified, first update byte 127 with the speciied
      // pageId
      qsfpImpl_->writeTransceiver(
          {TransceiverI2CApi::ADDR_QSFP, 127, sizeof(page)}, &page);
    }
    qsfpImpl_->readTransceiver(
        {TransceiverI2CApi::ADDR_QSFP, offset, length}, iobuf->writableData());
    // Mark the valid data in the buffer
    iobuf->append(length);
  } catch (const std::exception& ex) {
    QSFP_LOG(ERR, this) << "Error reading data: " << ex.what();
    throw;
  }
  return iobuf;
}

folly::Future<std::pair<int32_t, bool>> QsfpModule::futureWriteTransceiver(
    TransceiverIOParameters param,
    uint8_t data) {
  // Always use i2cEvb to program transceivers if there's an i2cEvb
  auto i2cEvb = qsfpImpl_->getI2cEventBase();
  auto id = getID();
  if (!i2cEvb) {
    // Certain platforms cannot execute multiple I2C transactions in parallel
    // and therefore don't have an I2C evb thread
    return std::make_pair(id, writeTransceiver(param, data));
  }
  // As with all the other i2c transactions, run in the i2c event base thread
  return via(i2cEvb).thenValue([&, param, id, data](auto&&) mutable {
    return std::make_pair(id, writeTransceiver(param, data));
  });
}

bool QsfpModule::writeTransceiver(TransceiverIOParameters param, uint8_t data) {
  lock_guard<std::mutex> g(qsfpModuleMutex_);
  return writeTransceiverLocked(param, data);
}

bool QsfpModule::writeTransceiverLocked(
    TransceiverIOParameters param,
    uint8_t data) {
  /*
   * This must be called with a lock held on qsfpModuleMutex_
   */
  if (!present_) {
    return false;
  }
  try {
    auto offset = *(param.offset());
    if (param.page().has_value()) {
      uint8_t page = *(param.page());
      // When the page is specified, first update byte 127 with the speciied
      // pageId
      qsfpImpl_->writeTransceiver(
          {TransceiverI2CApi::ADDR_QSFP, 127, sizeof(page)}, &page);
    }
    qsfpImpl_->writeTransceiver(
        {TransceiverI2CApi::ADDR_QSFP, offset, sizeof(data)}, &data);
  } catch (const std::exception& ex) {
    QSFP_LOG(ERR, this) << "Error writing data: " << ex.what();
    throw;
  }
  return true;
}

SignalFlags QsfpModule::readAndClearCachedSignalFlags() {
  lock_guard<std::mutex> g(qsfpModuleMutex_);
  SignalFlags signalFlag;
  // Store the cached data before clearing it.
  signalFlag.txLos() = *signalFlagCache_.txLos();
  signalFlag.rxLos() = *signalFlagCache_.rxLos();
  signalFlag.txLol() = *signalFlagCache_.txLol();
  signalFlag.rxLol() = *signalFlagCache_.rxLol();

  // Clear the cached data after read.
  signalFlagCache_.txLos() = 0;
  signalFlagCache_.rxLos() = 0;
  signalFlagCache_.txLol() = 0;
  signalFlagCache_.rxLol() = 0;
  return signalFlag;
}

std::map<int, MediaLaneSignals>
QsfpModule::readAndClearCachedMediaLaneSignals() {
  lock_guard<std::mutex> g(qsfpModuleMutex_);
  // Store the cached data before clearing it.
  std::map<int, MediaLaneSignals> mediaSignals(mediaSignalsCache_);

  // Clear the cached data after read.
  for (auto& signal : mediaSignalsCache_) {
    signal.second.txFault() = false;
  }
  return mediaSignals;
}

ModuleStatus QsfpModule::readAndClearCachedModuleStatus() {
  lock_guard<std::mutex> g(qsfpModuleMutex_);
  // Store the cached data before clearing it.
  ModuleStatus moduleStatus(moduleStatusCache_);

  // Clear the cached data after read.
  moduleStatusCache_ = ModuleStatus();

  return moduleStatus;
}

TransceiverManagementInterface QsfpModule::getTransceiverManagementInterface(
    const uint8_t moduleId,
    const unsigned int oneBasedPort) {
  if (moduleId ==
          static_cast<uint8_t>(TransceiverModuleIdentifier::QSFP_PLUS_CMIS) ||
      moduleId == static_cast<uint8_t>(TransceiverModuleIdentifier::QSFP_DD) ||
      moduleId == static_cast<uint8_t>(TransceiverModuleIdentifier::OSFP)) {
    return TransceiverManagementInterface::CMIS;
  } else if (
      moduleId ==
          static_cast<uint8_t>(TransceiverModuleIdentifier::QSFP_PLUS) ||
      moduleId == static_cast<uint8_t>(TransceiverModuleIdentifier::QSFP) ||
      moduleId ==
          static_cast<uint8_t>(TransceiverModuleIdentifier::MINIPHOTON_OBO) ||
      moduleId == static_cast<uint8_t>(TransceiverModuleIdentifier::QSFP28)) {
    return TransceiverManagementInterface::SFF;
  } else if (
      moduleId == static_cast<uint8_t>(TransceiverModuleIdentifier::SFP_PLUS)) {
    return TransceiverManagementInterface::SFF8472;
  } else if (
      moduleId != static_cast<uint8_t>(TransceiverModuleIdentifier::UNKNOWN)) {
    XLOG(ERR) << fmt::format(
        "QSFP {:d}: Unrecognized non zero module Id = {:d}",
        oneBasedPort,
        moduleId);
    return TransceiverManagementInterface::UNKNOWN;
  }

  XLOG(ERR) << fmt::format(
      "QSFP {:d}: Bad module Id = {:d}", oneBasedPort, moduleId);
  return TransceiverManagementInterface::NONE;
}

void QsfpModule::programTransceiver(
    ProgramTransceiverState& programTcvrState,
    bool needResetDataPath) {
  // Always use i2cEvb to program transceivers if there's an i2cEvb
  auto programTcvrFunc = [this, &programTcvrState, needResetDataPath]() {
    lock_guard<std::mutex> g(qsfpModuleMutex_);
    if (present_) {
      // Don't consider ports for programming if they have a startHostLane >=
      // the number of lanes on the plugged in transceiver.
      auto hostLaneCount = numHostLanes();
      for (auto portIt : programTcvrState.ports) {
        // startHostLane is 0-indexed hence the >= comparison
        if (portIt.second.startHostLane >= hostLaneCount) {
          programTcvrState.ports.erase(portIt.first);
        }
      }

      if (!cacheIsValid()) {
        throw FbossError(
            "Transceiver: ",
            getNameString(),
            " - Cache is not valid, so cannot program the transceiver");
      }
      // Make sure customize xcvr first so that we can set the application code
      // correctly and then call configureModule() later to program serdes like
      // Rx equalizer setting based on QSFP config
      for (auto portIt : programTcvrState.ports) {
        customizeTransceiverLocked(portIt.second);
      }
      // updateQsfpData so that we can make sure the new application code in
      // cache or the new host settings ges updated before calling
      // configureModule()
      updateQsfpData(true);
      // Current configureModule() actually assumes the locked is obtained.
      // See CmisModule::configureModule(). Need to clean it up in the future.
      for (auto portIt : programTcvrState.ports) {
        auto startHostLane = portIt.second.startHostLane;
        configureModule(startHostLane);
      }

      TransceiverSettings settings = getTransceiverSettingsInfo();
      // We found that some module did not enable Rx output squelch by default,
      // which introduced some difficulty to bring link back up when flapped.
      // Here we ensure that Rx output squelch is always enabled.
      // Skip doing this for 200G-FR4 modules configured in 2x50G mode. For this
      // mode, we need all 4 lanes to operate independently
      for (auto portIt : programTcvrState.ports) {
        auto speed = portIt.second.speed;
        if (getModuleMediaInterface() != MediaInterfaceCode::FR4_200G ||
            speed != cfg::PortSpeed::FIFTYTHREEPOINTONETWOFIVEG) {
          if (auto hostLaneSettings = settings.hostLaneSettings()) {
            ensureRxOutputSquelchEnabled(*hostLaneSettings);
          }
        }
      }

      if (needResetDataPath) {
        resetDataPath();
      }

      // Since we're touching the transceiver, we need to update the cached
      // transceiver info
      updateQsfpData(false);

      // Cache has been updated, populate the host/mediaLane to port name
      // mapping
      // First clear the existing mappings
      hostLaneToPortName_.clear();
      mediaLaneToPortName_.clear();
      portNameToHostLanes_.clear();
      portNameToMediaLanes_.clear();
      for (auto portIt : programTcvrState.ports) {
        auto startHostLane = portIt.second.startHostLane;
        updateLaneToPortNameMapping(portIt.first, startHostLane);
      }
      updateCachedTransceiverInfoLocked({});
    }
  };

  auto i2cEvb = qsfpImpl_->getI2cEventBase();
  if (!i2cEvb) {
    // Certain platforms cannot execute multiple I2C transactions in parallel
    // and therefore don't have an I2C evb thread
    programTcvrFunc();
  } else {
    via(i2cEvb)
        .thenValue([programTcvrFunc](auto&&) mutable { programTcvrFunc(); })
        .get();
  }
}

void QsfpModule::updateLaneToPortNameMapping(
    const std::string& portName,
    uint8_t startHostLane) {
  auto hostLanes = configuredHostLanes(startHostLane);
  auto mediaLanes = configuredMediaLanes(
      startHostLane); // assumption: startMediaLane = startHostLane

  portNameToHostLanes_[portName] = {};
  for (auto lane : hostLanes) {
    hostLaneToPortName_[lane] = portName;
    portNameToHostLanes_[portName].insert(lane);
  }

  portNameToMediaLanes_[portName] = {};
  for (auto lane : mediaLanes) {
    mediaLaneToPortName_[lane] = portName;
    portNameToMediaLanes_[portName].insert(lane);
  }
}

/*
 * readyTransceiver
 *
 * Runs a function in the i2c controller's event base thread to check if the
 * module's power control configuration is same as the desired one. If it is
 * same then return true or false based on whether module is in ready state or
 * not. If the power controll config value is not same as desired one then
 * configure it correctly and return false
 */
bool QsfpModule::readyTransceiver() {
  // Always use i2cEvb to program transceivers if there's an i2cEvb
  auto powerStateCheckFn = [this]() -> bool {
    lock_guard<std::mutex> g(qsfpModuleMutex_);
    if (present_) {
      if (!cacheIsValid()) {
        QSFP_LOG(DBG1, this) << folly::sformat(
            "Transceiver {:s} - Cache is not valid, so cannot check the transceiver state",
            getNameString());
        return false;
      }
      // Check the transceiver power configuration state and then return
      // accordingly. This function's implementation is dependent on optics
      // type (Cmis, Sff etc)
      return ensureTransceiverReadyLocked();
    } else {
      // If module is not present then don't block state machine transition
      // and return true
      return true;
    }
  };

  auto i2cEvb = qsfpImpl_->getI2cEventBase();
  if (!i2cEvb) {
    // Certain platforms cannot execute multiple I2C transactions in parallel
    // and therefore don't have an I2C evb thread. For them, call the function
    // directly from current thread
    return powerStateCheckFn();
  } else {
    // Call the function in I2c controller's event base thread
    return via(i2cEvb)
        .thenValue(
            [powerStateCheckFn](auto&&) mutable { return powerStateCheckFn(); })
        .get();
  }
}

void QsfpModule::publishSnapshots() {
  auto snapshotsLocked = snapshots_.wlock();
  snapshotsLocked->publishAllSnapshots();
  snapshotsLocked->publishFutureSnapshots();
}

bool QsfpModule::tryRemediate(
    bool allPortsDown,
    const std::vector<std::string>& ports) {
  // Always use i2cEvb to program transceivers if there's an i2cEvb
  auto remediateTcvrFunc = [this, allPortsDown, &ports]() {
    lock_guard<std::mutex> g(qsfpModuleMutex_);
    return tryRemediateLocked(allPortsDown, ports);
  };
  auto i2cEvb = qsfpImpl_->getI2cEventBase();
  if (!i2cEvb) {
    // Certain platforms cannot execute multiple I2C transactions in parallel
    // and therefore don't have an I2C evb thread
    return remediateTcvrFunc();
  } else {
    bool didRemediate = false;
    via(i2cEvb)
        .thenValue([&didRemediate, remediateTcvrFunc](auto&&) mutable {
          didRemediate = remediateTcvrFunc();
        })
        .get();
    return didRemediate;
  }
}

bool QsfpModule::tryRemediateLocked(
    bool allPortsDown,
    const std::vector<std::string>& ports) {
  // Only update numRemediation_ iff this transceiver should remediate and
  // remediation actually happens
  if (shouldRemediateLocked() &&
      remediateFlakyTransceiver(allPortsDown, ports)) {
    ++numRemediation_;
    // Remediation touches the hardware, hard resetting the optics in Cmis case,
    // so set dirty so that we always do a refresh in the next cycle and update
    // the cache with the recent data
    dirty_ = true;
    return true;
  }
  return false;
}

void QsfpModule::markLastDownTime() {
  lastDownTime_.store(std::time(nullptr));
}

/*
 * getBerFloatValue
 *
 * A utility function to convert the 16 bit BER value from module register to
 * the double value. This function is applicable to SFF as well as CMIS
 */
double QsfpModule::getBerFloatValue(uint8_t lsb, uint8_t msb) {
  int exponent = (lsb >> 3) & 0x1f;
  int mantissa = ((lsb & 0x7) << 8) | msb;

  exponent -= 24;
  double berVal = mantissa * exp10(exponent);
  return berVal;
}

void QsfpModule::setModulePauseRemediation(int32_t timeout) {
  modulePauseRemediationUntil_ = std::time(nullptr) + timeout;
}

time_t QsfpModule::getModulePauseRemediationUntil() {
  return modulePauseRemediationUntil_;
}

std::set<uint8_t> QsfpModule::getTcvrLanesForPort(
    const std::string& portName,
    bool lineSide) const {
  std::set<uint8_t> tcvrLanes;

  if (lineSide) {
    auto portNameToMediaLanes = getPortNameToMediaLanes();
    if (portNameToMediaLanes.find(portName) == portNameToMediaLanes.end()) {
      XLOG(ERR) << fmt::format(
          "Port name to line side lanes not available for {:s}", portName);
    } else {
      tcvrLanes = portNameToMediaLanes.at(portName);
    }
  } else {
    auto portNameToHostLanes = getPortNameToHostLanes();
    if (portNameToHostLanes.find(portName) == portNameToHostLanes.end()) {
      XLOG(ERR) << fmt::format(
          "Port name to lanes not available for {:s}", portName);
    } else {
      tcvrLanes = portNameToHostLanes.at(portName);
    }
  }

  return tcvrLanes;
}

} // namespace fboss
} // namespace facebook
