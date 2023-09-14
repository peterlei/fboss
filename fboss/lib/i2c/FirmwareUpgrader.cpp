// Copyright 2004-present Facebook. All Rights Reserved.

#include "fboss/lib/i2c/FirmwareUpgrader.h"

#include <folly/Conv.h>
#include <folly/Exception.h>
#include <folly/File.h>
#include <folly/FileUtil.h>
#include <folly/Format.h>
#include <folly/Memory.h>
#include <folly/init/Init.h>
#include <folly/io/IOBuf.h>
#include <folly/io/async/EventBase.h>
#include <folly/logging/xlog.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <chrono>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sysexits.h>

#include <thread>
#include <utility>
#include <vector>

using folly::MutableByteRange;
using folly::StringPiece;
using std::make_pair;
using std::pair;
using std::chrono::seconds;
using std::chrono::steady_clock;
using namespace facebook::fboss;

namespace facebook::fboss {

// CMIS firmware related register offsets
constexpr uint8_t kfirmwareVersionReg = 39;
constexpr uint8_t kModulePasswordEntryReg = 122;

constexpr int moduleDatapathInitDurationUsec = 5000000;

/*
 * CmisFirmwareUpgrader
 *
 * This is one of the two constructor and it will be invoked if the upgrader
 * is called from qsfp_service process. The caller will get the FbossFirmware
 * object and using that this CmisFirmwareUpgrader will be created. This
 * function will load the image file to IOBuf and get image pointer to use in
 * loading the firmware
 */
CmisFirmwareUpgrader::CmisFirmwareUpgrader(
    TransceiverI2CApi* bus,
    unsigned int modId,
    std::unique_ptr<FbossFirmware> fbossFirmware)
    : bus_(bus), moduleId_(modId), fbossFirmware_(std::move(fbossFirmware)) {
  // Check the FbossFirmware object first
  if (fbossFirmware_.get() == nullptr) {
    XLOG(ERR) << "FbossFirmware object is null, returning...";
    return;
  }
  // Load the image
  fbossFirmware_->load();
  // Get the image pointer
  imageCursor_ = fbossFirmware_->getImage();

  // Get the header length of image
  std::string hdrLen = fbossFirmware_->getProperty("header_length");
  imageHeaderLen_ = folly::to<uint32_t>(hdrLen);

  // Get the msa password
  std::string msaPwStr = fbossFirmware_->getProperty("msa_password");
  uint32_t msaPwVal = std::stoull(msaPwStr, nullptr, 16);
  msaPassword_[0] = (msaPwVal & 0xFF000000) >> 24;
  msaPassword_[1] = (msaPwVal & 0x00FF0000) >> 16;
  msaPassword_[2] = (msaPwVal & 0x0000FF00) >> 8;
  msaPassword_[3] = (msaPwVal & 0x000000FF);

  // Get the image type
  std::string imageTypeStr = fbossFirmware_->getProperty("image_type");
  appImage_ = (imageTypeStr == "application") ? true : false;
}

/*
 * cmisModuleFirmwareDownload
 *
 * This function runs the firmware download operation for a module. This takes
 * the image buffer as input. This is basic function to do firmware download
 * and it can be run in any context - single thread, multiple thread etc
 */
bool CmisFirmwareUpgrader::cmisModuleFirmwareDownload(
    const uint8_t* imageBuf,
    int imageLen) {
  uint8_t startCommandPayloadSize = 0;
  bool status;
  int imageOffset, imageChunkLen;
  bool eplSupported = false;

  XLOG(INFO) << folly::sformat(
      "cmisModuleFirmwareDownload: Mod{:d}: Starting to download the image with length {:d}",
      moduleId_,
      imageLen);

  // Set the password to let the privileged operation of firmware download
  bus_->moduleWrite(
      moduleId_,
      {TransceiverI2CApi::ADDR_QSFP, kModulePasswordEntryReg, 4},
      msaPassword_.data());

  CdbCommandBlock commandBlockBuf;
  CdbCommandBlock* commandBlock = &commandBlockBuf;

  // Basic validation first. Check if the firmware download is allowed by
  // issuing the Query command to CDB
  commandBlock->createCdbCmdModuleQuery();
  // Run the CDB command
  status = commandBlock->cmisRunCdbCommand(bus_, moduleId_);
  if (status) {
    // Query result will be in LPL memory at byte offset 2
    if (commandBlock->getCdbRlplLength() >= 3) {
      if (commandBlock->getCdbLplFlatMemory()[2] == 0) {
        // This should not happen because before calling this function
        // we supply the password to module to allow priviledged
        // operation. But still download feature is not available here
        // so return false here
        XLOG(INFO) << folly::sformat(
            "cmisModuleFirmwareDownload: Mod{:d}: The firmware download feature is locked by vendor",
            moduleId_);
        return false;
      }
    }
  } else {
    // The QUERY command can fail if the module is in bootloader mode
    // Not able to determine CDB module status but don't return from here
    XLOG(INFO) << folly::sformat(
        "cmisModuleFirmwareDownload: Mod{:d}: Could not get result from CDB Query command",
        moduleId_);
  }

  // Step 0: Retrieve the Start Command Payload Size (the image header size)
  // Done by sending Firmware upgrade feature command to CDB
  commandBlock->createCdbCmdGetFwFeatureInfo();
  // Run the CDB command
  status = commandBlock->cmisRunCdbCommand(bus_, moduleId_);

  // If the CDB command is successfull then the Start Command Payload Size is
  // returned by CDB in LPL memory at offset 2

  if (status && commandBlock->getCdbRlplLength() >= 3) {
    // Get the firmware header size from CDB
    startCommandPayloadSize = commandBlock->getCdbLplFlatMemory()[2];

    // Check if EPL memory is supported
    if (commandBlock->getCdbLplFlatMemory()[5] == 0x10 ||
        commandBlock->getCdbLplFlatMemory()[5] == 0x11) {
      eplSupported = true;
      XLOG(INFO) << folly::sformat(
          "cmisModuleFirmwareDownload: Mod{:d} will use EPL memory for firmware download",
          moduleId_);
    }
  } else {
    XLOG(INFO) << folly::sformat(
        "cmisModuleFirmwareDownload: Mod{:d}: Could not get result from CDB Firmware Update Feature command",
        moduleId_);

    // Sometime when the optics is  in boot loader mode, this CDB command
    // fails. So fill in the header size if it is a known optics otherwise
    // return false
    startCommandPayloadSize = imageHeaderLen_;
    XLOG(INFO) << folly::sformat(
        "cmisModuleFirmwareDownload: Mod{:d}: Setting the module startCommandPayloadSize as {:d}",
        moduleId_,
        startCommandPayloadSize);
  }

  XLOG(INFO) << folly::sformat(
      "cmisModuleFirmwareDownload: Mod{:d}: Step 0: Got Start Command Payload Size as {:d}",
      moduleId_,
      startCommandPayloadSize);

  // Validate if the image length is greater than this. If not then our new
  // image is bad
  if (imageLen < startCommandPayloadSize) {
    XLOG(INFO) << folly::sformat(
        "cmisModuleFirmwareDownload: Mod{:d}: The image length {:d} is smaller than startCommandPayloadSize {:d}",
        moduleId_,
        imageLen,
        startCommandPayloadSize);
    return false;
  }

  // Step 1: Issue CDB command: Firmware Download start
  imageChunkLen = startCommandPayloadSize;
  commandBlock->createCdbCmdFwDownloadStart(
      startCommandPayloadSize, imageLen, imageOffset, imageBuf);

  // Run the CDB command
  status = commandBlock->cmisRunCdbCommand(bus_, moduleId_);
  if (!status) {
    // DOWNLOAD_START command failed
    XLOG(INFO) << folly::sformat(
        "cmisModuleFirmwareDownload: Mod{:d}: Could not run the CDB Firmware Download Start command",
        moduleId_);
    return false;
  }

  XLOG(INFO) << folly::sformat(
      "cmisModuleFirmwareDownload: Mod{:d}: Step 1: Issued Firmware download start command successfully",
      moduleId_);

  // Step 2: Issue CDB command: Firmware Download image

  XLOG(INFO) << folly::sformat(
      "cmisModuleFirmwareDownload: Mod{:d}: Step 2: Issuing Firmware Download Image command. Starting offset: {:d}",
      moduleId_,
      imageOffset);

  while (imageOffset < imageLen) {
    if (!eplSupported) {
      // Create CDB command block using internal LPL memory
      commandBlock->createCdbCmdFwDownloadImageLpl(
          startCommandPayloadSize,
          imageLen,
          imageBuf,
          imageOffset,
          imageChunkLen);
    } else {
      // Create CDB command block assuming external EPL memory
      commandBlock->createCdbCmdFwDownloadImageEpl(
          startCommandPayloadSize, imageLen, imageOffset, imageChunkLen);

      // Write the image payload to external EPL before invoking the command
      commandBlock->writeEplPayload(
          bus_, moduleId_, imageBuf, imageOffset, imageChunkLen);
    }

    // Run the CDB command
    status = commandBlock->cmisRunCdbCommand(bus_, moduleId_);
    if (!status) {
      // DOWNLOAD_IMAGE command failed
      XLOG(INFO) << folly::sformat(
          "cmisModuleFirmwareDownload: Mod{:d}: Could not run the CDB Firmware Download Image command",
          moduleId_);
      return false;
    }
    XLOG(INFO) << folly::sformat(
        "cmisModuleFirmwareDownload: Mod{:d}: Image wrote, offset: {:d} .. {:d}",
        moduleId_,
        imageOffset - imageChunkLen,
        imageOffset);
  }
  XLOG(INFO) << folly::sformat(
      "cmisModuleFirmwareDownload: Mod{:d}: Step 2: Issued Firmware Download Image successfully. Downloaded file size {:d}",
      moduleId_,
      imageOffset);

  // Step 3: Issue CDB command: Firmware download complete
  commandBlock->createCdbCmdFwDownloadComplete();

  // Run the CDB command
  status = commandBlock->cmisRunCdbCommand(bus_, moduleId_);
  if (!status) {
    // DOWNLOAD_COMPLETE command failed
    XLOG(INFO) << folly::sformat(
        "cmisModuleFirmwareDownload: Mod{:d}: Could not run the CDB Firmware Download Complete command",
        moduleId_);
    // Send the DOWNLOAD_ABORT command to CDB and return.
    return false;
  }

  XLOG(INFO) << folly::sformat(
      "cmisModuleFirmwareDownload: Mod{:d}: Step 3: Issued Firmware download complete command successfully",
      moduleId_);

  // Non App images like DSP image don't need last 2 steps (Run, Commit) for the
  // firmware download
  if (!appImage_) {
    return true;
  }

  // Step 4: Issue CDB command: Run the downloaded firmware
  commandBlock->createCdbCmdFwImageRun();

  // Run the CDB command
  // No need to check status because RUN command issues soft reset to CDB
  // so we can't check status here
  status = commandBlock->cmisRunCdbCommand(bus_, moduleId_);

  XLOG(INFO) << folly::sformat(
      "cmisModuleFirmwareDownload: Mod{:d}: Step 4: Issued Firmware download Run command successfully",
      moduleId_);

  usleep(2 * moduleDatapathInitDurationUsec);

  // Set the password to let the privileged operation of firmware download
  bus_->moduleWrite(
      moduleId_,
      {TransceiverI2CApi::ADDR_QSFP, kModulePasswordEntryReg, 4},
      msaPassword_.data());

  // Step 5: Issue CDB command: Commit the downloaded firmware
  commandBlock->createCdbCmdFwCommit();

  // Run the CDB command
  status = commandBlock->cmisRunCdbCommand(bus_, moduleId_);

  if (!status) {
    XLOG(INFO) << folly::sformat(
        "cmisModuleFirmwareDownload: Mod{:d}: Step 5: Issued Firmware commit command failed",
        moduleId_);
  } else {
    XLOG(INFO) << folly::sformat(
        "cmisModuleFirmwareDownload: Mod{:d}: Step 5: Issued Firmware commit command successful",
        moduleId_);
  }

  usleep(10 * moduleDatapathInitDurationUsec);

  // Set the password to let the privileged operation of firmware download
  bus_->moduleWrite(
      moduleId_,
      {TransceiverI2CApi::ADDR_QSFP, kModulePasswordEntryReg, 4},
      msaPassword_.data());

  return true;
}

/*
 * cmisModuleFirmwareUpgrade
 *
 * This function triggers the firmware download to a module. This specific
 * function does the firmware download for a module in a single thread under
 * the context of calling thread. This function validates the image file and
 * calls the above cmisModuleFirmwareDownload function to do firmware upgrade
 */
bool CmisFirmwareUpgrader::cmisModuleFirmwareUpgrade() {
  std::array<uint8_t, 2> versionNumber;
  bool result;

  XLOG(INFO) << folly::sformat(
      "cmisModuleFirmwareUpgrade: Mod{:d}: Called for port {:d}",
      moduleId_,
      moduleId_);

  // Call the firmware download operation with this image content
  result = cmisModuleFirmwareDownload(
      imageCursor_.data(), imageCursor_.totalLength());
  if (!result) {
    // If the download failed then print the message and return. No need
    // to do any recovery here
    XLOG(INFO) << folly::sformat(
        "cmisModuleFirmwareUpgrade: Mod{:d}: Firmware download function failed for the module",
        moduleId_);

    return false;
  }

  // Find out the current version running on module
  bus_->moduleRead(
      moduleId_,
      {TransceiverI2CApi::ADDR_QSFP, kfirmwareVersionReg, 2},
      versionNumber.data());
  XLOG(INFO) << folly::sformat(
      "cmisModuleFirmwareUpgrade: Mod{:d}: Module Active Firmware Revision now: {:d}.{:d}",
      moduleId_,
      versionNumber[0],
      versionNumber[1]);

  return true;
}

} // namespace facebook::fboss
