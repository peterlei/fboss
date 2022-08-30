/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "fboss/agent/gen-cpp2/switch_config_types.h"
#include "fboss/agent/hw/sai/switch/SaiAclTableGroupManager.h"
#include "fboss/agent/hw/sai/switch/SaiSwitch.h"
#include "fboss/agent/hw/test/ConfigFactory.h"
#include "fboss/agent/hw/test/HwTest.h"
#include "fboss/agent/hw/test/HwTestAclUtils.h"
#include "fboss/agent/hw/test/dataplane_tests/HwTestDscpMarkingUtils.h"
#include "fboss/agent/hw/test/dataplane_tests/HwTestQueuePerHostUtils.h"

#include "fboss/agent/state/SwitchState.h"

#include <string>

DECLARE_bool(enable_acl_table_group);

using namespace facebook::fboss;

namespace {

bool isAclTableGroupEnabled(
    const HwSwitch* hwSwitch,
    sai_acl_stage_t aclStage) {
  const auto& aclTableGroupManager = static_cast<const SaiSwitch*>(hwSwitch)
                                         ->managerTable()
                                         ->aclTableGroupManager();

  auto aclTableGroupHandle =
      aclTableGroupManager.getAclTableGroupHandle(aclStage);
  return aclTableGroupHandle != nullptr;
}

} // namespace

namespace facebook::fboss {

class SaiAclTableGroupTest : public HwTest {
 protected:
  void SetUp() override {
    FLAGS_enable_acl_table_group = true;
    HwTest::SetUp();
  }

  cfg::SwitchConfig initialConfig() const {
    return utility::oneL3IntfConfig(getHwSwitch(), masterLogicalPortIds()[0]);
  }

  bool isSupported() const {
    return HwTest::isSupported(HwAsic::Feature::MULTIPLE_ACL_TABLES);
  }

  void addAclTable1(cfg::SwitchConfig& cfg) {
    // table1 with dscp matcher ACL entry
    utility::addAclTable(
        &cfg,
        kAclTable1(),
        1 /* priority */,
        {cfg::AclTableActionType::PACKET_ACTION},
        {cfg::AclTableQualifier::DSCP});
  }

  void addAclTable2(cfg::SwitchConfig& cfg) {
    // table2 with ttl matcher ACL entry
    utility::addAclTable(
        &cfg,
        kAclTable2(),
        2 /* priority */,
        {cfg::AclTableActionType::COUNTER},
        {cfg::AclTableQualifier::TTL});
  }

  void addAclTable1Entry1(cfg::SwitchConfig& cfg) {
    auto* acl1 = utility::addAcl(
        &cfg, kAclTable1Entry1(), cfg::AclActionType::DENY, kAclTable1());
    acl1->dscp() = 0x20;
  }

  void addAclTable2Entry1(cfg::SwitchConfig& cfg) {
    auto* acl2 = utility::addAcl(
        &cfg, kAclTable2Entry1(), cfg::AclActionType::DENY, kAclTable2());
    cfg::Ttl ttl;
    std::tie(*ttl.value(), *ttl.mask()) = std::make_tuple(0x80, 0x80);
    acl2->ttl() = ttl;
  }

  void verifyMultipleTableWithEntriesHelper() {
    ASSERT_TRUE(isAclTableGroupEnabled(getHwSwitch(), SAI_ACL_STAGE_INGRESS));
    ASSERT_TRUE(utility::isAclTableEnabled(getHwSwitch(), kAclTable1()));
    EXPECT_EQ(
        utility::getAclTableNumAclEntries(getHwSwitch(), kAclTable1()), 1);
    utility::checkSwHwAclMatch(
        getHwSwitch(),
        getProgrammedState(),
        kAclTable1Entry1(),
        kAclStage(),
        kAclTable1());

    ASSERT_TRUE(utility::isAclTableEnabled(getHwSwitch(), kAclTable2()));
    EXPECT_EQ(
        utility::getAclTableNumAclEntries(getHwSwitch(), kAclTable2()), 1);
    utility::checkSwHwAclMatch(
        getHwSwitch(),
        getProgrammedState(),
        kAclTable2Entry1(),
        kAclStage(),
        kAclTable2());
  }

  std::string kAclTable1() const {
    return "table1";
  }

  std::string kAclTable2() const {
    return "table2";
  }

  std::string kAclTable1Entry1() const {
    return "table1_entry1";
  }

  std::string kAclTable2Entry1() const {
    return "table2_entry1";
  }

  cfg::AclStage kAclStage() const {
    return cfg::AclStage::INGRESS;
  }

  std::string kQphDscpTable() const {
    return "qph_dscp_table";
  }

