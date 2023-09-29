// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include <folly/dynamic.h>
#include <gtest/gtest.h>

#include <fboss/thrift_cow/gen-cpp2/patch_types.h>
#include <fboss/thrift_cow/visitors/PatchApplier.h>
#include <fboss/thrift_cow/visitors/tests/VisitorTestUtils.h>
#include "fboss/thrift_cow/nodes/Types.h"
#include "fboss/thrift_cow/nodes/tests/gen-cpp2/test_fatal_types.h"
#include "fboss/thrift_cow/nodes/tests/gen-cpp2/test_types.h"

using k = facebook::fboss::test_tags::strings;

namespace {

using folly::dynamic;
using namespace facebook::fboss;
using namespace facebook::fboss::thrift_cow;

} // namespace

namespace facebook::fboss::thrift_cow {

TEST(PatchApplierTests, ModifyPrimitive) {
  auto structA = createSimpleTestStruct();

  PatchNode n;
  n.set_val(serializeBuf<apache::thrift::type_class::string>(
      fsdb::OperProtocol::COMPACT, "new val"));

  auto nodeA = std::make_shared<ThriftStructNode<TestStruct>>(structA);
  PatchApplier<apache::thrift::type_class::string>::apply(
      nodeA->template ref<k::inlineString>(), std::move(n));
  EXPECT_EQ(*nodeA->template ref<k::inlineString>(), "new val");

  n = PatchNode();
  n.set_val(serializeBuf<apache::thrift::type_class::integral>(
      fsdb::OperProtocol::COMPACT, 1234));

  PatchApplier<apache::thrift::type_class::integral>::apply(
      nodeA->template ref<k::inlineInt>(), std::move(n));
  EXPECT_EQ(*nodeA->template ref<k::inlineInt>(), 1234);
}

TEST(PatchApplierTests, ModifyStructMember) {
  auto structA = createSimpleTestStruct();

  auto nodeA = std::make_shared<ThriftStructNode<TestStruct>>(structA);

  PatchNode inlineString;
  inlineString.set_val(serializeBuf<apache::thrift::type_class::string>(
      fsdb::OperProtocol::COMPACT, "new val"));

  StructPatch structPatch;
  structPatch.children() = {
      {TestStructMembers::inlineString::id(), inlineString}};
  PatchNode n;
  n.set_struct_node(std::move(structPatch));

  PatchApplier<apache::thrift::type_class::structure>::apply(
      nodeA, std::move(n));
  EXPECT_EQ(*nodeA->template ref<k::inlineString>(), "new val");
}

TEST(PatchApplierTests, ModifyMapMember) {
  auto structA = createSimpleTestStruct();
  structA.mapOfI32ToI32() = {{123, 456}};
  auto nodeA = std::make_shared<ThriftStructNode<TestStruct>>(structA);

  PatchNode intPatch;
  intPatch.set_val(serializeBuf<apache::thrift::type_class::integral>(
      fsdb::OperProtocol::COMPACT, 42));

  MapPatch mapPatch;
  mapPatch.children() = {{"123", intPatch}};
  PatchNode n;
  n.set_map_node(mapPatch);

  PatchApplier<apache::thrift::type_class::map<
      apache::thrift::type_class::integral,
      apache::thrift::type_class::integral>>::
      apply(nodeA->template ref<k::mapOfI32ToI32>(), std::move(n));
  EXPECT_EQ(*nodeA->template ref<k::mapOfI32ToI32>()->at(123), 42);
}

TEST(PatchApplierTests, ModifyListMember) {
  auto structA = createSimpleTestStruct();
  structA.listOfPrimitives() = {1, 2, 3};

  auto nodeA = std::make_shared<ThriftStructNode<TestStruct>>(structA);

  PatchNode intPatch;
  intPatch.set_val(serializeBuf<apache::thrift::type_class::integral>(
      fsdb::OperProtocol::COMPACT, 42));

  ListPatch listPatch;
  listPatch.children() = {{1, intPatch}};
  PatchNode n;
  n.set_list_node(listPatch);

  PatchApplier<
      apache::thrift::type_class::list<apache::thrift::type_class::integral>>::
      apply(nodeA->template ref<k::listOfPrimitives>(), std::move(n));
  EXPECT_EQ(*nodeA->template ref<k::listOfPrimitives>()->at(1), 42);
}

TEST(PatchApplierTests, ModifyVariantValue) {
  auto structA = createSimpleTestStruct();
  structA.inlineVariant()->set_inlineInt(123);

  auto nodeA = std::make_shared<ThriftStructNode<TestStruct>>(structA);

  PatchNode intPatch;
  intPatch.set_val(serializeBuf<apache::thrift::type_class::integral>(
      fsdb::OperProtocol::COMPACT, 42));

  VariantPatch variantPatch;
  variantPatch.id() = TestStructMembers::inlineInt::id();
  variantPatch.child() = intPatch;
  PatchNode n;
  n.set_variant_node(variantPatch);

  PatchApplier<apache::thrift::type_class::variant>::apply(
      nodeA->template ref<k::inlineVariant>(), std::move(n));
  EXPECT_EQ(*nodeA->template ref<k::inlineVariant>()->ref<k::inlineInt>(), 42);
}

TEST(PatchApplierTests, AddRemoveSetItems) {
  auto structA = createSimpleTestStruct();
  structA.setOfI32() = {1, 2};

  auto nodeA = std::make_shared<ThriftStructNode<TestStruct>>(structA);

  PatchNode intPatch;
  intPatch.set_val(serializeBuf<apache::thrift::type_class::integral>(
      fsdb::OperProtocol::COMPACT, 3));

  PatchNode delPatch;
  delPatch.set_del();

  SetPatch setPatch;
  setPatch.children() = {{"3", intPatch}, {"2", delPatch}};
  PatchNode n;
  n.set_set_node(setPatch);

  PatchApplier<
      apache::thrift::type_class::set<apache::thrift::type_class::integral>>::
      apply(nodeA->template ref<k::setOfI32>(), std::move(n));
  EXPECT_EQ(nodeA->template ref<k::setOfI32>()->size(), 2);
  EXPECT_EQ(nodeA->template ref<k::setOfI32>()->count(1), 1);
  EXPECT_EQ(nodeA->template ref<k::setOfI32>()->count(3), 1);
}

TEST(PatchApplierTests, SetMemberFull) {
  auto structA = createSimpleTestStruct();
  auto structB = structA;

  cfg::L4PortRange t;
  t.min() = 12;
  t.max() = 34;
  structB.inlineStruct() = std::move(t);
  structB.inlineVariant()->set_inlineString("test");
  structB.mapOfI32ToI32() = {{1, 2}, {3, 4}};
  structB.listOfPrimitives() = {5, 6};
  structB.setOfI32() = {7, 8};
  auto nodeA = std::make_shared<ThriftStructNode<TestStruct>>(structA);
  auto nodeB = std::make_shared<ThriftStructNode<TestStruct>>(structB);

  // use second node to create full patches
  PatchNode inlineStruct;
  inlineStruct.set_val(
      nodeB->ref<k::inlineStruct>()->encodeBuf(fsdb::OperProtocol::COMPACT));
  PatchNode inlineVariant;
  inlineVariant.set_val(
      nodeB->ref<k::inlineVariant>()->encodeBuf(fsdb::OperProtocol::COMPACT));
  PatchNode mapOfI32ToI32;
  mapOfI32ToI32.set_val(
      nodeB->ref<k::mapOfI32ToI32>()->encodeBuf(fsdb::OperProtocol::COMPACT));
  PatchNode listOfPrimitives;
  listOfPrimitives.set_val(nodeB->ref<k::listOfPrimitives>()->encodeBuf(
      fsdb::OperProtocol::COMPACT));
  PatchNode setOfI32;
  setOfI32.set_val(
      nodeB->ref<k::setOfI32>()->encodeBuf(fsdb::OperProtocol::COMPACT));
  StructPatch structPatch;
  structPatch.children() = {
      {TestStructMembers::inlineStruct::id(), inlineStruct},
      {TestStructMembers::inlineVariant::id(), inlineVariant},
      {TestStructMembers::mapOfI32ToI32::id(), mapOfI32ToI32},
      {TestStructMembers::listOfPrimitives::id(), listOfPrimitives},
      {TestStructMembers::setOfI32::id(), setOfI32}};
  PatchNode n;
  n.set_struct_node(std::move(structPatch));

  auto ret = RootPatchApplier::apply(nodeA, std::move(n));
  EXPECT_EQ(ret, PatchResult::OK);

  EXPECT_EQ(nodeA->ref<k::inlineStruct>()->toThrift(), *structB.inlineStruct());
  EXPECT_EQ(
      nodeA->ref<k::inlineVariant>()->toThrift(), *structB.inlineVariant());
  EXPECT_EQ(
      nodeA->ref<k::mapOfI32ToI32>()->toThrift(), *structB.mapOfI32ToI32());
  EXPECT_EQ(
      nodeA->ref<k::listOfPrimitives>()->toThrift(),
      *structB.listOfPrimitives());
  EXPECT_EQ(nodeA->ref<k::setOfI32>()->toThrift(), *structB.setOfI32());
}
} // namespace facebook::fboss::thrift_cow
