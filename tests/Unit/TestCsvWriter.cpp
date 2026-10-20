#include "Base/Api/CsvWriter.h"

#include <gtest/gtest.h>

#include <string>

// 钉住：BOM、CRLF、逗号/引号/换行的 RFC 4180 转义；中文原样保留
TEST(CsvWriter, EscapesCellsAndPrependsBom)
{
    EdgePulse::Common::CsvWriter writer;
    writer.addRow({"名称", "含,逗号", "含\"引号", "含\n换行"});
    const std::string text = writer.take();
    ASSERT_GE(text.size(), 3U);
    EXPECT_EQ("\xEF\xBB\xBF", text.substr(0, 3));
    EXPECT_NE(std::string::npos, text.find("名称,\"含,逗号\",\"含\"\"引号\",\"含\n换行\"\r\n"));
}

// 钉住：普通单元格不加引号；空行保留 CRLF
TEST(CsvWriter, LeavesPlainCellsUnquoted)
{
    EdgePulse::Common::CsvWriter writer;
    writer.addRow({"a", "b"});
    writer.addRow({});
    EXPECT_EQ("\xEF\xBB\xBF"
              "a,b\r\n\r\n",
              writer.take());
}
