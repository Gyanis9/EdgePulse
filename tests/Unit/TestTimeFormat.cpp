#include "Base/Runtime/TimeFormat.h"

#include <gtest/gtest.h>

#include <string>

// 钉住：输出是 20 字符的 RFC 3339 UTC 形状，前端可直接展示
TEST(TimeFormat, UtcTextHasRfc3339Shape)
{
    const std::string text = EdgePulse::Common::nowIso8601Utc();
    ASSERT_EQ(20U, text.size());
    EXPECT_EQ('-', text[4]);
    EXPECT_EQ('-', text[7]);
    EXPECT_EQ('T', text[10]);
    EXPECT_EQ(':', text[13]);
    EXPECT_EQ(':', text[16]);
    EXPECT_EQ('Z', text[19]);
}

// 钉住：秒级时间戳按 UTC 换算，不受本机时区影响
TEST(TimeFormat, KnownEpochFormatsAsExpected)
{
    const std::chrono::system_clock::time_point epoch{std::chrono::seconds{0}};
    EXPECT_EQ("1970-01-01T00:00:00Z", EdgePulse::Common::formatUtc(epoch));
}
