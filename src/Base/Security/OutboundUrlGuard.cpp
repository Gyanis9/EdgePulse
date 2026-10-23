#include "Base/Security/OutboundUrlGuard.h"

#include "Net/Http/Client/HttpClient.h"
#include "Platform/IO/Socket.h"

#include <Core/Coroutine/AsyncExecutor.h>
#include <Core/EventLoop/EventLoop.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#endif

namespace EdgePulse::Common
{
    namespace
    {
        enum class AddressFamily
        {
            None,
            IPv4,
            IPv6
        };

        /**
         * @brief 去掉主机地址两侧的方括号与 IPv6 作用域后缀
         * @param address 原始地址文本
         * @return 只保留可交给 inet_pton 的部分
         * @note getaddrinfo 会把链路本地地址写成 fe80::1%12 的形式，作用域不参与地址段判断
         */
        [[nodiscard]] std::string_view addressText(const std::string_view address)
        {
            std::string_view text = address;
            if (text.size() >= 2 && text.front() == '[' && text.back() == ']')
            {
                text.remove_prefix(1);
                text.remove_suffix(1);
            }
            if (const std::size_t scope = text.find('%'); scope != std::string_view::npos)
            {
                text = text.substr(0, scope);
            }
            return text;
        }

        /**
         * @brief 判断一段文本是否为 IP 字面量
         */
        [[nodiscard]] AddressFamily classifyLiteral(const std::string_view address)
        {
            const std::string_view text = addressText(address);
            if (text.empty())
            {
                return AddressFamily::None;
            }
            std::array<std::uint8_t, 16> bytes{};
            const std::string            terminated{text};
            if (inet_pton(AF_INET, terminated.c_str(), bytes.data()) == 1)
            {
                return AddressFamily::IPv4;
            }
            if (inet_pton(AF_INET6, terminated.c_str(), bytes.data()) == 1)
            {
                return AddressFamily::IPv6;
            }
            return AddressFamily::None;
        }

        /**
         * @brief 按前缀长度判断 IPv4 是否落在某个地址段
         */
        [[nodiscard]] bool inIpv4Range(const std::uint32_t value, const std::uint32_t prefix, const int bits)
        {
            const std::uint32_t mask = bits == 0 ? 0U : 0xFFFFFFFFU << (32 - bits);
            return (value & mask) == (prefix & mask);
        }

        /**
         * @brief 判定 4 字节 IPv4 地址是否属于私网或保留段
         */
        [[nodiscard]] bool isPrivateIpv4(const std::array<std::uint8_t, 16> &bytes)
        {
            const std::uint32_t value = (static_cast<std::uint32_t>(bytes[0]) << 24U) | (static_cast<std::uint32_t>(bytes[1]) << 16U) |
                                        (static_cast<std::uint32_t>(bytes[2]) << 8U) | static_cast<std::uint32_t>(bytes[3]);
            // 0/8、10/8、127/8 与 172.16/12 是回环与私网；169.254/16 是链路本地
            if (inIpv4Range(value, 0x00000000U, 8) || inIpv4Range(value, 0x0A000000U, 8) || inIpv4Range(value, 0x7F000000U, 8) || inIpv4Range(value, 0xAC100000U, 12) ||
                inIpv4Range(value, 0xA9FE0000U, 16))
            {
                return true;
            }
            // 100.64/10 是运营商级 NAT，内网服务常落在这里，同样不能作为公网回拨目标
            if (inIpv4Range(value, 0x64400000U, 10))
            {
                return true;
            }
            // 192.0.0/24、192.0.2/24、198.51.100/24 与 203.0.113/24 是协议保留与文档示例段
            if (inIpv4Range(value, 0xC0000000U, 24) || inIpv4Range(value, 0xC0000200U, 24) || inIpv4Range(value, 0xC6336400U, 24) || inIpv4Range(value, 0xCB007100U, 24))
            {
                return true;
            }
            // 192.168/16 是私网；198.18/15 是基准测试保留段
            if (inIpv4Range(value, 0xC0A80000U, 16) || inIpv4Range(value, 0xC6120000U, 15))
            {
                return true;
            }
            // 224/4 组播与 240/4 保留段都不该成为单播目标
            return inIpv4Range(value, 0xE0000000U, 4) || inIpv4Range(value, 0xF0000000U, 4);
        }

