#include "Common/EdgePulseException.h"
#include "System/MigrationRunner.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

// 钉住：分号只在引号与注释之外才切分
TEST(MigrationSqlSplitter, SplitsStatementsOutsideQuotes)
{
    const std::vector<std::string> statements = EdgePulse::System::splitSqlStatements("CREATE TABLE a (id INT); INSERT INTO a VALUES ('x;y'); -- 行注释\nUPDATE a SET id = 1;");
    ASSERT_EQ(3U, statements.size());
    EXPECT_EQ("CREATE TABLE a (id INT)", statements[0]);
    EXPECT_EQ("INSERT INTO a VALUES ('x;y')", statements[1]);
    EXPECT_EQ("UPDATE a SET id = 1", statements[2]);
}

// 钉住：行注释与块注释不会进入语句文本
TEST(MigrationSqlSplitter, DropsComments)
{
    const std::vector<std::string> statements = EdgePulse::System::splitSqlStatements("-- 开头注释\nSELECT 1; /* 中间\n注释 */ SELECT 2;");
    ASSERT_EQ(2U, statements.size());
    EXPECT_EQ("SELECT 1", statements[0]);
    EXPECT_EQ("SELECT 2", statements[1]);
}

// 钉住：双引号标识符里的分号同样不切分
TEST(MigrationSqlSplitter, KeepsSemicolonInQuotedIdentifier)
{
    const std::vector<std::string> statements = EdgePulse::System::splitSqlStatements("CREATE TABLE \"a;b\" (id INT);");
    ASSERT_EQ(1U, statements.size());
    EXPECT_EQ("CREATE TABLE \"a;b\" (id INT)", statements[0]);
}

// 钉住：未闭合的字符串或块注释在交给数据库之前就被拒绝
TEST(MigrationSqlSplitter, RejectsUnclosedQuoteOrBlockComment)
{
    EXPECT_THROW(static_cast<void>(EdgePulse::System::splitSqlStatements("SELECT 'unterminated;")), EdgePulse::Common::MigrationException);
    EXPECT_THROW(static_cast<void>(EdgePulse::System::splitSqlStatements("SELECT 1; /* open")), EdgePulse::Common::MigrationException);
}
