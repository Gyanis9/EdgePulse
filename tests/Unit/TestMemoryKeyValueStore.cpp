#include "Core/Cache/MemoryKeyValueStore.h"
#include "TestSupport/EventLoopRunner.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>

// 钉住：写入后可读、删除后不可读
TEST(MemoryKeyValueStore, SetGetErase)
{
    auto                                    store = std::make_shared<EdgePulse::Cache::MemoryKeyValueStore>();
    EdgePulse::TestSupport::EventLoopRunner runner;

    EXPECT_TRUE(runner.run(store->set(runner.loop(), "k", "v", std::chrono::seconds{60})));
    EXPECT_EQ("v", runner.run(store->get(runner.loop(), "k")).value());
    EXPECT_TRUE(runner.run(store->erase(runner.loop(), "k")));
    EXPECT_FALSE(runner.run(store->get(runner.loop(), "k")).has_value());
}

// 钉住：读取并删除是原子的一次消费——首次取回值，第二次已没有该键
TEST(MemoryKeyValueStore, TakeReturnsValueOnce)
{
    EdgePulse::Cache::MemoryKeyValueStore   store;
    EdgePulse::TestSupport::EventLoopRunner runner;

    ASSERT_TRUE(runner.run(store.set(runner.loop(), "k", "v", std::chrono::seconds{60})));
    const auto first = runner.run(store.take(runner.loop(), "k"));
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ("v", *first);
    EXPECT_FALSE(runner.run(store.take(runner.loop(), "k")).has_value());
    EXPECT_FALSE(runner.run(store.get(runner.loop(), "k")).has_value());
}

// 钉住：不存在的键返回空值，而不是空串或抛异常
TEST(MemoryKeyValueStore, TakeReturnsEmptyForMissingKey)
{
    EdgePulse::Cache::MemoryKeyValueStore   store;
    EdgePulse::TestSupport::EventLoopRunner runner;

    EXPECT_FALSE(runner.run(store.take(runner.loop(), "absent")).has_value());
}

// 钉住：过期键不再交回值，且不残留在键表里
TEST(MemoryKeyValueStore, TakeReturnsEmptyForExpiredKey)
{
    auto                                    current = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
    EdgePulse::Cache::MemoryKeyValueStore   store([current] { return *current; });
    EdgePulse::TestSupport::EventLoopRunner runner;

    ASSERT_TRUE(runner.run(store.set(runner.loop(), "k", "v", std::chrono::seconds{10})));
    *current += std::chrono::seconds{11};
    EXPECT_FALSE(runner.run(store.take(runner.loop(), "k")).has_value());
    EXPECT_EQ(0U, store.size());
}

// 钉住：固定窗口自增——计数从 1 开始，窗口内不被每次自增续期，过期后重新从 1 开始
TEST(MemoryKeyValueStore, IncrementKeepsFixedWindow)
{
    auto                                    current = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
    auto                                    store   = std::make_shared<EdgePulse::Cache::MemoryKeyValueStore>([current] { return *current; });
    EdgePulse::TestSupport::EventLoopRunner runner;

    EXPECT_EQ(1, runner.run(store->increment(runner.loop(), "counter", std::chrono::seconds{10})));
    *current += std::chrono::seconds{5};
    EXPECT_EQ(2, runner.run(store->increment(runner.loop(), "counter", std::chrono::seconds{10})));
    *current += std::chrono::seconds{6};
    EXPECT_EQ(1, runner.run(store->increment(runner.loop(), "counter", std::chrono::seconds{10})));
}
