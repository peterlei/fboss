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

#include "fboss/agent/state/ForwardingInformationBase.h"
#include "fboss/agent/state/NodeBase.h"
#include "fboss/agent/types.h"

#include <folly/dynamic.h>
#include <memory>

namespace facebook::fboss {

struct ForwardingInformationBaseContainerFields {
  explicit ForwardingInformationBaseContainerFields(RouterID vrf);

  template <typename Fn>
  void forEachChild(Fn fn) {
    fn(fibV4.get());
    fn(fibV6.get());
  }

  RouterID vrf{0};
  std::shared_ptr<ForwardingInformationBaseV4> fibV4{nullptr};
  std::shared_ptr<ForwardingInformationBaseV6> fibV6{nullptr};
};

class ForwardingInformationBaseContainer
    : public NodeBaseT<
          ForwardingInformationBaseContainer,
          ForwardingInformationBaseContainerFields> {
 public:
  explicit ForwardingInformationBaseContainer(RouterID vrf);
  ~ForwardingInformationBaseContainer() override;

  RouterID getID() const;
  const std::shared_ptr<ForwardingInformationBaseV4>& getFibV4() const;
  const std::shared_ptr<ForwardingInformationBaseV6>& getFibV6() const;

  template <typename AddressT>
  const std::shared_ptr<ForwardingInformationBase<AddressT>>& getFib() const {
    if constexpr (std::is_same_v<folly::IPAddressV6, AddressT>) {
      return getFibV6();
    } else {
      return getFibV4();
    }
  }
  template <typename AddressT>
  void setFib(const std::shared_ptr<ForwardingInformationBase<AddressT>>& fib) {
    if constexpr (std::is_same_v<folly::IPAddressV6, AddressT>) {
      writableFields()->fibV6 = fib;
    } else {
      writableFields()->fibV4 = fib;
    }
  }

  ForwardingInformationBaseContainer* modify(
      std::shared_ptr<SwitchState>* state);

  static std::shared_ptr<ForwardingInformationBaseContainer> fromFollyDynamic(
      const folly::dynamic& json);
  folly::dynamic toFollyDynamic() const override;

 private:
  // Inherit the constructors required for clone()
  using NodeBaseT::NodeBaseT;
  friend class CloneAllocator;
};

} // namespace facebook::fboss
