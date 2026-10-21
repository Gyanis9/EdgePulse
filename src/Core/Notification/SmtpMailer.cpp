#include "Core/Notification/SmtpMailer.h"

#include "Base/Coding/Base64.h"
#include "Base/Exception/EdgePulseException.h"
#include "Core/Tls/TlsContext.h"
#include "Core/Tls/TlsSocket.h"
#include "Net/Tcp/TcpClient.h"
#include "Net/Tcp/TcpStream.h"

#include <Core/EventLoop/EventLoop.h>

#include <openssl/ssl.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace EdgePulse::Notification
{
    namespace
    {
        class SmtpTransport
        {
        public:
            virtual ~SmtpTransport() = default;

            virtual AsynGyanis::Core::Task<std::string> readLine()                      = 0;
            virtual AsynGyanis::Core::Task<>            writeAll(std::string_view text) = 0;
            virtual void                                close()                         = 0;
        };

        class PlainTransport final : public SmtpTransport
        {
        public:
            explicit PlainTransport(std::unique_ptr<AsynGyanis::Net::TcpStream> stream) : m_stream(std::move(stream))
            {
            }

            AsynGyanis::Core::Task<std::string> readLine() override
            {
                std::string line = co_await m_stream->readUntil('\n');
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                co_return line;
            }

            AsynGyanis::Core::Task<> writeAll(const std::string_view text) override
            {
                co_await m_stream->writeAll(text.data(), text.size());
            }

            void close() override
            {
                m_stream->close();
            }

            [[nodiscard]] std::unique_ptr<AsynGyanis::Net::TcpStream> takeStream()
            {
                return std::move(m_stream);
            }

        private:
            std::unique_ptr<AsynGyanis::Net::TcpStream> m_stream;
        };

        class TlsTransport final : public SmtpTransport
        {
        public:
            explicit TlsTransport(AsynGyanis::Core::TlsSocket socket) : m_socket(std::move(socket))
            {
            }

            AsynGyanis::Core::Task<std::string> readLine() override
            {
                while (m_buffer.find('\n') == std::string::npos)
                {
                    char       chunk[1024];
                    const auto received = co_await m_socket.asyncReceive(chunk, sizeof(chunk));
                    if (received <= 0)
                    {
                        break;
                    }
                    m_buffer.append(chunk, static_cast<std::size_t>(received));
                }
                const std::size_t newline = m_buffer.find('\n');
                std::string       line    = m_buffer.substr(0, newline);
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                if (newline == std::string::npos)
                {
                    m_buffer.clear();
                } else
                {
                    m_buffer.erase(0, newline + 1);
                }
                co_return line;
            }

            AsynGyanis::Core::Task<> writeAll(const std::string_view text) override
            {
                std::size_t written = 0;
                while (written < text.size())
                {
                    const auto count = co_await m_socket.asyncSend(text.data() + written, text.size() - written);
                    if (count <= 0)
                    {
                        throw Common::EdgePulseException("SMTP TLS 写入失败：连接已关闭");
                    }
                    written += static_cast<std::size_t>(count);
                }
            }

            void close() override
            {
                m_socket.close();
            }

        private:
            AsynGyanis::Core::TlsSocket m_socket;
            std::string                 m_buffer;
        };

        struct Endpoint
        {
            bool          tls{false};      ///< smtps://
            bool          startTls{false}; ///< starttls://
            std::string   host;            ///< 主机名
            std::uint16_t port{25};        ///< 端口
        };

        [[nodiscard]] bool parseEndpoint(const std::string &url, Endpoint &endpoint)
        {
            std::string_view rest;
            if (url.starts_with("smtps://"))
            {
                endpoint.tls  = true;
                endpoint.port = 465;
                rest          = std::string_view{url}.substr(8);
            } else if (url.starts_with("smtp://"))
            {
                endpoint.port = 25;
                rest          = std::string_view{url}.substr(7);
            } else if (url.starts_with("starttls://"))
            {
                endpoint.startTls = true;
                endpoint.port     = 587;
                rest              = std::string_view{url}.substr(11);
            } else
            {
                return false;
            }
            const std::size_t slash = rest.find('/');
            if (slash != std::string_view::npos)
            {
                rest = rest.substr(0, slash);
            }
            if (rest.empty())
            {
                return false;
            }
            std::string_view portText;
            if (rest.front() == '[')
            {
                const std::size_t close = rest.find(']');
                if (close == std::string_view::npos)
                {
                    return false;
                }
                endpoint.host = std::string{rest.substr(0, close + 1)};
                if (close + 1 < rest.size() && rest[close + 1] == ':')
                {
                    portText = rest.substr(close + 2);
                }
            } else
            {
                const std::size_t colon = rest.rfind(':');
                endpoint.host           = std::string{rest.substr(0, colon)};
                if (colon != std::string_view::npos)
                {
                    portText = rest.substr(colon + 1);
                }
            }
            if (endpoint.host.empty())
            {
                return false;
            }
            // 去掉 IPv6 方括号，TcpClient 要的是纯地址
            if (endpoint.host.front() == '[' && endpoint.host.back() == ']')
            {
                endpoint.host = endpoint.host.substr(1, endpoint.host.size() - 2);
            }
            if (!portText.empty())
            {
                int port = 0;
                for (const char character: portText)
                {
                    if (!std::isdigit(static_cast<unsigned char>(character)) || port > 6553)
                    {
                        return false;
                    }
                    port = port * 10 + (character - '0');
                }
                if (port < 1 || port > 65535)
                {
                    return false;
                }
                endpoint.port = static_cast<std::uint16_t>(port);
            }
            return true;
        }

        [[nodiscard]] int replyCode(const std::string &line)
        {
            if (line.size() < 3)
            {
                return 0;
            }
            int code = 0;
            for (std::size_t index = 0; index < 3; ++index)
            {
                if (!std::isdigit(static_cast<unsigned char>(line[index])))
                {
                    return 0;
                }
                code = code * 10 + (line[index] - '0');
            }
            return code;
        }

        [[nodiscard]] AsynGyanis::Core::Task<int> readReply(SmtpTransport &transport)
        {
            std::string first = co_await transport.readLine();
            int         code  = replyCode(first);
            while (first.size() > 3 && first[3] == '-')
            {
                const std::string next = co_await transport.readLine();
                if (next.size() < 4 || replyCode(next) != code)
                {
                    break;
                }
                first = next;
            }
            co_return code;
        }

        [[nodiscard]] AsynGyanis::Core::Task<bool> command(SmtpTransport &transport, const std::string &line, const int expected, std::string &error)
        {
            co_await transport.writeAll(line + "\r\n");
            const int code = co_await readReply(transport);
            if (code != expected)
            {
                error = "SMTP 命令被拒：" + std::to_string(code);
                co_return false;
            }
            co_return true;
        }

        [[nodiscard]] AsynGyanis::Core::Task<std::unique_ptr<SmtpTransport>> upgradeToTls(AsynGyanis::Core::EventLoop &loop, std::unique_ptr<AsynGyanis::Net::TcpStream> stream,
                                                                                          const std::string &host, const std::string &caFile, std::string &error)
        {
            AsynGyanis::Core::TlsPolicy policy;
            policy.certificateAuthorityFile = caFile;
            AsynGyanis::Core::TlsContext context(policy, AsynGyanis::Core::TlsContext::Role::Client);
            SSL *const                   ssl = SSL_new(context.nativeHandle());
            if (ssl == nullptr || SSL_set_fd(ssl, stream->socket().fileDescriptor()) == 0)
            {
                if (ssl != nullptr)
                {
                    SSL_free(ssl);
                }
                error = "SMTP TLS 会话创建失败";
                co_return nullptr;
            }
            static_cast<void>(SSL_set_tlsext_host_name(ssl, host.c_str()));
            static_cast<void>(SSL_set1_host(ssl, host.c_str()));
            AsynGyanis::Core::TlsSocket tls(ssl, loop, std::move(stream->socket()), AsynGyanis::Core::TlsSocket::Role::Client);
            co_await tls.handshake();
            co_return std::make_unique<TlsTransport>(std::move(tls));
        }

        [[nodiscard]] std::string base64Lines(const std::string &text)
        {
            const std::string encoded = AsynGyanis::Base::base64Encode(text);
            std::string       result;
            for (std::size_t offset = 0; offset < encoded.size(); offset += 76)
            {
                result += encoded.substr(offset, 76);
                result += "\r\n";
            }
            return result;
        }
    } // namespace

    AsynGyanis::Core::Task<bool> SmtpMailer::send(AsynGyanis::Core::EventLoop &loop, const SmtpSettings &settings, std::string subject, std::string body, std::string &error)
    {
        Endpoint endpoint;
        if (!parseEndpoint(settings.endpoint, endpoint))
        {
            error = "SMTP 地址必须是 smtp://、smtps:// 或 starttls://host:port";
            co_return false;
        }
        if (settings.recipients.empty())
        {
            error = "邮件渠道没有收件人";
            co_return false;
        }
        const std::string sender = settings.sender.empty() ? settings.username : settings.sender;
        if (sender.empty())
        {
            error = "邮件渠道缺少发件人";
            co_return false;
        }

        std::unique_ptr<SmtpTransport> transport;
        try
        {
            auto stream = co_await AsynGyanis::Net::TcpClient::connect(loop, endpoint.host, endpoint.port);
            if (!stream)
            {
                error = "SMTP 连接失败：" + endpoint.host;
                co_return false;
            }
            if (endpoint.tls)
            {
                transport = co_await upgradeToTls(loop, std::move(stream), endpoint.host, settings.caFile, error);
                if (!transport)
                {
                    co_return false;
                }
            } else
            {
                transport = std::make_unique<PlainTransport>(std::move(stream));
            }
        } catch (const std::exception &failure)
        {
            error = std::string{"SMTP 连接失败："} + failure.what();
            co_return false;
        }

        try
        {
            if (const int greeting = co_await readReply(*transport); greeting != 220)
            {
                error = "SMTP 服务器未返回 220，实际 " + std::to_string(greeting);
                co_return false;
            }
            if (!co_await command(*transport, "EHLO edgepulse.local", 250, error))
            {
                co_return false;
            }
            if (endpoint.startTls)
            {
                if (!co_await command(*transport, "STARTTLS", 220, error))
                {
                    co_return false;
                }
                auto *plain = dynamic_cast<PlainTransport *>(transport.get());
                if (plain == nullptr)
                {
                    error = "SMTP STARTTLS 状态异常";
                    co_return false;
                }
                auto upgraded = co_await upgradeToTls(loop, plain->takeStream(), endpoint.host, settings.caFile, error);
                if (!upgraded)
                {
                    co_return false;
                }
                transport = std::move(upgraded);
                // RFC 3207：升级后必须重新 EHLO，能力列表才是 TLS 通道上的
                if (!co_await command(*transport, "EHLO edgepulse.local", 250, error))
                {
                    co_return false;
                }
            }
            if (!settings.username.empty())
            {
                const std::string plain = AsynGyanis::Base::base64Encode("\0" + settings.username + "\0" + settings.password);
                if (!co_await command(*transport, "AUTH PLAIN " + plain, 235, error))
                {
                    co_return false;
                }
            }
            if (!co_await command(*transport, "MAIL FROM:<" + sender + ">", 250, error))
            {
                co_return false;
            }
            for (const std::string &recipient: settings.recipients)
            {
                if (!co_await command(*transport, "RCPT TO:<" + recipient + ">", 250, error))
                {
                    co_return false;
                }
            }
            if (!co_await command(*transport, "DATA", 354, error))
            {
                co_return false;
            }
            std::string message = "From: " + sender + "\r\nTo: ";
            for (std::size_t index = 0; index < settings.recipients.size(); ++index)
            {
                message += index == 0 ? settings.recipients[index] : ", " + settings.recipients[index];
            }
            message += "\r\nSubject: =?UTF-8?B?" + AsynGyanis::Base::base64Encode(subject) + "?=\r\n";
            message += "MIME-Version: 1.0\r\nContent-Type: text/plain; charset=UTF-8\r\nContent-Transfer-Encoding: base64\r\n\r\n";
            message += base64Lines(body);
            message += ".\r\n";
            co_await transport->writeAll(message);
            const int accepted = co_await readReply(*transport);
            if (accepted != 250)
            {
                error = "SMTP 未接收邮件，实际 " + std::to_string(accepted);
                co_return false;
            }
            co_await transport->writeAll("QUIT\r\n");
            static_cast<void>(co_await readReply(*transport));
            transport->close();
            co_return true;
        } catch (const std::exception &failure)
        {
            error = std::string{"SMTP 交互失败："} + failure.what();
            transport->close();
            co_return false;
        }
    }
} // namespace EdgePulse::Notification
