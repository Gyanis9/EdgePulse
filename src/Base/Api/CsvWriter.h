/**
 * @file CsvWriter.h
 * @brief UTF-8 BOM 的 CSV 组装与转义
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace EdgePulse::Common
{
    /**
     * @brief 逐行拼装 CSV 文本
     * @details 单元格按 RFC 4180 加引号转义；输出以 UTF-8 BOM 开头，
     *          保证 Excel 在中文 Windows 上按 UTF-8 打开而不是乱码。
     */
    class CsvWriter
    {
    public:
        /**
         * @brief 追加一行
         * @param cells 已转成文本的单元格
         */
        void addRow(const std::vector<std::string> &cells);

        /**
         * @brief 取完整文本
         * @return std::string BOM + 所有行，行尾 CRLF
         */
        [[nodiscard]] std::string take() const;

        /**
         * @brief 单元格转义：含逗号、引号、换行时整体加引号并把引号翻倍
         * @param value 原始文本
         * @return std::string 转义后的单元格
         */
        [[nodiscard]] static std::string escape(std::string_view value);

    private:
        std::string m_text; ///< 已拼好的正文（不含 BOM）
    };
} // namespace EdgePulse::Common
