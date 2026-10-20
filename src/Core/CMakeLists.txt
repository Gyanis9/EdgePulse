add_library(edgepulse-core STATIC
        Auth/AuthService.cpp
        Auth/CaptchaService.cpp
        Auth/LoginAudit.cpp
        Auth/SessionStore.cpp
        Device/AgentRegistry.cpp
        Device/NodeRepository.cpp
        Device/NodeService.cpp
        Http/Authorization.cpp
        Http/MiddlewareSetup.cpp
        Http/RequestId.cpp
        Http/WebSocketEndpoints.cpp
        System/MenuRepository.cpp
        System/MigrationRunner.cpp
        System/UserRepository.cpp)
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
