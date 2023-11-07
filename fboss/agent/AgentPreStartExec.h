// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#pragma once

#include <memory>

namespace facebook::fboss {

class AgentDirectoryUtil;
class AgentConfig;
class AgentCommandExecutor;

class AgentPreStartExec {
 public:
  AgentPreStartExec() {}
  void run();
  void run(
      AgentCommandExecutor* commandExecutor,
      const AgentDirectoryUtil& dirUtil,
      std::unique_ptr<AgentConfig> config,
      bool cppWedgeAgentWrapper);
};

} // namespace facebook::fboss