        /**
         * @brief 判定 16 字节 IPv6 地址是否属于私网或保留段
         */
        [[nodiscard]] bool isPrivateIpv6(const std::array<std::uint8_t, 16> &bytes)
        {
            const std::uint16_t first  = static_cast<std::uint16_t>((bytes[0] << 8U) | bytes[1]);
            const std::uint16_t second = static_cast<std::uint16_t>((bytes[2] << 8U) | bytes[3]);
            // 全零（未指定地址）与 ::1 都是本机语义
            const bool isUnspecified = std::all_of(bytes.begin(), bytes.end(), [](const std::uint8_t byte) { return byte == 0; });
            const bool isLoopback    = std::all_of(bytes.begin(), bytes.end() - 1, [](const std::uint8_t byte) { return byte == 0; }) && bytes.back() == 1;
            if (isUnspecified || isLoopback)
            {
                return true;
            }
            // IPv4-mapped ::ffff:a.b.c.d 要按内嵌的 IPv4 判断，否则回环与私网会从这里绕过去
            const bool isMapped = std::all_of(bytes.begin(), bytes.begin() + 10, [](const std::uint8_t byte) { return byte == 0; }) && bytes[10] == 0xFF && bytes[11] == 0xFF;
            if (isMapped)
            {
                std::array<std::uint8_t, 16> mapped{};
                std::copy(bytes.begin() + 12, bytes.end(), mapped.begin());
                return isPrivateIpv4(mapped);
            }
            // fc00::/7 唯一本地地址、fe80::/10 链路本地、ff00::/8 组播
            if ((first & 0xFE00U) == 0xFC00U || (first & 0xFFC0U) == 0xFE80U || (first & 0xFF00U) == 0xFF00U)
            {
                return true;
            }
            // 2001:db8::/32 是文档示例段
            return first == 0x2001U && second == 0x0DB8U;
        }

        /**
         * @brief 把主机名归一化成小写并去掉末尾的点
         */
        [[nodiscard]] std::string canonicalHostName(const std::string_view host)
        {
            std::string text{host};
            while (!text.empty() && text.back() == '.')
            {
                text.pop_back();
            }
            std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
            return text;
        }

        /**
         * @brief 判断主机名是否指向云元数据服务
         * @details 这些名字由云厂商在实例内解析到链路本地元数据地址，按名字先拒可以避免
         *          依赖本机 DNS 的表现；即使 allowPrivateNetworks 打开也不能放行。
         */
        [[nodiscard]] bool isMetadataHostName(const std::string_view host)
        {
            const std::string name = canonicalHostName(host);
            return name == "metadata" || name == "metadata.google.internal" || name == "metadata.goog" || name == "instance-data" || name == "instance-data.ec2.internal" ||
                   name == "metadata.tencentyun.com";
        }

        /**
         * @brief 判断主机名是否为本机或局域网别名
         */
        [[nodiscard]] bool isLocalHostName(const std::string_view host)
        {
            const std::string name = canonicalHostName(host);
            // .local 是 mDNS 保留后缀，语义上属于本机或同链路主机，默认不交给外部解析
            return name == "localhost" || name == "localhost.localdomain" || name == "ip6-localhost" || name == "ip6-loopback" || name.ends_with(".localhost") ||
                   name.ends_with(".local");
        }

        /**
         * @brief 判定 4 字节 IPv4 是否落在云元数据服务的传输段
         */
        [[nodiscard]] bool isMetadataIpv4(const std::array<std::uint8_t, 16> &bytes)
        {
            // 169.254/16 是 IPv4 链路本地段，云厂商的实例元数据端点都落在这里：
            // 169.254.169.254 为通用地址，169.254.170.2 是 ECS 任务元数据，
            // 腾讯云使用 169.254.0.23 与 169.254.10.10。整段拦截，避免只认已知端点。
            if (bytes[0] == 169 && bytes[1] == 254)
            {
                return true;
            }
            // 100.100.100.200 是阿里云元数据端点，不在链路本地段内，单独列出
            return bytes[0] == 100 && bytes[1] == 100 && bytes[2] == 100 && bytes[3] == 200;
        }

