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

#include <folly/dynamic.h>

#include <memory>

namespace facebook::fboss {
class ForwardingInformationBaseMap;
class RouteTableMap;
class SwSwitch;
struct HwInitResult;
class RoutingInformationBase;

std::unique_ptr<RoutingInformationBase> switchStateToStandaloneRib(
    const std::shared_ptr<RouteTableMap>& swStateRib);

std::shared_ptr<RouteTableMap> standaloneToSwitchStateRib(
    const RoutingInformationBase& standaloneRib);

void programRib(RoutingInformationBase& standaloneRib, SwSwitch* swSwitch);

std::shared_ptr<ForwardingInformationBaseMap> fibFromStandaloneRib(
    RoutingInformationBase& rib);

void handleStandaloneRIBTransition(
    const folly::dynamic& switchStateJson,
    HwInitResult& ret);
} // namespace facebook::fboss
