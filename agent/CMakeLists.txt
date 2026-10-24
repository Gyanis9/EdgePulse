add_library(edgepulse-agent-core STATIC
        AgentApplication.cpp
        AgentClient.cpp
        AgentConfiguration.cpp
        AgentIdentity.cpp
        AgentState.cpp
        AgentWebSocket.cpp
        CommandDispatcher.cpp
        Executors/ConfigExecutor.cpp
        Executors/DiagnosticsExecutor.cpp
        Executors/FirmwareExecutor.cpp
        Executors/RestartExecutor.cpp
        Executors/TelemetryPublisher.cpp
        Executors/UpdateExecutor.cpp
        ProcessRunner.cpp
        TelemetryCollector.cpp)
target_include_directories(edgepulse-agent-core PUBLIC ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(edgepulse-agent-core PRIVATE EDGEPULSE_AGENT_VERSION="${PROJECT_VERSION}")
target_link_libraries(edgepulse-agent-core PUBLIC EdgePulse::Core AsynGyanis::Net AsynGyanis::Database AsynGyanis::Core AsynGyanis::Base OpenSSL::Crypto)
if(WIN32)
    target_sources(edgepulse-agent-core PRIVATE ServiceHost.cpp)
    target_link_libraries(edgepulse-agent-core PUBLIC advapi32)
endif()
edgepulse_enable_warnings(edgepulse-agent-core)

add_executable(edgepulse-agent Main.cpp)
target_link_libraries(edgepulse-agent PRIVATE edgepulse-agent-core)
target_compile_definitions(edgepulse-agent PRIVATE EDGEPULSE_AGENT_VERSION="${PROJECT_VERSION}")
edgepulse_enable_warnings(edgepulse-agent)
set_target_properties(edgepulse-agent PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
