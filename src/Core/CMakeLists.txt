add_library(edgepulse-core STATIC
        Auth/AuthService.cpp
        Auth/CaptchaService.cpp
        Auth/LoginAudit.cpp
        Auth/SessionStore.cpp
        Auth/DataScopeService.cpp
        Cache/MemoryKeyValueStore.cpp
        Cache/RedisKeyValueStore.cpp
        Alarm/AlarmRepository.cpp
        Alarm/AlarmService.cpp
        Command/AgentChannelRegistry.cpp
        Command/CommandRepository.cpp
        Device/AgentRegistry.cpp
        Device/NodeGroupRepository.cpp
        Device/NodeRepository.cpp
        Device/NodeService.cpp
        Firmware/FirmwareFileWriter.cpp
        Firmware/FirmwareRepository.cpp
        Http/AgentWebSocket.cpp
        Http/AlarmEventStream.cpp
        Http/AuditMiddleware.cpp
        Http/Authorization.cpp
        Http/MiddlewareSetup.cpp
        Http/ReverseProxy.cpp
        Http/RequestId.cpp
        Http/SseBroadcaster.cpp
        Http/WebSocketEndpoints.cpp
        Ops/OpsRepository.cpp
        System/DictionaryRepository.cpp
        System/LogRepository.cpp
        System/MenuRepository.cpp
        System/MigrationRunner.cpp
        System/OrgRepository.cpp
        System/RoleRepository.cpp
        System/UserRepository.cpp
        Telemetry/TelemetryIngest.cpp
        Telemetry/TelemetryRepository.cpp
        Telemetry/UdpTelemetryEndpoint.cpp
        Telemetry/UdpTelemetryProtocol.cpp)
add_library(EdgePulse::Core ALIAS edgepulse-core)

target_include_directories(edgepulse-core PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(edgepulse-core
        PUBLIC EdgePulse::Base
               AsynGyanis::Net
               AsynGyanis::Database
               AsynGyanis::Core
               AsynGyanis::Base
               OpenSSL::Crypto)
edgepulse_enable_warnings(edgepulse-core)
