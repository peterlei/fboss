// (c) Facebook, Inc. and its affiliates. Confidential and proprietary.
#pragma once

#include <memory>

#include "fboss/platform/weutil/WeutilDarwin.h"

namespace facebook::fboss::platform {
/*
 * Creates an instance of WeutilInterface based on the eeprom name or
 * eepromPath. If eepromPath is specified, we ignore the eepromName.
 */
std::unique_ptr<WeutilInterface> createWeUtilIntf(
    const std::string& eepromName,
    const std::string& eepromPath);

/*
 * Get the EEPROM Names based on the default config of the platform.
 */
std::vector<std::string> getEepromNames();
} // namespace facebook::fboss::platform
