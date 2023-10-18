// Copyright 2004-present Facebook. All Rights Reserved.
//

#include <fb303/FollyLoggingHandler.h>
#include <folly/experimental/FunctionScheduler.h>
#include <folly/logging/Init.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include "fboss/platform/config_lib/ConfigLib.h"
#include "fboss/platform/data_corral_service/DataCorralServiceThriftHandler.h"
#include "fboss/platform/data_corral_service/Flags.h"
#include "fboss/platform/data_corral_service/FruPresenceExplorer.h"
#include "fboss/platform/data_corral_service/if/gen-cpp2/data_corral_service_types.h"
#include "fboss/platform/helpers/Init.h"

using namespace facebook;
using namespace facebook::fboss::platform;
using namespace facebook::fboss::platform::data_corral_service;

DEFINE_int32(
    refresh_interval_ms,
    10000,
    "How often to refresh FRU and program LED.");

int main(int argc, char** argv) {
  fb303::registerFollyLoggingOptionHandlers();
  helpers::init(argc, argv);

  LedManagerConfig config;
  auto configJson = ConfigLib().getLedManagerConfig();
  apache::thrift::SimpleJSONSerializer::deserialize<LedManagerConfig>(
      configJson, config);

  auto ledManager = std::make_shared<LedManager>(
      *config.systemLedConfig(), *config.fruTypeLedConfigs());
  auto fruPresenceExplorer =
      std::make_shared<FruPresenceExplorer>(*config.fruConfigs(), ledManager);

  folly::FunctionScheduler scheduler;
  scheduler.addFunction(
      [&]() { fruPresenceExplorer->detectFruPresence(); },
      std::chrono::milliseconds(FLAGS_refresh_interval_ms),
      "PresenceDetection");
  scheduler.start();

  auto server = std::make_shared<apache::thrift::ThriftServer>();
  auto handler = std::make_shared<DataCorralServiceThriftHandler>();

  server->setPort(FLAGS_thrift_port);
  server->setInterface(handler);
  server->setAllowPlaintextOnLoopback(true);
  helpers::runThriftService(
      server, handler, "DataCorralService", FLAGS_thrift_port);

  return 0;
}
