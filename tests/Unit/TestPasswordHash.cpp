#include "Common/PasswordHash.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace
{
    constexpr const char *kSeedPasswordHash = "pbkdf2_sha256$210000$nUwvGntuPFCBorPE1eb3CA==$cxCcCezVVbPzfukzjfcpWoTZpVyQ3AR1H9Z2V1QWObk=";
}

// 钉住：种子里写死的哈希能校验开发口令，口令错误时拒绝
TEST(PasswordHash, VerifiesSeedHashAndRejectsWrongPassword)
{
    EXPECT_TRUE(EdgePulse::Common::verifyPassword(kSeedPasswordHash, "admin123"));
    EXPECT_FALSE(EdgePulse::Common::verifyPassword(kSeedPasswordHash, "admin124"));
    EXPECT_FALSE(EdgePulse::Common::verifyPassword(kSeedPasswordHash, ""));
}

// 钉住：新哈希是四段格式且迭代数正确，生成后能原样校验
TEST(PasswordHash, FreshHashHasContractShapeAndRoundTrips)
{
    const std::string hash = EdgePulse::Common::hashPassword("s3cr3t!");
    EXPECT_EQ("pbkdf2_sha256", hash.substr(0, hash.find('$')));
    EXPECT_EQ(3U, static_cast<std::size_t>(std::count(hash.begin(), hash.end(), '$')));
    EXPECT_NE(std::string::npos, hash.find("$210000$"));
    EXPECT_TRUE(EdgePulse::Common::verifyPassword(hash, "s3cr3t!"));
    EXPECT_FALSE(EdgePulse::Common::verifyPassword(hash, "s3cr3t?"));
}

// 钉住：格式不合法、段数不足或 base64 坏掉都只返回 false，不抛异常
TEST(PasswordHash, RejectsMalformedStoredHash)
{
    EXPECT_FALSE(EdgePulse::Common::verifyPassword("", "admin123"));
    EXPECT_FALSE(EdgePulse::Common::verifyPassword("md5$1$a$b", "admin123"));
    EXPECT_FALSE(EdgePulse::Common::verifyPassword("pbkdf2_sha256$0$abc$def", "admin123"));
    EXPECT_FALSE(EdgePulse::Common::verifyPassword("pbkdf2_sha256$210000$not-base64$not-base64", "admin123"));
    EXPECT_FALSE(EdgePulse::Common::verifyPassword("pbkdf2_sha256$210000$nUwvGntuPFCBorPE1eb3CA==", "admin123"));
}
