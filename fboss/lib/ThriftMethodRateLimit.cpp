/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
// Copyright 2014-present Facebook. All Rights Reserved.
#include "fboss/lib/ThriftMethodRateLimit.h"

using namespace std::chrono;

namespace facebook::fboss {

bool ThriftMethodRateLimit::isQpsLimitExceeded(const std::string& method) {
  auto it = method2QpsLimitAndTokenBucket_.find(method);
  if (it != method2QpsLimitAndTokenBucket_.end()) {
    double qpsLimit = it->second.first;
    auto& tokenBucket = it->second.second;
    return !tokenBucket.consume(1.0, qpsLimit, std::max(1.0, qpsLimit));
  }
  return false;
}

double ThriftMethodRateLimit::getQpsLimit(const std::string& method) {
  auto it = method2QpsLimitAndTokenBucket_.find(method);
  if (it != method2QpsLimitAndTokenBucket_.end()) {
    return it->second.first;
  }
  return 0.0;
}

void ThriftMethodRateLimit::incrementDenyCounter(const std::string& method) {
  auto it = method2DenyCounter_.find(method);
  if (it == method2DenyCounter_.end()) {
    return;
  }
  it->second.fetch_add(1, std::memory_order_relaxed);
  aggDenyCounter_.fetch_add(1, std::memory_order_relaxed);
  if (populateCounterFunc_) {
    populateCounterFunc_(
        method,
        it->second.load(std::memory_order_relaxed),
        aggDenyCounter_.load(std::memory_order_relaxed));
  }
}

apache::thrift::PreprocessFunc
ThriftMethodRateLimit::getThriftMethodRateLimitPreprocessFunc(
    std::shared_ptr<ThriftMethodRateLimit> rateLimiter) {
  return [rateLimiter = rateLimiter](
             const apache::thrift::server::PreprocessParams& params)
             -> apache::thrift::PreprocessResult {
    if (rateLimiter->isQpsLimitExceeded(params.method)) {
      const apache::thrift::Cpp2ConnContext& ctx2 = params.connContext;
      std::string client = ctx2.getPeerAddress()->describe();
      XLOG(WARN) << "reject thrift method due to rate limit: "
                 << rateLimiter->getQpsLimit(params.method)
                 << " for method: " << params.method << ", from: " << client;
      rateLimiter->incrementDenyCounter(params.method);
      if (!rateLimiter->getShadowMode()) {
        return apache::thrift::AppOverloadedException(
            fmt::format(
                "reject thrift methd due to rate limit: {} for method: {}",
                rateLimiter->getQpsLimit(params.method),
                params.method),
            "ThriftMethoRateLimit");
      } else {
        XLOG(WARN) << "Rate limit is running in shadow mode and no real impact";
      }
    }
    return apache::thrift::PreprocessResult{};
  };
}

} // namespace facebook::fboss
