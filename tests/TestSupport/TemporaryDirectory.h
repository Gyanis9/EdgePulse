/**
 * @file TemporaryDirectory.h
 * @brief 测试用临时目录：创建独立目录、写入文本文件并在析构时递归清理
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <string>
#include <system_error>

namespace EdgePulse::TestSupport
{
    /**
     * @brief 自持生命周期的临时目录
     * @details 构造时在系统临时目录下建一个带时间戳与序号的名字，析构时递归删除；
     *          写入失败直接抛出，避免用例在缺文件的情况下给出与被测逻辑无关的失败。
     */
    class TemporaryDirectory
    {
    public:
        /**
         * @brief 创建临时目录
         * @throws std::runtime_error 系统临时目录不可用或目录创建失败
         */
        TemporaryDirectory()
        {
            std::error_code error;
            const std::filesystem::path temporaryRoot = std::filesystem::temp_directory_path(error);
            if (error)
            {
                throw std::runtime_error("无法定位系统临时目录：" + error.message());
            }
            static std::atomic<std::uint64_t> sequence{0};
            const std::string suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                                       std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
            m_path = temporaryRoot / ("edgepulse-agent-test-" + suffix);
            std::filesystem::create_directories(m_path, error);
            if (error)
            {
                throw std::runtime_error("无法创建测试临时目录：" + m_path.string() + "（" + error.message() + "）");
            }
        }

        /**
         * @brief 递归删除临时目录及其内容
         */
        ~TemporaryDirectory()
        {
            std::error_code error;
            std::filesystem::remove_all(m_path, error);
        }

        TemporaryDirectory(const TemporaryDirectory &) = delete;

        TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

        TemporaryDirectory(TemporaryDirectory &&) = delete;

        TemporaryDirectory &operator=(TemporaryDirectory &&) = delete;

        /**
         * @brief 取临时目录路径
         * @return const std::filesystem::path & 当前用例独占的目录
         */
        [[nodiscard]] const std::filesystem::path &path() const noexcept
        {
            return m_path;
        }

        /**
         * @brief 在临时目录下写入文本文件，缺少的父目录一并创建
         * @param name 相对临时目录的文件名，可含子目录
         * @param contents 完整文件内容
         * @return std::filesystem::path 写入后的绝对路径
         * @throws std::runtime_error 父目录或文件无法创建
         */
        [[nodiscard]] std::filesystem::path write(const std::filesystem::path &name, const std::string &contents) const
        {
            const std::filesystem::path filePath = m_path / name;
            if (filePath.has_parent_path())
            {
                std::error_code error;
                std::filesystem::create_directories(filePath.parent_path(), error);
                if (error)
                {
                    throw std::runtime_error("无法创建测试文件目录：" + filePath.parent_path().string() + "（" + error.message() + "）");
                }
            }

            std::ofstream file(filePath, std::ios::binary | std::ios::trunc);
            if (!file.is_open())
            {
                throw std::runtime_error("无法写入测试文件：" + filePath.string());
            }
            file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            file.close();
            if (!file)
            {
                throw std::runtime_error("测试文件写入未完整落盘：" + filePath.string());
            }
            return filePath;
        }

    private:
        std::filesystem::path m_path; ///< 本次用例独占的临时目录
    };
} // namespace EdgePulse::TestSupport
