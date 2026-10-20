add_library(edgepulse-common STATIC
        ApiResponse.cpp
        Database.cpp
        Pagination.cpp
        TimeFormat.cpp)
add_library(EdgePulse::Common ALIAS edgepulse-common)

target_include_directories(edgepulse-common PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(edgepulse-common
        PUBLIC AsynGyanis::Net
               AsynGyanis::Base
               AsynGyanis::Core
               AsynGyanis::Database
               OpenSSL::Crypto)
edgepulse_enable_warnings(edgepulse-common)
