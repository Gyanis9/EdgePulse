add_library(edgepulse-auth STATIC
        AuthController.cpp
        AuthService.cpp
        CaptchaService.cpp
        LoginAudit.cpp
        SessionStore.cpp)
add_library(EdgePulse::Auth ALIAS edgepulse-auth)

target_include_directories(edgepulse-auth PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(edgepulse-auth
        PUBLIC EdgePulse::Common
               EdgePulse::System
               EdgePulse::Web
               AsynGyanis::Net
               AsynGyanis::Core
               AsynGyanis::Base
               OpenSSL::Crypto)
edgepulse_enable_warnings(edgepulse-auth)
