// Copyright 2004-present Facebook. All Rights Reserved.

#include "fboss/agent/AgentFeatures.h"

DEFINE_bool(dsf_4k, false, "Enable DSF Scale Test config");

DEFINE_bool(
    sai_user_defined_trap,
    false,
    "Flag to use user defined trap when programming ACL action to punt packets to cpu queue.");

DEFINE_bool(enable_acl_table_chain_group, false, "Allow ACL table chaining");

DEFINE_int32(
    oper_sync_req_timeout,
    30,
    "request timeout for oper sync client in seconds");

DEFINE_bool(
    classid_for_unresolved_routes,
    false,
    "Flag to set the class ID for unresolved routes that points  to CPU port");

DEFINE_bool(hide_fabric_ports, false, "Elide ports of type fabric");

// DSF Subscriber flags
DEFINE_bool(
    dsf_subscribe,
    true,
    "Issue DSF subscriptions to all DSF Interface nodes");
DEFINE_bool(dsf_subscriber_skip_hw_writes, false, "Skip writing to HW");
DEFINE_bool(
    dsf_subscriber_cache_updated_state,
    false,
    "Cache switch state after update by dsf subsriber");
DEFINE_uint32(
    dsf_gr_hold_time,
    0,
    "GR hold time for FSDB DsfSubscription in sec");

DEFINE_bool(
    classid_for_connected_subnet_routes,
    false,
    "Flag to set the class ID for connected subnet routes that point to RIF");