        /**
         * @brief 使用系统解析器把主机名换成地址文本
         * @param host 主机名；调用方保证不是 IP 字面量
         * @return 去重后的地址列表；解析失败时为空
         */
        [[nodiscard]] std::vector<std::string> resolveWithSystemResolver(const std::string_view host)
        {
            // Windows 上 getaddrinfo 前必须完成 Winsock 初始化；非 Windows 平台该守卫为空操作
            const AsynGyanis::Platform::Socket::Initialization network;
            if (!network.isValid())
            {
                return {};
            }
            addrinfo hints{};
            hints.ai_family          = AF_UNSPEC;
            hints.ai_socktype        = SOCK_STREAM;
            hints.ai_flags           = AI_ADDRCONFIG;
            addrinfo         *result = nullptr;
            const std::string terminated{host};
            if (getaddrinfo(terminated.c_str(), nullptr, &hints, &result) != 0)
            {
                return {};
            }
            std::vector<std::string> addresses;
            for (const addrinfo *entry = result; entry != nullptr; entry = entry->ai_next)
            {
                std::array<char, NI_MAXHOST> text{};
                // 两个长度参数在 Windows 与 POSIX 上的具体类型不同，统一落到 int 再交给系统调用
                const int addressLength = static_cast<int>(entry->ai_addrlen);
                const int textLength    = static_cast<int>(text.size());
                if (getnameinfo(entry->ai_addr, static_cast<socklen_t>(addressLength), text.data(), static_cast<socklen_t>(textLength), nullptr, 0, NI_NUMERICHOST) != 0)
                {
                    continue;
                }
                const std::string address{text.data()};
                if (std::find(addresses.begin(), addresses.end(), address) == addresses.end())
                {
                    addresses.push_back(address);
                }
            }
            freeaddrinfo(result);
            return addresses;
        }
    } // namespace

    OutboundUrlGuard::OutboundUrlGuard(OutboundUrlGuardOptions options, HostAddressResolver resolver) : m_options(options), m_resolver(std::move(resolver))
    {
        // 未注入解析器时走系统解析器；解析本身在阻塞执行器上执行，不在事件循环线程里查 DNS
        if (!m_resolver)
        {
            m_resolver = [](const std::string_view host) { return resolveWithSystemResolver(host); };
        }
    }

    bool OutboundUrlGuard::isPrivateOrReservedAddress(const std::string_view address)
    {
        const std::string_view text = addressText(address);
        if (text.empty())
        {
            // 空文本不是可用的目标地址；按不可出站处理，避免调用方把缺失值当成公网
            return true;
        }
        std::array<std::uint8_t, 16> bytes{};
        const std::string            terminated{text};
        if (inet_pton(AF_INET, terminated.c_str(), bytes.data()) == 1)
        {
            return isPrivateIpv4(bytes);
        }
        if (inet_pton(AF_INET6, terminated.c_str(), bytes.data()) == 1)
        {
            return isPrivateIpv6(bytes);
        }
        // 解析不出来的文本不能当成公网地址放行
        return true;
    }

    bool OutboundUrlGuard::isMetadataAddress(const std::string_view address)
    {
        const std::string_view text = addressText(address);
        if (text.empty())
        {
            return false;
        }
        std::array<std::uint8_t, 16> bytes{};
        const std::string            terminated{text};
        if (inet_pton(AF_INET, terminated.c_str(), bytes.data()) == 1)
        {
            return isMetadataIpv4(bytes);
        }
        if (inet_pton(AF_INET6, terminated.c_str(), bytes.data()) != 1)
        {
            return false;
        }
        const bool isMapped = std::all_of(bytes.begin(), bytes.begin() + 10, [](const std::uint8_t byte) { return byte == 0; }) && bytes[10] == 0xFF && bytes[11] == 0xFF;
        if (isMapped)
        {
            std::array<std::uint8_t, 16> mapped{};
            std::copy(bytes.begin() + 12, bytes.end(), mapped.begin());
            return isMetadataIpv4(mapped);
        }
        // fe80::/10 是 IPv6 链路本地段，同样整段视为元数据服务；AWS 的 fd00:ec2::254
        // 与 GCP 的 fd20:ce::254 落在唯一本地段里，按固定端点补充
        const std::uint16_t                           first       = static_cast<std::uint16_t>((bytes[0] << 8U) | bytes[1]);
        const bool                                    isLinkLocal = (first & 0xFFC0U) == 0xFE80U;
        static constexpr std::array<std::uint8_t, 16> kAwsMetadataV6{0xFD, 0x00, 0x0E, 0xC2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02, 0x54};
        static constexpr std::array<std::uint8_t, 16> kGcpMetadataV6{0xFD, 0x20, 0x00, 0xCE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02, 0x54};
        return isLinkLocal || bytes == kAwsMetadataV6 || bytes == kGcpMetadataV6;
    }

