find_package(GTest REQUIRED)
include(GoogleTest)

add_library(edgepulse-test-support STATIC
        TestSupport/AuthTestContext.cpp
        TestSupport/HttpClientHarness.cpp)
target_include_directories(edgepulse-test-support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(edgepulse-test-support PRIVATE EDGEPULSE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(edgepulse-test-support
        PUBLIC EdgePulse::Core
               AsynGyanis::Net
               AsynGyanis::Base
               AsynGyanis::Core
               GTest::gtest)
edgepulse_enable_warnings(edgepulse-test-support)

add_executable(edgepulse-unit-tests
        Unit/TestCaptchaService.cpp
        Unit/TestCertificateBinding.cpp
        Unit/TestCsvWriter.cpp
        Unit/TestIdGenerator.cpp
        Unit/TestMemoryKeyValueStore.cpp
        Unit/TestOidcJwt.cpp
        Unit/TestMigrationSqlSplitter.cpp
        Unit/TestNodeRepositoryRetry.cpp
        Unit/TestPagination.cpp
        Unit/TestPasswordHash.cpp
        Unit/TestSessionStore.cpp
        Unit/TestSseBroadcaster.cpp
        Unit/TestTimeFormat.cpp
        Unit/TestUdpTelemetryProtocol.cpp)
target_include_directories(edgepulse-unit-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(edgepulse-unit-tests PRIVATE EdgePulse::Core GTest::gtest_main)
edgepulse_enable_warnings(edgepulse-unit-tests)
set_target_properties(edgepulse-unit-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
gtest_discover_tests(edgepulse-unit-tests DISCOVERY_MODE PRE_TEST)

add_executable(edgepulse-integration-tests
        Integration/TestAgentAuthorization.cpp
        Integration/TestAuthService.cpp
        Integration/TestDeviceService.cpp
        Integration/TestEnrollment.cpp
        Integration/TestExternalStores.cpp
        Integration/TestMigrations.cpp
        Integration/TestNodeConfig.cpp
        Integration/TestNodeLifecycle.cpp
        Integration/TestNotificationChannels.cpp
        Integration/TestRealtimeBus.cpp
        Integration/TestFleetServices.cpp
        Integration/TestSystemServices.cpp
        Integration/TestTenantIsolation.cpp
        Integration/TestUdpTelemetry.cpp)
target_include_directories(edgepulse-integration-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(edgepulse-integration-tests PRIVATE EDGEPULSE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(edgepulse-integration-tests PRIVATE EdgePulse::Core EdgePulse::Plugins edgepulse-test-support GTest::gtest_main)
edgepulse_enable_warnings(edgepulse-integration-tests)
set_target_properties(edgepulse-integration-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
gtest_discover_tests(edgepulse-integration-tests DISCOVERY_MODE PRE_TEST)

add_executable(edgepulse-protocol-tests
        Protocol/TestApiResponse.cpp
        Protocol/TestAuthEndpoints.cpp
        Protocol/TestEnrollmentEndpoints.cpp
        Protocol/TestHealthServer.cpp
        Protocol/TestFleetEndpoints.cpp
        Protocol/TestNodeConfigEndpoints.cpp
        Protocol/TestNodeEndpoints.cpp
        Protocol/TestReverseProxy.cpp
        Protocol/TestSystemEndpoints.cpp)
target_link_libraries(edgepulse-protocol-tests PRIVATE EdgePulse::App edgepulse-test-support GTest::gtest_main)
edgepulse_enable_warnings(edgepulse-protocol-tests)
set_target_properties(edgepulse-protocol-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
gtest_discover_tests(edgepulse-protocol-tests DISCOVERY_MODE PRE_TEST)
