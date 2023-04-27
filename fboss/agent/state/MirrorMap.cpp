// Copyright 2004-present Facebook. All Rights Reserved.

#include "fboss/agent/state/MirrorMap.h"
#include "fboss/agent/state/Mirror.h"
#include "fboss/agent/state/NodeMap-defs.h"
#include "fboss/agent/state/SwitchState.h"

namespace facebook::fboss {

std::shared_ptr<MirrorMap> MirrorMap::fromThrift(
    const std::map<std::string, state::MirrorFields>& mirrors) {
  auto map = std::make_shared<MirrorMap>();
  for (auto mirror : mirrors) {
    auto node = std::make_shared<Mirror>();
    node->fromThrift(mirror.second);
    // TODO(pshaikh): make this private
    node->markResolved();
    map->insert(*mirror.second.name(), std::move(node));
  }
  return map;
}

std::shared_ptr<Mirror> MultiMirrorMap::getMirrorIf(
    const std::string& name) const {
  return getNodeIf(name);
}

MultiMirrorMap* MultiMirrorMap::modify(std::shared_ptr<SwitchState>* state) {
  if (!isPublished()) {
    CHECK(!(*state)->isPublished());
    return this;
  }

  SwitchState::modify(state);
  auto newMnpuMirrors = clone();
  for (auto mnitr = cbegin(); mnitr != cend(); ++mnitr) {
    (*newMnpuMirrors)[mnitr->first] = mnitr->second->clone();
  }
  auto* ptr = newMnpuMirrors.get();
  (*state)->resetMirrors(std::move(newMnpuMirrors));
  return ptr;
}

std::shared_ptr<MultiMirrorMap> MultiMirrorMap::fromThrift(
    const std::map<std::string, std::map<std::string, state::MirrorFields>>&
        mnpuMirrors) {
  auto mnpuMap = std::make_shared<MultiMirrorMap>();
  for (const auto& matcherAndMirrors : mnpuMirrors) {
    auto map = MirrorMap::fromThrift(matcherAndMirrors.second);
    mnpuMap->insert(matcherAndMirrors.first, std::move(map));
  }
  return mnpuMap;
}

size_t MultiMirrorMap::numMirrors() const {
  size_t cnt = 0;
  for (auto mnitr = cbegin(); mnitr != cend(); ++mnitr) {
    cnt += mnitr->second->size();
  }
  return cnt;
}

template class ThriftMapNode<MirrorMap, MirrorMapTraits>;

} // namespace facebook::fboss
