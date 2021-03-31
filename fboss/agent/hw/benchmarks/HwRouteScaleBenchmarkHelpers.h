/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "fboss/agent/HwSwitch.h"
#include "fboss/agent/hw/test/ConfigFactory.h"
#include "fboss/agent/hw/test/HwSwitchEnsemble.h"
#include "fboss/agent/hw/test/HwSwitchEnsembleFactory.h"
#include "fboss/agent/hw/test/HwSwitchEnsembleRouteUpdateWrapper.h"

#include <folly/Benchmark.h>
#include <iostream>
#include "fboss/agent/FibHelpers.h"
#include "fboss/agent/Utils.h"
#include "fboss/agent/test/ResourceLibUtil.h"
#include "fboss/lib/FunctionCallTimeReporter.h"

DECLARE_bool(json);

namespace facebook::fboss {

/*
 * Helper function to benchmark speed of route insertion, deletion
 * in HW. This function inits the ASIC, generate switch states for
 * a given route distribution and then measures the time it takes
 * to add (or delete post addition) these routes
 */
template <typename RouteScaleGeneratorT>
void routeAddDelBenchmarker(bool measureAdd) {
  folly::BenchmarkSuspender suspender;
  auto ensemble = createHwEnsemble(HwSwitchEnsemble::getAllFeatures());
  auto config = utility::onePortPerVlanConfig(
      ensemble->getHwSwitch(), ensemble->masterLogicalPortIds());
  ensemble->applyInitialConfig(config);
  auto routeGenerator = RouteScaleGeneratorT(
      ensemble->getProgrammedState(), ensemble->isStandaloneRibEnabled());
  if (!routeGenerator.isSupported(ensemble->getPlatform()->getMode())) {
    // skip if this is not supported for a platform
    return;
  }
  const RouterID kRid(0);
  auto routeChunks = routeGenerator.getThriftRoutes();

  std::atomic<bool> done{false};

  auto doLookups = [&ensemble, kRid, &done]() {
    auto programmedState = ensemble->getProgrammedState();
    std::vector<folly::IPAddressV6> addrsToLookup;
    utility::IPAddressGenerator<folly::IPAddressV6> ipAddrGen;
    for (auto i = 0; i < 100; ++i) {
      addrsToLookup.emplace_back(ipAddrGen.getNext());
    }
    double worstCaseLookupMsecs = 0;
    while (!done) {
      {
        StopWatch lookupTimer(std::nullopt, FLAGS_json);
        for (auto i = 0; i < 10; ++i) {
          std::for_each(
              addrsToLookup.begin(),
              addrsToLookup.end(),
              [&ensemble, &programmedState, kRid](const auto& addr) {
                findLongestMatchRoute(
                    ensemble->getRib(), kRid, addr, programmedState);
              });
        }
        auto msecsElapsed = lookupTimer.msecsElapsed().count();
        worstCaseLookupMsecs = msecsElapsed > worstCaseLookupMsecs
            ? msecsElapsed
            : worstCaseLookupMsecs;
      }
      // Give some breathing room so the thread doesn't  eat all its quantum
      // of ticks and gets scheduled out in middle of loookups
      usleep(1000);
    }
    if (FLAGS_json) {
      folly::dynamic time = folly::dynamic::object;
      time["worst_case_lookup_msescs"] = worstCaseLookupMsecs;
      std::cout << time << std::endl;
    } else {
      XLOG(INFO) << "worst_case_lookup_msescs"
                 << " : " << worstCaseLookupMsecs;
    }
  };
  // Start parallel lookup thread
  std::thread lookupThread([&doLookups]() { doLookups(); });
  HwSwitchEnsembleRouteUpdateWrapper updater(ensemble.get());
  if (measureAdd) {
    ScopedCallTimer timeIt;
    // Activate benchmarker before applying switch states
    // for adding routes to h/w
    suspender.dismiss();
    // Program 1 chunk to seed ~4k routes
    // program remaining chunks
    updater.programRoutes(RouterID(0), ClientID::BGPD, routeChunks);
    // We are about to blow away all routes, before that
    // deactivate benchmark measurement.
    suspender.rehire();
  } else {
    updater.programRoutes(kRid, ClientID::BGPD, routeChunks);
    ScopedCallTimer timeIt;
    // We are about to blow away all routes, before that
    // activate benchmark measurement.
    suspender.dismiss();
    updater.unprogramRoutes(kRid, ClientID::BGPD, routeChunks);
    suspender.rehire();
  }
  done = true;
  // Lookup with all routes installaed
  lookupThread.join();
}

#define ROUTE_ADD_BENCHMARK(name, RouteScaleGeneratorT) \
  BENCHMARK(name) {                                     \
    routeAddDelBenchmarker<RouteScaleGeneratorT>(true); \
  }

#define ROUTE_DEL_BENCHMARK(name, RouteScaleGeneratorT)  \
  BENCHMARK(name) {                                      \
    routeAddDelBenchmarker<RouteScaleGeneratorT>(false); \
  }

} // namespace facebook::fboss
