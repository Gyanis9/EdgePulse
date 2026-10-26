add_library(edgepulse-core STATIC
        Auth/ApiTokenRepository.cpp
        Auth/ApiTokenService.cpp
        Auth/AuthService.cpp
        Auth/CaptchaService.cpp
        Auth/LoginAudit.cpp
        Auth/MfaCrypto.cpp
        Auth/MfaRepository.cpp
        Auth/MfaService.cpp
        Auth/SessionStore.cpp
        Auth/Totp.cpp
        Auth/DataScopeService.cpp
        Auth/Jwt.cpp
        Auth/OidcService.cpp
        Cache/MemoryKeyValueStore.cpp
        Cache/RedisKeyValueStore.cpp
        Cache/RedisProbe.cpp
        Alarm/AlarmRepository.cpp
        Alarm/AlarmService.cpp
        Command/AgentChannelRegistry.cpp
        Command/AgentCommandDelivery.cpp
        Command/CommandPublishThrottle.cpp
        Command/CommandRepository.cpp
        Device/AgentAuthorization.cpp
        Device/AgentCertificateAuthority.cpp
        Device/AgentCertificateRepository.cpp
        Device/AgentRegistry.cpp
        Device/CertificateBinding.cpp
        Device/EnrollmentRepository.cpp
        Device/EnrollmentService.cpp
        Device/NodeConfigRepository.cpp
        Device/NodeConfigService.cpp
        Device/NodeGroupRepository.cpp
        Device/NodeLifecycleService.cpp
        Device/NodeRepository.cpp
        Device/NodeService.cpp
        Firmware/FirmwareFileWriter.cpp
        Firmware/FirmwareRepository.cpp
        Firmware/FirmwareRolloutPolicy.cpp
        Firmware/FirmwareRolloutRepository.cpp
        Firmware/FirmwareRolloutService.cpp
        Firmware/FirmwareSignatureVerifier.cpp
        Http/AgentWebSocket.cpp
        Http/AlarmEventStream.cpp
        Http/AuditMiddleware.cpp
        Http/Authorization.cpp
        Http/IdempotencyMiddleware.cpp
        Http/IdempotencyRepository.cpp
        Http/MiddlewareSetup.cpp
        Http/RealtimeBus.cpp
        Http/PublisherHealth.cpp
        Http/ReadinessState.cpp
        Http/ReverseProxy.cpp
        Http/RequestId.cpp
        Http/SseBroadcaster.cpp
        Http/WebSocketEndpoints.cpp
        Metrics/BusinessMetrics.cpp
        Notification/NotificationDispatcher.cpp
        Notification/NotificationRepository.cpp
        Notification/NotificationService.cpp
        Notification/SmtpMailer.cpp
        Ops/OpsRepository.cpp
        System/DictionaryRepository.cpp
        System/LeaderGate.cpp
        System/LeaderTick.cpp
        System/LeaderLease.cpp
        System/LogRepository.cpp
        System/MenuRepository.cpp
        System/MigrationLock.cpp
        System/MigrationRunner.cpp
        System/OrgRepository.cpp
        System/RoleRepository.cpp
        System/TenantRepository.cpp
        System/UserRepository.cpp
        Telemetry/TelemetryIngest.cpp
        Telemetry/TelemetryRepository.cpp
        Telemetry/UdpTelemetryEndpoint.cpp
        Telemetry/UdpTelemetryProtocol.cpp)
add_library(EdgePulse::Core ALIAS edgepulse-core)

# GCC 14 在 LeaderLease 析构内联引擎协程句柄析构时误报 maybe-uninitialized；
# 只对这个实现文件关闭该告警，其余文件保持开启
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 14)
    set_source_files_properties(System/LeaderLease.cpp PROPERTIES COMPILE_OPTIONS "-Wno-maybe-uninitialized")
endif()

target_include_directories(edgepulse-core PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(edgepulse-core
        PUBLIC EdgePulse::Base
               AsynGyanis::Net
               AsynGyanis::Database
               AsynGyanis::Core
               AsynGyanis::Base
               OpenSSL::Crypto)
edgepulse_enable_warnings(edgepulse-core)
