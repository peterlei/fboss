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

extern "C" {
#include <bcm/types.h>
#include <bcm/udf.h>
}

#include "fboss/agent/FbossError.h"
#include "fboss/agent/hw/bcm/types.h"
#include "fboss/agent/state/UdfGroup.h"

namespace facebook::fboss {

class BcmSwitch;

/**
 * BcmUdfGroup is the class to abstract bcm_udf_create in
 * BcmSwitch
 */
class BcmUdfGroup {
 public:
  BcmUdfGroup(BcmSwitch* hw, const std::shared_ptr<UdfGroup>& udfGroup);
  ~BcmUdfGroup();

  bcm_udf_id_t getUdfId() {
    return udfId_;
  }

 private:
  int udfCreate(bcm_udf_t* udfInfo);
  int udfDelete(bcm_udf_id_t udfId);

  int udfPktFormatAdd(bcm_udf_id_t udfId, bcm_udf_pkt_format_id_t pktFormatId);
  int udfPktFormatDelete(
      bcm_udf_id_t udfId,
      bcm_udf_pkt_format_id_t pktFormatId);

  BcmSwitch* hw_;
  bcm_udf_id_t udfId_ = 0;
  std::string udfGroupName_;
};

} // namespace facebook::fboss
