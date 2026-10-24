add_executable(edgepulse-agent-test-hook AgentTestHook.cpp)
if(MSVC)
    target_compile_definitions(edgepulse-agent-test-hook PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
edgepulse_enable_warnings(edgepulse-agent-test-hook)
set_target_properties(edgepulse-agent-test-hook PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(edgepulse-agent-tests
        AgentLoop.h
        CertificateFixture.cpp
        FirmwareKeyFixture.cpp
        TestAgentApplication.cpp
        TestAgentClient.cpp
        TestAgentCommand.cpp
        TestAgentConfiguration.cpp
        TestAgentExecutors.cpp
        TestAgentFirmware.cpp
        TestAgentIdentity.cpp
        TestAgentState.cpp
        TestAgentTelemetry.cpp)
target_compile_definitions(edgepulse-agent-tests PRIVATE EDGEPULSE_AGENT_TEST_HOOK_PATH="$<TARGET_FILE:edgepulse-agent-test-hook>")
add_dependencies(edgepulse-agent-tests edgepulse-agent-test-hook)
target_link_libraries(edgepulse-agent-tests PRIVATE edgepulse-agent-core edgepulse-test-support GTest::gtest_main)
edgepulse_enable_warnings(edgepulse-agent-tests)
set_target_properties(edgepulse-agent-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
gtest_discover_tests(edgepulse-agent-tests DISCOVERY_MODE PRE_TEST)
