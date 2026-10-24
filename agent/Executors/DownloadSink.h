/**
 * @file DownloadSink.h
 * @brief 流式下载落盘与 SHA-256 收尾
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Platform/FileSystem/FileSystem.h"

#include <openssl/evp.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace EdgePulse::Agent
{
    /**
     * @brief 下载正文的落盘与摘要状态
     * @details 每次写入都由调用方经阻塞执行器调度，因此文件流与 EVP 上下文只会被一个工作线程
     *          依次碰到；接收口只负责把批次交进来，不在事件循环上做文件读写或摘要计算。
     *          预期字节数为 0 表示不限长度，适用于命令没有给出大小的升级包。
     */
    struct DownloadSink
    {
        std::filesystem::path path;                    ///< 暂存文件路径
        std::uint64_t         expectedBytes{0};        ///< 命令给出的字节数；0 表示不限
        std::string           purpose{"文件"};          ///< 写进中文原因的对象名，如固件、升级包
        std::ofstream         file;                    ///< 暂存文件，首次写入时打开
        EVP_MD_CTX           *digest{nullptr};         ///< SHA-256 上下文
        std::uint64_t         receivedBytes{0};        ///< 已写入的字节数
        bool                  opened{false};           ///< 暂存文件是否已打开
        bool                  failed{false};           ///< 是否已判定失败
        std::string           failure;                 ///< 中文失败原因

        DownloadSink() = default;

        ~DownloadSink()
        {
            if (digest != nullptr)
            {
                EVP_MD_CTX_free(digest);
            }
        }

        DownloadSink(const DownloadSink &)            = delete;
        DownloadSink &operator=(const DownloadSink &) = delete;

        /**
         * @brief 创建暂存目录、打开文件并初始化摘要
         * @return true 表示已就绪
         */
        bool open()
        {
            std::error_code filesystemError;
            std::filesystem::create_directories(path.parent_path(), filesystemError);
            if (filesystemError)
            {
                failure = "无法创建" + purpose + "暂存目录 " + AsynGyanis::Platform::FileSystem::utf8FromPath(path.parent_path()) + "：" + filesystemError.message();
                return false;
            }
            file.open(path, std::ios::binary | std::ios::trunc);
            if (!file.is_open())
            {
                failure = "无法创建" + purpose + "暂存文件 " + AsynGyanis::Platform::FileSystem::utf8FromPath(path);
                return false;
            }
            digest = EVP_MD_CTX_new();
            if (digest == nullptr || EVP_DigestInit_ex(digest, EVP_sha256(), nullptr) != 1)
            {
                failure = "无法初始化" + purpose + " SHA-256 计算";
                return false;
            }
            opened = true;
            return true;
        }

        /**
         * @brief 关闭暂存文件
         * @return true 表示文件已关闭或本来就没打开
         * @note Windows 上删除仍被打开的文件会失败，任何结局在删除前都要先关闭
         */
        bool close()
        {
            if (!file.is_open())
            {
                return true;
            }
            file.close();
            if (!file)
            {
                failed = true;
                if (failure.empty())
                {
                    failure = "关闭" + purpose + "暂存文件失败";
                }
                return false;
            }
            return true;
        }

        /**
         * @brief 写入一批正文并更新摘要
         * @param batch 本批字节
         * @return true 表示已完整写入；失败时 failed 与 failure 已置位
         */
        bool append(const std::string_view batch)
        {
            if (failed)
            {
                return false;
            }
            if (!opened && !open())
            {
                failed = true;
                return false;
            }
            if (batch.empty())
            {
                return true;
            }
            if (expectedBytes != 0 && receivedBytes + batch.size() > expectedBytes)
            {
                failed  = true;
                failure = purpose + "正文超过命令给出的 " + std::to_string(expectedBytes) + " 字节，已中止下载";
                return false;
            }
            file.write(batch.data(), static_cast<std::streamsize>(batch.size()));
            if (!file)
            {
                failed  = true;
                failure = "写入" + purpose + "暂存文件失败，可用磁盘空间可能不足";
                return false;
            }
            if (EVP_DigestUpdate(digest, batch.data(), batch.size()) != 1)
            {
                failed  = true;
                failure = "更新" + purpose + " SHA-256 失败";
                return false;
            }
            receivedBytes += batch.size();
            return true;
        }

        /**
         * @brief 收尾文件与摘要
         * @return 成功返回已收字节数与十六进制摘要；失败返回中文原因
         */
        std::expected<std::pair<std::uint64_t, std::string>, std::string> finish()
        {
            const bool closed = close();
            if (failed)
            {
                return std::unexpected(failure);
            }
            if (!closed)
            {
                return std::unexpected(failure);
            }
            if (!opened)
            {
                // 零字节正文不会触发写入，摘要仍要按空文件算完
                if (!open() || !close())
                {
                    return std::unexpected(failure);
                }
            }
            std::array<unsigned char, EVP_MAX_MD_SIZE> digestValue{};
            unsigned int                               digestLength = 0;
            if (EVP_DigestFinal_ex(digest, digestValue.data(), &digestLength) != 1 || digestLength != 32)
            {
                return std::unexpected("计算" + purpose + " SHA-256 失败");
            }
            static constexpr char kHexDigits[] = "0123456789abcdef";
            std::string           text;
            text.reserve(64);
            for (std::size_t index = 0; index < 32; ++index)
            {
                text.push_back(kHexDigits[digestValue[index] >> 4]);
                text.push_back(kHexDigits[digestValue[index] & 0x0F]);
            }
            return std::pair<std::uint64_t, std::string>{receivedBytes, text};
        }
    };
} // namespace EdgePulse::Agent