  void addQphDscpAclTable(cfg::SwitchConfig* newCfg) {
    // Table 1: For QPH and Dscp Acl.
    utility::addAclTable(
        newCfg,
        kQphDscpTable(),
        1 /* priority */,
        {cfg::AclTableActionType::PACKET_ACTION,
         cfg::AclTableActionType::COUNTER},
        {cfg::AclTableQualifier::L4_SRC_PORT,
         cfg::AclTableQualifier::L4_DST_PORT,
         cfg::AclTableQualifier::IP_PROTOCOL,
         cfg::AclTableQualifier::ICMPV4_TYPE,
         cfg::AclTableQualifier::ICMPV4_CODE,
         cfg::AclTableQualifier::ICMPV6_TYPE,
         cfg::AclTableQualifier::ICMPV6_CODE,
         cfg::AclTableQualifier::DSCP,
         cfg::AclTableQualifier::LOOKUP_CLASS_L2,
         cfg::AclTableQualifier::LOOKUP_CLASS_NEIGHBOR,
         cfg::AclTableQualifier::LOOKUP_CLASS_ROUTE});

    utility::addQueuePerHostAclEntry(newCfg, kQphDscpTable());
    utility::addDscpAclEntryWithCounter(newCfg, kQphDscpTable());
  }

  void addTwoAclTables(cfg::SwitchConfig* newCfg) {
    utility::addAclTableGroup(newCfg, kAclStage(), "Ingress Table Group");

    // Table 1: Create QPH and DSCP ACLs in the same table.
    addQphDscpAclTable(newCfg);

    // Table 2: Create TTL acl Table follwed by entry.
    // This utlity call adds the TTL Acl entry as well.
    utility::addTtlAclTable(newCfg, 2 /* priority */);
    applyNewConfig(*newCfg);
  }

  // Delete the QphDscpAclTable
  void deleteQphDscpAclTable(cfg::SwitchConfig* newCfg) {
    utility::delAclTable(newCfg, kQphDscpTable());
    utility::deleteQueuePerHostMatchers(newCfg);
    utility::delDscpMatchers(newCfg);
    applyNewConfig(*newCfg);
  }