    AsynGyanis::Core::Task<std::vector<std::string>> OutboundUrlGuard::resolve(AsynGyanis::Core::EventLoop &loop, std::string host) const
    {
        const std::string key = canonicalHostName(host);
        if (const std::optional<std::vector<std::string>> cached = cachedAddresses(key); cached.has_value())
        {
            co_return *cached;
        }
        std::vector<std::string> addresses;
        try
        {
            // 主机名与解析器都按值搬进工作线程；协程帧与守卫对象都不跨线程访问
            addresses = co_await AsynGyanis::Core::AsyncExecutor::shared().submit<std::vector<std::string>>(loop, [host = std::move(host), resolver = m_resolver]() mutable
                                                                                                            { return resolver(host); });
        } catch (const std::exception &)
        {
            // 执行器过载等失败按解析失败处理，由 validate 统一转成中文拒绝原因
            addresses.clear();
        }
        rememberAddresses(key, addresses);
        co_return addresses;
    }

    std::optional<std::vector<std::string>> OutboundUrlGuard::cachedAddresses(const std::string &host) const
    {
        if (m_options.cacheTtl <= std::chrono::seconds::zero())
        {
            return std::nullopt;
        }
        const std::lock_guard<std::mutex> lock(m_cacheMutex);
        const auto                        entry = m_cache.find(host);
        if (entry == m_cache.end() || entry->second.expiresAt <= std::chrono::steady_clock::now())
        {
            return std::nullopt;
        }
        return entry->second.addresses;
    }

    void OutboundUrlGuard::rememberAddresses(const std::string &host, const std::vector<std::string> &addresses) const
    {
        if (addresses.empty() || m_options.cacheTtl <= std::chrono::seconds::zero())
        {
            return;
        }
        const std::lock_guard<std::mutex> lock(m_cacheMutex);
        m_cache[host] = CacheEntry{addresses, std::chrono::steady_clock::now() + m_options.cacheTtl};
    }

    AsynGyanis::Core::Task<std::expected<void, std::string>> OutboundUrlGuard::validate(AsynGyanis::Core::EventLoop &loop, const std::string_view url) const
    {
        AsynGyanis::Net::ParsedUrl parsed;
        try
        {
            parsed = AsynGyanis::Net::parseUrl(url);
        } catch (const std::exception &failure)
        {
            // parseUrl 已拒绝 userinfo、非 http(s)、空白与控制字符，且诊断文本不回显凭据
            co_return std::unexpected("Webhook 地址不合法：" + std::string{failure.what()});
        }
        if (isMetadataHostName(parsed.host))
        {
            co_return std::unexpected("Webhook 地址指向云元数据主机，已拒绝：" + parsed.host);
        }
        if (!m_options.allowPrivateNetworks && isLocalHostName(parsed.host))
        {
            // 本机别名不走解析：既省一次 DNS，也不让本机 hosts 的写法影响结论
            co_return std::unexpected("Webhook 地址指向本机主机名，已拒绝：" + parsed.host);
        }
        std::vector<std::string> addresses;
        if (classifyLiteral(parsed.host) != AddressFamily::None)
        {
            addresses.push_back(parsed.host);
        } else
        {
            addresses = co_await resolve(loop, parsed.host);
        }
        if (addresses.empty())
        {
            co_return std::unexpected("Webhook 主机解析失败：" + parsed.host);
        }
        for (const std::string &address: addresses)
        {
            if (isMetadataAddress(address))
            {
                // 元数据端点与整个链路本地段都属于这一条，开关打开也不放行
                co_return std::unexpected("Webhook 地址指向云元数据服务或链路本地地址，已拒绝：" + address);
            }
            if (!m_options.allowPrivateNetworks && isPrivateOrReservedAddress(address))
            {
                co_return std::unexpected("Webhook 地址解析到私网或保留地址，已拒绝：" + address);
            }
        }
        co_return std::expected<void, std::string>{};
    }
} // namespace EdgePulse::Common
