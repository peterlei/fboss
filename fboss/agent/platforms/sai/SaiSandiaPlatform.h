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
class EbroAsic;

class SaiSandiaPlatform : public SaiTajoPlatform {
 public:
  explicit SaiSandiaPlatform(
      std::unique_ptr<PlatformProductInfo> productInfo,
      folly::MacAddress localMac);
  ~SaiSandiaPlatform() override;
  std::string getHwConfig() override;
  HwAsic* getAsic() const override;

  std::unique_ptr<EbroAsic> asic_;
};

} // namespace facebook::fboss
