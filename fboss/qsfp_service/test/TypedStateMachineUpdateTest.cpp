/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "fboss/qsfp_service/TypedStateMachineUpdate.h"
#include <gtest/gtest.h>

namespace facebook::fboss {

TEST(BlockingStateMachineUpdateResultTest, SignalCompetionAndWait) {
  auto result = std::make_shared<BlockingStateMachineUpdateResult>();
  result->signalCompletion();
  result->wait();

  try {
    result->wait();
  } catch (...) {
    FAIL() << "Expected no exception";
  }
}

} // namespace facebook::fboss
