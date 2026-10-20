/**
 * @file Pagination.h
 * @brief 分页参数归一化与分页载荷构造
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "Base/Config/ConfigValue.h"

namespace EdgePulse::Common
{
    inline constexpr int kDefaultPageSize = 10;
    inline constexpr int kMaximumPageSize = 100;

    /**
     * @brief 一页查询结果
     * @tparam Row 行类型
     */
    template<typename Row>
    struct PageRows
    {
        std::int64_t     total{0}; ///< 满足条件的总行数
        std::vector<Row> rows;     ///< 当前页行
    };

    /**
     * @brief 已归一化的分页参数
     */
    struct PageParameters
    {
        int pageNum{1};   ///< 页码，从 1 开始
        int pageSize{10}; ///< 每页条数，上限 100

        /**
         * @brief SQL OFFSET 的取值
         * @return std::size_t (pageNum - 1) * pageSize
         */
        [[nodiscard]] std::size_t offset() const noexcept;
    };

    /**
     * @brief 从查询串里读分页参数，非法值回落默认值并把 pageSize 钳到 1..100
     * @param query 已解码的查询参数
     * @return PageParameters 归一化结果
     */
    [[nodiscard]] PageParameters parsePageParameters(const std::unordered_map<std::string, std::string> &query);

    /**
     * @brief 构造 scaffold 契约的分页载荷
     * @param parameters 已归一化的分页参数
     * @param total 总行数
     * @param rows 当前页行数组
     * @return AsynGyanis::Base::ConfigValue {pageNum,pageSize,total,rows}
     */
    [[nodiscard]] AsynGyanis::Base::ConfigValue makePageResult(const PageParameters &parameters, std::int64_t total, AsynGyanis::Base::ConfigValue rows);
} // namespace EdgePulse::Common
