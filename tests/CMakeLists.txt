find_package(GTest REQUIRED)
include(GoogleTest)

add_library(edgepulse-test-support STATIC TestSupport/HttpClientHarness.cpp)
target_include_directories(edgepulse-test-support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(edgepulse-test-support
        PUBLIC EdgePulse::Common
               EdgePulse::Web
               AsynGyanis::Net
               AsynGyanis::Base
               AsynGyanis::Core
               GTest::gtest)
edgepulse_enable_warnings(edgepulse-test-support)

add_executable(edgepulse-unit-tests
        Unit/TestMigrationSqlSplitter.cpp
        Unit/TestPagination.cpp
        Unit/TestTimeFormat.cpp)
target_link_libraries(edgepulse-unit-tests PRIVATE EdgePulse::Common EdgePulse::System GTest::gtest_main)
edgepulse_enable_warnings(edgepulse-unit-tests)
set_target_properties(edgepulse-unit-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
gtest_discover_tests(edgepulse-unit-tests DISCOVERY_MODE PRE_TEST)

add_executable(edgepulse-integration-tests
        Integration/TestMigrations.cpp)
target_include_directories(edgepulse-integration-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(edgepulse-integration-tests PRIVATE EDGEPULSE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(edgepulse-integration-tests PRIVATE EdgePulse::System EdgePulse::Common GTest::gtest_main)
edgepulse_enable_warnings(edgepulse-integration-tests)
set_target_properties(edgepulse-integration-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
gtest_discover_tests(edgepulse-integration-tests DISCOVERY_MODE PRE_TEST)

add_executable(edgepulse-protocol-tests
        Protocol/TestApiResponse.cpp
        Protocol/TestHealthServer.cpp)
target_link_libraries(edgepulse-protocol-tests PRIVATE EdgePulse::App edgepulse-test-support GTest::gtest_main)
edgepulse_enable_warnings(edgepulse-protocol-tests)
set_target_properties(edgepulse-protocol-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
gtest_discover_tests(edgepulse-protocol-tests DISCOVERY_MODE PRE_TEST)
