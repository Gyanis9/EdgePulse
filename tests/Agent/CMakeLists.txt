add_executable(edgepulse-agent-tests
        CertificateFixture.cpp
        TestAgentApplication.cpp
        TestAgentClient.cpp
        TestAgentConfiguration.cpp
        TestAgentIdentity.cpp
        TestAgentState.cpp)
target_link_libraries(edgepulse-agent-tests PRIVATE edgepulse-agent-core edgepulse-test-support GTest::gtest_main)
edgepulse_enable_warnings(edgepulse-agent-tests)
set_target_properties(edgepulse-agent-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
gtest_discover_tests(edgepulse-agent-tests DISCOVERY_MODE PRE_TEST)
