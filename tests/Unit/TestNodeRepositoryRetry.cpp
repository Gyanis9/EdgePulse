#include "Core/Device/NodeRepository.h"

#include <gtest/gtest.h>

// 钉住：并发注册只对锁冲突、死锁与唯一键冲突重试；其余数据库故障必须原样抛出
TEST(NodeRepositoryRetry, ClassifiesConcurrentFailures)
{
    using EdgePulse::Device::isRetryableRegistrationFailure;

    EXPECT_TRUE(isRetryableRegistrationFailure("执行 SQL 失败：database is locked"));
    EXPECT_TRUE(isRetryableRegistrationFailure("database table is busy"));
    EXPECT_TRUE(isRetryableRegistrationFailure("Deadlock found when trying to get lock"));
    EXPECT_TRUE(isRetryableRegistrationFailure("UNIQUE constraint failed: biz_node.name"));
    EXPECT_TRUE(isRetryableRegistrationFailure("Duplicate entry '20260101' for key 'uq_biz_node_name'"));

    EXPECT_FALSE(isRetryableRegistrationFailure("disk I/O error"));
    EXPECT_FALSE(isRetryableRegistrationFailure("no such table: biz_node"));
    EXPECT_FALSE(isRetryableRegistrationFailure(""));
}
