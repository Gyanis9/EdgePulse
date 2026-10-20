add_library(edgepulse-app STATIC
        Application.cpp
        ApplicationConfiguration.cpp
        ListenerAssembly.cpp)
add_library(EdgePulse::App ALIAS edgepulse-app)

target_include_directories(edgepulse-app PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(edgepulse-app
        PUBLIC EdgePulse::Plugins
               EdgePulse::Core
               EdgePulse::Base
               AsynGyanis::Net
               AsynGyanis::Database
               AsynGyanis::Core
               AsynGyanis::Base)
edgepulse_enable_warnings(edgepulse-app)
