#include "Auth/SessionStore.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>

namespace
{
    EdgePulse::Auth::SessionRecord makeSession()
    {
        EdgePulse::Auth::SessionRecord record;
        record.userId      = 7;
        record.username    = "tester";
        record.nickname    = "测试员";
        record.permissions = {"device:node:list"};
        return record;
    }
} // namespace

// 钉住：滑动过期——只要在有效期内访问，就从当前时刻续期；超过 TTL 不访问则失效
TEST(SessionStore, SlidesExpiryOnAccess)
{
    auto                          current = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
    EdgePulse::Auth::SessionStore store(100, [current] { return *current; });

    const EdgePulse::Auth::SessionRecord session = store.create(makeSession());
    EXPECT_EQ(32U * 2U, session.token.size());
    EXPECT_EQ(1U, store.size());

    *current += std::chrono::seconds{90};
    const auto firstTouch = store.validateAndTouch(session.token);
    ASSERT_TRUE(firstTouch.has_value());
    EXPECT_EQ(7, firstTouch->userId);

    *current += std::chrono::seconds{90};
    EXPECT_TRUE(store.validateAndTouch(session.token).has_value());

    *current += std::chrono::seconds{101};
    EXPECT_FALSE(store.validateAndTouch(session.token).has_value());
}

// 钉住：登出立即删除，未知令牌不产生记录
TEST(SessionStore, RemoveInvalidatesImmediately)
{
    EdgePulse::Auth::SessionStore        store(100);
    const EdgePulse::Auth::SessionRecord session = store.create(makeSession());
    store.remove(session.token);
    EXPECT_FALSE(store.validateAndTouch(session.token).has_value());
    EXPECT_FALSE(store.validateAndTouch("not-a-token").has_value());
    EXPECT_EQ(0U, store.size());
}
