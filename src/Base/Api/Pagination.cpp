#include "Base/Api/Pagination.h"

#include <algorithm>
#include <charconv>
#include <optional>
#include <string_view>
#include <utility>

namespace EdgePulse::Common
{
    namespace
    {
        /**
         * @brief 严格读取一个十进制正整数：空串、负号、尾随字符与溢出一律判非法
         */
        [[nodiscard]] std::optional<std::int64_t> parsePositiveInteger(const std::string_view text)
        {
            if (text.empty())
            {
                return std::nullopt;
            }
            std::int64_t value      = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size() || value <= 0)
            {
                return std::nullopt;
            }
            return value;
        }
    } // namespace

    std::size_t PageParameters::offset() const noexcept
    {
        // pageNum 已在解析时钳到 1 以上，这里的减一不会下溢
        return static_cast<std::size_t>(pageNum - 1) * static_cast<std::size_t>(pageSize);
    }

    PageParameters parsePageParameters(const std::unordered_map<std::string, std::string> &query)
    {
        PageParameters parameters;
        // 非法页码回落 1：前端漏传或手改查询串时仍返回第一页，而不是把 OFFSET 算成负数
        if (const auto pageNum = query.find("pageNum"); pageNum != query.end())
        {
            if (const auto parsed = parsePositiveInteger(pageNum->second); parsed.has_value())
            {
                // 页码只影响 OFFSET，仍给它一个上界，避免乘法把 size_t 撑爆
                parameters.pageNum = static_cast<int>(std::min<std::int64_t>(*parsed, 1'000'000));
            }
        }
        // pageSize 上限是契约的一部分：超过 100 的请求按 100 返回，避免一次拉走整表
        if (const auto pageSize = query.find("pageSize"); pageSize != query.end())
        {
            if (const auto parsed = parsePositiveInteger(pageSize->second); parsed.has_value())
            {
                parameters.pageSize = static_cast<int>(std::min<std::int64_t>(*parsed, kMaximumPageSize));
            }
        }
        return parameters;
    }

    AsynGyanis::Base::ConfigValue makePageResult(const PageParameters &parameters, const std::int64_t total, AsynGyanis::Base::ConfigValue rows)
    {
        // 字段名与 scaffold 的 PageResult 一致，前端分页组件直接消费
        return AsynGyanis::Base::ConfigValue::object({{"pageNum", parameters.pageNum}, {"pageSize", parameters.pageSize}, {"total", total}, {"rows", std::move(rows)}});
    }
} // namespace EdgePulse::Common
