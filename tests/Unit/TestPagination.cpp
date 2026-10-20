#include "Base/Api/Pagination.h"

#include <gtest/gtest.h>

#include <unordered_map>

using EdgePulse::Common::makePageResult;
using EdgePulse::Common::PageParameters;
using EdgePulse::Common::parsePageParameters;

// 钉住：查询串为空时给出契约默认值，OFFSET 从 0 开始
TEST(Pagination, EmptyQueryUsesDefaults)
{
    const PageParameters parameters = parsePageParameters({});
    EXPECT_EQ(1, parameters.pageNum);
    EXPECT_EQ(10, parameters.pageSize);
    EXPECT_EQ(0U, parameters.offset());
}

// 钉住：pageSize 上限 100，超出部分被钳制而不是原样下发
TEST(Pagination, SizeIsClampedToMaximum)
{
    const PageParameters parameters = parsePageParameters({{"pageNum", "3"}, {"pageSize", "200"}});
    EXPECT_EQ(3, parameters.pageNum);
    EXPECT_EQ(100, parameters.pageSize);
    EXPECT_EQ(200U, parameters.offset());
}

// 钉住：0、负数、尾随字符与空串都回落默认值，不发生部分解析
TEST(Pagination, InvalidValuesFallBackToDefaults)
{
    for (const auto &query: std::vector<std::unordered_map<std::string, std::string>>{
                 {{"pageNum", "0"}, {"pageSize", "0"}},
                 {{"pageNum", "-1"}, {"pageSize", "abc"}},
                 {{"pageNum", " 2"}, {"pageSize", "10x"}},
                 {{"pageNum", ""}, {"pageSize", ""}},
         })
    {
        const PageParameters parameters = parsePageParameters(query);
        EXPECT_EQ(1, parameters.pageNum);
        EXPECT_EQ(10, parameters.pageSize);
    }
}

// 钉住：分页载荷的四个字段名与类型与前端 PageResult 契约一致
TEST(Pagination, PageResultCarriesContractFields)
{
    const auto result = makePageResult(PageParameters{.pageNum = 2, .pageSize = 25}, 42, AsynGyanis::Base::ConfigValue::array({}));
    EXPECT_EQ(2, result["pageNum"].get<int>());
    EXPECT_EQ(25, result["pageSize"].get<int>());
    EXPECT_EQ(42, result["total"].get<int>());
    EXPECT_TRUE(result["rows"].is_array());
    EXPECT_TRUE(result["rows"].empty());
}
