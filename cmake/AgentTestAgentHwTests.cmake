# CMake to build libraries and binaries in fboss/agent/hw/test

# In general, libraries and binaries in fboss/foo/bar are built by
# cmake/FooBar.cmake

add_library(agent_hw_test_src
  fboss/agent/test/agent_hw_tests/AgentEmptyTests.cpp
  fboss/agent/test/agent_hw_tests/AgentAclInDiscardCounterTests.cpp
  fboss/agent/test/agent_hw_tests/AgentInNullRouteDiscardsTest.cpp
  fboss/agent/test/agent_hw_tests/AgentPacketSendTests.cpp
  fboss/agent/test/agent_hw_tests/AgentVoqSwitchTests.cpp
  fboss/agent/test/agent_hw_tests/AgentFabricSwitchTests.cpp
)

target_link_libraries(agent_hw_test_src
  agent_test_acl_utils
  agent_test_copp_utils
  pkt_test_utils
  core
  hw_switch_fb303_stats
  config_factory
  agent_hw_test
  ecmp_helper
  hw_test_fabric_utils
)

add_executable(multi_switch_agent_hw_test
  fboss/agent/test/agent_hw_tests/MultiSwitchAgentHwTest.cpp
)

target_link_libraries(multi_switch_agent_hw_test
  -Wl,--whole-archive
  agent_hw_test_src
  agent_hw_test
  multi_switch_agent_ensemble
  Folly::folly
  hw_packet_utils
  -Wl,--no-whole-archive
  ${GTEST}
  ${LIBGMOCK_LIBRARIES}
)

if (BUILD_SAI_FAKE)
add_executable(sai_agent_hw_test-fake
  fboss/agent/test/agent_hw_tests/SaiAgentHwTest.cpp
)

target_link_libraries(sai_agent_hw_test-fake
  -Wl,--whole-archive
  agent_hw_test_src
  fake_sai
  sai_acl_utils
  sai_copp_utils
  hw_packet_utils
  -Wl,--no-whole-archive
)

set_target_properties(fake_sai PROPERTIES COMPILE_FLAGS
  "-DSAI_VER_MAJOR=${SAI_VER_MAJOR} \
  -DSAI_VER_MINOR=${SAI_VER_MINOR} \
  -DSAI_VER_RELEASE=${SAI_VER_RELEASE}"
)
endif()
