/**
 * @file OutboundUrlGuard.h
 * @brief 出站 Webhook 地址的 SSRF 防护
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <Core/Coroutine/Task.h>

#include <chrono>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace AsynGyanis::Core
{
    class EventLoop;
}

namespace EdgePulse::Common
{
    /**
     * @brief 出站地址校验选项
     */
    struct OutboundUrlGuardOptions
    {
        bool                 allowPrivateNetworks{false}; ///< 是否放行普通 RFC1918 私网与回环地址；云元数据及链路本地地址始终拒绝
        std::chrono::seconds cacheTtl{60};                ///< 解析结果缓存时长；非正数表示不缓存
    };

    /**
     * @brief 把主机名解析成地址列表
     * @details 测试注入该函数即可绕开真实 DNS；生产路径在阻塞执行器上调用系统解析器。
     */
    using HostAddressResolver = std::function<std::vector<std::string>(std::string_view host)>;

    /**
     * @brief Webhook 出站地址的 SSRF 校验器
     * @details 地址按 RFC 3986 拆开后，对主机名做解析，任何一个解析结果落在私网、回环、
     *          组播、保留段或云元数据地址上就整条拒绝。localhost、*.localhost 与 *.local
     *          这类本机或局域网别名默认短路拒绝，allowPrivateNetworks 打开后才交给解析器。
     *          云元数据主机名、元数据地址以及 169.254.0.0/16、fe80::/10 两个链路本地段
     *          始终拒绝：链路本地正是元数据服务的传输范围，打开开关也不放行。
     * @note validate 在事件循环上调用，阻塞的 DNS 查询交给 AsyncExecutor；解析结果按
     *       cacheTtl 缓存，避免每次投递都查一次。
     */
    class OutboundUrlGuard
    {
    public:
        /**
         * @brief 构造校验器
         * @param options 放行策略与缓存时长
         * @param resolver 主机名解析器；为空时使用系统解析器
         */
        explicit OutboundUrlGuard(OutboundUrlGuardOptions options, HostAddressResolver resolver = {});

        /**
         * @brief 校验一条待出站的 Webhook 地址
         * @param loop 恢复协程用的事件循环
         * @param url 完整地址，必须是 http(s) 绝对地址且不带 userinfo
         * @return 允许出站时为空；拒绝时返回中文原因，写明命中的地址或主机
         */
        [[nodiscard]] AsynGyanis::Core::Task<std::expected<void, std::string>> validate(AsynGyanis::Core::EventLoop &loop, std::string_view url) const;

        /**
         * @brief 判断地址是否属于私网或保留段
         * @param address IPv4 或 IPv6 地址文本；允许带方括号与 IPv6 作用域后缀
         * @return true 表示不可作为公网 Webhook 目标；空文本或无法解析的文本同样返回 true（失败关闭）
         */
        [[nodiscard]] static bool isPrivateOrReservedAddress(std::string_view address);

        /**
         * @brief 判断地址是否指向云元数据服务或其链路本地传输段
         * @param address IPv4 或 IPv6 地址文本
         * @return true 表示该地址永远不可作为 Webhook 目标，即使放行了私网
         */
        [[nodiscard]] static bool isMetadataAddress(std::string_view address);

    private:
        /**
         * @brief 一次解析结果的缓存项
         */
        struct CacheEntry
        {
            std::vector<std::string>              addresses; ///< 解析出的地址
            std::chrono::steady_clock::time_point expiresAt; ///< 过期时刻
        };

        /**
         * @brief 解析主机名，命中缓存时直接返回
         * @param loop 恢复协程用的事件循环
         * @param host 待解析主机名
         * @return 地址列表；解析失败时为空
         */
        [[nodiscard]] AsynGyanis::Core::Task<std::vector<std::string>> resolve(AsynGyanis::Core::EventLoop &loop, std::string host) const;

        /**
         * @brief 读取未过期的缓存
         * @param host 已归一化的主机名
         * @return 命中时返回地址列表
         */
        [[nodiscard]] std::optional<std::vector<std::string>> cachedAddresses(const std::string &host) const;

        /**
         * @brief 写入解析结果；空结果不缓存，避免一次临时失败挡住后续重试
         */
        void rememberAddresses(const std::string &host, const std::vector<std::string> &addresses) const;

        OutboundUrlGuardOptions                             m_options;    ///< 放行策略与缓存时长
        HostAddressResolver                                 m_resolver;   ///< 主机名解析器
        mutable std::mutex                                  m_cacheMutex; ///< 保护解析缓存的互斥锁
        mutable std::unordered_map<std::string, CacheEntry> m_cache;      ///< 主机名到解析结果的缓存
    };
} // namespace EdgePulse::Common
