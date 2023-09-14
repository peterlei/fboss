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

#include "fboss/agent/platforms/sai/SaiTajoPlatform.h"

namespace facebook::fboss {
class GaronneAsic;

class SaiSandiaPlatform : public SaiTajoPlatform {
 public:
  SaiSandiaPlatform(
      std::unique_ptr<PlatformProductInfo> productInfo,
      folly::MacAddress localMac,
      const std::string& platformMappingStr);
  ~SaiSandiaPlatform() override;
  std::string getHwConfig() override;
  HwAsic* getAsic() const override;

 private:
  void setupAsic(
      cfg::SwitchType switchType,
      std::optional<int64_t> switchId,
      int16_t switchIndex,
      std::optional<cfg::Range64> systemPortRange,
      folly::MacAddress& mac) override;
  std::unique_ptr<GaronneAsic> asic_;
};

} // namespace facebook::fboss
