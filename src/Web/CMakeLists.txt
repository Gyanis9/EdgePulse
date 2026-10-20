add_library(edgepulse-web STATIC
        Authorization.cpp
        MiddlewareSetup.cpp
        RequestId.cpp)
add_library(EdgePulse::Web ALIAS edgepulse-web)

target_include_directories(edgepulse-web PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(edgepulse-web
        PUBLIC EdgePulse::Common
               AsynGyanis::Net
               AsynGyanis::Base
               AsynGyanis::Core)
edgepulse_enable_warnings(edgepulse-web)