  // Delete the TtlAclTable
  void deleteTtlAclTable(cfg::SwitchConfig* newCfg) {
    utility::delAclTable(newCfg, utility::getTtlAclTableName());
    utility::deleteTtlCounters(newCfg);
    applyNewConfig(*newCfg);
  }
};

TEST_F(SaiAclTableGroupTest, SingleAclTableGroup) {
  ASSERT_TRUE(isSupported());

  auto setup = [this]() {
    auto newCfg = initialConfig();

    utility::addAclTableGroup(&newCfg, kAclStage(), "Ingress Table Group");

    applyNewConfig(newCfg);
  };

  auto verify = [=]() {
    ASSERT_TRUE(isAclTableGroupEnabled(getHwSwitch(), SAI_ACL_STAGE_INGRESS));
  };

  verifyAcrossWarmBoots(setup, verify);
}

TEST_F(SaiAclTableGroupTest, MultipleTablesNoEntries) {
  ASSERT_TRUE(isSupported());

  auto setup = [this]() {
    auto newCfg = initialConfig();

    utility::addAclTableGroup(&newCfg, kAclStage(), "Ingress Table Group");
    addAclTable1(newCfg);
    addAclTable2(newCfg);

    applyNewConfig(newCfg);
  };

  auto verify = [=]() {
    ASSERT_TRUE(isAclTableGroupEnabled(getHwSwitch(), SAI_ACL_STAGE_INGRESS));
    ASSERT_TRUE(utility::isAclTableEnabled(getHwSwitch(), kAclTable1()));
    ASSERT_TRUE(utility::isAclTableEnabled(getHwSwitch(), kAclTable2()));
  };

  verifyAcrossWarmBoots(setup, verify);
}

TEST_F(SaiAclTableGroupTest, MultipleTablesWithEntries) {
  ASSERT_TRUE(isSupported());

  auto setup = [this]() {
    auto newCfg = initialConfig();

    utility::addAclTableGroup(&newCfg, kAclStage(), "Ingress Table Group");
    addAclTable1(newCfg);
    addAclTable1Entry1(newCfg);
    addAclTable2(newCfg);
    addAclTable2Entry1(newCfg);

    applyNewConfig(newCfg);
  };

  auto verify = [=]() { verifyMultipleTableWithEntriesHelper(); };

  verifyAcrossWarmBoots(setup, verify);
}

TEST_F(SaiAclTableGroupTest, AddTablesThenEntries) {
  ASSERT_TRUE(isSupported());

  auto setup = [this]() {
    auto newCfg = initialConfig();

    utility::addAclTableGroup(&newCfg, kAclStage(), "Ingress Table Group");
    addAclTable1(newCfg);
    addAclTable2(newCfg);
    applyNewConfig(newCfg);

    addAclTable1Entry1(newCfg);
    addAclTable2Entry1(newCfg);
    applyNewConfig(newCfg);
  };

  auto verify = [=]() { verifyMultipleTableWithEntriesHelper(); };

  verifyAcrossWarmBoots(setup, verify);
}

TEST_F(SaiAclTableGroupTest, RemoveAclTable) {
  ASSERT_TRUE(isSupported());

  auto setup = [this]() {
    auto newCfg = initialConfig();

    utility::addAclTableGroup(&newCfg, kAclStage(), "Ingress Table Group");
    addAclTable1(newCfg);
    addAclTable1Entry1(newCfg);
    addAclTable2(newCfg);
    applyNewConfig(newCfg);

    // Remove one Table with entries, and another with no entries
    utility::delAclTable(&newCfg, kAclTable1());
    utility::delAclTable(&newCfg, kAclTable2());
    applyNewConfig(newCfg);
  };

  auto verify = [=]() {
    ASSERT_TRUE(isAclTableGroupEnabled(getHwSwitch(), SAI_ACL_STAGE_INGRESS));
    ASSERT_FALSE(utility::isAclTableEnabled(getHwSwitch(), kAclTable1()));
    ASSERT_FALSE(utility::isAclTableEnabled(getHwSwitch(), kAclTable2()));
  };

  verifyAcrossWarmBoots(setup, verify);
}

/*
 * The below testcases vary from the above in the sense that they add the QPH
 * and DSCP ACLs in the Table 1 and TTL ACL in Table 2 (production use case) and
 * then tests deletion and addition of these tables in the config
 */
TEST_F(SaiAclTableGroupTest, AddTwoTablesDeleteFirst) {
  ASSERT_TRUE(isSupported());

  auto setup = [this]() {
    auto newCfg = initialConfig();
    addTwoAclTables(&newCfg);
    // Delete the First table
    deleteQphDscpAclTable(&newCfg);
  };

  auto verify = [=]() {
    ASSERT_TRUE(isAclTableGroupEnabled(getHwSwitch(), SAI_ACL_STAGE_INGRESS));
    ASSERT_FALSE(utility::isAclTableEnabled(getHwSwitch(), kQphDscpTable()));
    ASSERT_TRUE(utility::isAclTableEnabled(
        getHwSwitch(), utility::getTtlAclTableName()));
  };

  verifyAcrossWarmBoots(setup, verify);
}

TEST_F(SaiAclTableGroupTest, AddTwoTablesDeleteSecond) {
  ASSERT_TRUE(isSupported());

  auto setup = [this]() {
    auto newCfg = initialConfig();
    addTwoAclTables(&newCfg);
    // Delete the Second Table
    deleteTtlAclTable(&newCfg);
  };

  auto verify = [=]() {
    ASSERT_TRUE(isAclTableGroupEnabled(getHwSwitch(), SAI_ACL_STAGE_INGRESS));
    ASSERT_TRUE(utility::isAclTableEnabled(getHwSwitch(), kQphDscpTable()));
    ASSERT_FALSE(utility::isAclTableEnabled(
        getHwSwitch(), utility::getTtlAclTableName()));
  };

  verifyAcrossWarmBoots(setup, verify);
}

TEST_F(SaiAclTableGroupTest, AddTwoTablesDeleteAddFirst) {
  ASSERT_TRUE(isSupported());

  auto setup = [this]() {
    auto newCfg = initialConfig();
    addTwoAclTables(&newCfg);
    // Delete and Readd the first Acl Table
    deleteQphDscpAclTable(&newCfg);
    addQphDscpAclTable(&newCfg);
    applyNewConfig(newCfg);
  };

  auto verify = [=]() {
    ASSERT_TRUE(isAclTableGroupEnabled(getHwSwitch(), SAI_ACL_STAGE_INGRESS));
    ASSERT_TRUE(utility::isAclTableEnabled(getHwSwitch(), kQphDscpTable()));
    ASSERT_TRUE(utility::isAclTableEnabled(
        getHwSwitch(), utility::getTtlAclTableName()));
  };

  verifyAcrossWarmBoots(setup, verify);
}

TEST_F(SaiAclTableGroupTest, AddTwoTablesDeleteAddSecond) {
  ASSERT_TRUE(isSupported());

  auto setup = [this]() {
    auto newCfg = initialConfig();
    addTwoAclTables(&newCfg);
    // Delete and Readd the second Acl Table
    deleteTtlAclTable(&newCfg);
    utility::addTtlAclTable(&newCfg, 2 /* priority */);
    applyNewConfig(newCfg);
  };

  auto verify = [=]() {
    ASSERT_TRUE(isAclTableGroupEnabled(getHwSwitch(), SAI_ACL_STAGE_INGRESS));
    ASSERT_TRUE(utility::isAclTableEnabled(getHwSwitch(), kQphDscpTable()));
    ASSERT_TRUE(utility::isAclTableEnabled(
        getHwSwitch(), utility::getTtlAclTableName()));
  };

  verifyAcrossWarmBoots(setup, verify);
}
} // namespace facebook::fboss
