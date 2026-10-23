add_library(edgepulse-base STATIC
        Api/ApiResponse.cpp
        Api/CsvWriter.cpp
        Api/Pagination.cpp
        Database/Database.cpp
        Database/SqlRetry.cpp
        Runtime/Environment.cpp
        Runtime/IdGenerator.cpp
        Runtime/TimeFormat.cpp
        Security/Hmac.cpp
        Security/PasswordHash.cpp
        Security/RandomBytes.cpp
        Security/RandomToken.cpp
        Security/OutboundUrlGuard.cpp)
add_library(EdgePulse::Base ALIAS edgepulse-base)

target_include_directories(edgepulse-base PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(edgepulse-base
        PUBLIC AsynGyanis::Database
               AsynGyanis::Core
               AsynGyanis::Base
               OpenSSL::Crypto
        PRIVATE AsynGyanis::Net)
edgepulse_enable_warnings(edgepulse-base)
