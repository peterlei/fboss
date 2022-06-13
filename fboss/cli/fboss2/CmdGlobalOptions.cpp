/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "fboss/cli/fboss2/CmdGlobalOptions.h"
#include <CLI/Validators.hpp>

#include <folly/Singleton.h>
#include <folly/String.h>

namespace {
struct singleton_tag_type {};
} // namespace

using facebook::fboss::CmdGlobalOptions;
static folly::Singleton<CmdGlobalOptions, singleton_tag_type>
    cmdGlobalOptions{};
std::shared_ptr<CmdGlobalOptions> CmdGlobalOptions::getInstance() {
  return cmdGlobalOptions.try_get();
}

namespace facebook::fboss {

void CmdGlobalOptions::init(CLI::App& app) {
  app.add_option("-H, --host", hosts_, "Hostname(s) to query");
  app.add_option("-l,--loglevel", logLevel_, "Debug log level");
  app.add_option("--file", file_, "filename, queries every host in the file")
      ->check(CLI::ExistingFile);
  app.add_option("--fmt", fmt_, OutputFormat::getDescription())
      ->check(OutputFormat::getValidator());
  app.add_option(
         "--agent-port", agentThriftPort_, "Agent thrift port to connect to")
      ->check(CLI::PositiveNumber);
  app.add_option(
         "--qsfp-port",
         qsfpThriftPort_,
         "QsfpService thrift port to connect to")
      ->check(CLI::PositiveNumber);
  app.add_option(
      "--color", color_, "color (no, yes => yes for tty and no for pipe)");
  app.add_option(
      "--filter",
      filter_,
      "filter expression. expression must be of the form TERM1&&TERM2||TERM3||TERM4... where each TERMi is of the form <key op value> See specific commands for list of available filters. Please note the specific whitespace requirements for the command");

  initAdditional(app);
}

// assuming each filter term to be "key op value" (with spaces).
CmdGlobalOptions::UnionList CmdGlobalOptions::getFilters() const {
  if (filter_.size() == 0) {
    return {};
  }
  /*logic for parsing the filters from command line:
    The command line filter string is expected to be of the form
    TERM1&&TERM2||TERM3||TERM4... where each TERMi is of the form <key op
    value>.
    We don't support parenthesization for now. The implicit precedence
    follows the C-style precedence rules where && has higher priority than ||.
    Due to this, we first split the input string on '||' to get a UnionList
    which form the upper-level filters (unioned).
    Next, each element of the union list is an intersection of terms. So, for
    the next level, we split based on &&.
    Next we parse the individual key op value in a FilterTerm.
    Hence, overall, a filter input of the form A < B||C > D&&E = F||G <= H would
    be parsed as: (A < B) || (C > D && E = F) || (G <= H).
  */
  UnionList parsedFilters;
  std::vector<std::string> unionVec;
  folly::split("||", filter_, unionVec);
  for (std::string unionStr : unionVec) {
    std::vector<std::string> interVec;
    IntersectionList intersectList;
    folly::split("&&", unionStr, interVec);
    for (std::string termStr : interVec) {
      std::vector<std::string> filterTermVector;
      folly::split(" ", termStr, filterTermVector);
      if (filterTermVector.size() != 3) {
        std::cout << "Each filter term must be of the form key op value. ";
        exit(1);
      }
      FilterTerm filterTerm = make_tuple(
          filterTermVector[0], filterTermVector[1], filterTermVector[2]);
      intersectList.push_back(filterTerm);
    }
    parsedFilters.push_back(intersectList);
  }
  return parsedFilters;
}

} // namespace facebook::fboss
