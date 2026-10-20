add_library(edgepulse-system STATIC
        MenuRepository.cpp
        MigrationRunner.cpp
        UserRepository.cpp)
add_library(EdgePulse::System ALIAS edgepulse-system)

target_include_directories(edgepulse-system PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(edgepulse-system
        PUBLIC EdgePulse::Common
               AsynGyanis::Database
               AsynGyanis::Core
               AsynGyanis::Base)
edgepulse_enable_warnings(edgepulse-system)
