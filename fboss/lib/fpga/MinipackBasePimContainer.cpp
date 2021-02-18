// Copyright 2004-present Facebook. All Rights Reserved.

#include "fboss/lib/fpga/MinipackBasePimContainer.h"

namespace {
constexpr uint32_t kQsfpManagementRegStart = 0x40;
constexpr uint32_t kQsfpManagementRegSize = 0x40;
constexpr uint32_t kPortLedStart = 0x0310;
constexpr uint32_t kPortLedSize = 0x4;
} // namespace

namespace facebook::fboss {

// To avoid ambiguity, we explicitly decided the pim number starts from 2.
MinipackBasePimContainer::MinipackBasePimContainer(
    int pim,
    const std::string& name,
    FpgaDevice* device,
    uint32_t pimStart,
    uint32_t pimSize,
    unsigned int portsPerPim)
    : pim_(pim) {
  pimQsfpController_ = std::make_unique<FbFpgaPimQsfpController>(
      std::make_unique<FpgaMemoryRegion>(
          "PimQsfpController",
          device,
          pimStart + kQsfpManagementRegStart,
          kQsfpManagementRegSize),
      portsPerPim);

  for (auto led = 0; led < kNumLedPerPim; led++) {
    ledControllers_[led] =
        std::make_unique<MinipackLed>(std::make_unique<FpgaMemoryRegister>(
            folly::format("{}-led{}", name, led).str(),
            device,
            pimStart + kPortLedStart + led * kPortLedSize));
  }

  pimController_ = std::make_unique<MinipackPimController>(
      std::make_unique<FpgaMemoryRegion>(
          folly::format("pim{:d}-controller", pim).str(),
          device,
          pimStart,
          pimSize));
}

FbFpgaPimQsfpController* MinipackBasePimContainer::getPimQsfpController() {
  return pimQsfpController_.get();
}

MinipackLed* MinipackBasePimContainer::getLedController(int qsfp) const {
  CHECK(qsfp >= 0 && qsfp < kNumLedPerPim);
  return ledControllers_[qsfp].get();
}
} // namespace facebook::fboss
