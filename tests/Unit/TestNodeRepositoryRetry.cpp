#include "Base/Database/SqlRetry.h"

#include <gtest/gtest.h>

// 钉住：并发写入只对锁冲突、死锁与唯一键冲突重试；其余数据库故障必须原样抛出
TEST(NodeRepositoryRetry, ClassifiesConcurrentFailures)
{
    using EdgePulse::Sql::isRetryableSqlFailure;
    using EdgePulse::Sql::isUniqueViolation;

    EXPECT_TRUE(isRetryableSqlFailure("执行 SQL 失败：database is locked"));
    EXPECT_TRUE(isRetryableSqlFailure("database table is busy"));
    EXPECT_TRUE(isRetryableSqlFailure("Deadlock found when trying to get lock"));
    EXPECT_TRUE(isRetryableSqlFailure("UNIQUE constraint failed: biz_node.name"));
    EXPECT_TRUE(isRetryableSqlFailure("Duplicate entry '20260101' for key 'uq_biz_node_name'"));

    EXPECT_FALSE(isRetryableSqlFailure("disk I/O error"));
    EXPECT_FALSE(isRetryableSqlFailure("no such table: biz_node"));
    EXPECT_FALSE(isRetryableSqlFailure(""));

    EXPECT_TRUE(isUniqueViolation("UNIQUE constraint failed: biz_node_config_revision.node_id, biz_node_config_revision.version"));
    EXPECT_TRUE(isUniqueViolation("Duplicate entry '20260101' for key 'uq_node_config_revision'"));
    EXPECT_FALSE(isUniqueViolation("database is locked"));
    EXPECT_FALSE(isUniqueViolation(""));
}
