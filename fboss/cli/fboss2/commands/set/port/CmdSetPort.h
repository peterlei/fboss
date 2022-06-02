// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#pragma once

#include <folly/String.h>
#include "fboss/cli/fboss2/CmdHandler.h"

namespace facebook::fboss {

struct CmdSetPortTraits : public BaseCommandTraits {
  static constexpr utils::ObjectArgTypeId ObjectArgTypeId =
      utils::ObjectArgTypeId::OBJECT_ARG_TYPE_ID_PORT_LIST;
  using ObjectArgType = std::vector<std::string>;
  using RetType = std::string;
};

class CmdSetPort : public CmdHandler<CmdSetPort, CmdSetPortTraits> {
 public:
  RetType queryClient(
      const HostInfo& /* hostInfo */,
      const std::vector<std::string>& /* queriedPortIds */) {
    throw std::runtime_error(
        "Incomplete command, please use one the subcommands");
    return RetType();
  }

  void printOutput(const RetType& /* model */) {}
};

} // namespace facebook::fboss
