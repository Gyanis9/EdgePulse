#include "Core/Auth/OidcService.h"

#include "Base/Config/ConfigValue.h"
#include "Base/Json/JsonFields.h"
#include "Base/Security/PasswordHash.h"
#include "Base/Security/RandomToken.h"
#include "Core/Auth/AuthService.h"
#include "Core/Cache/KeyValueStore.h"
#include "Core/System/UserRepository.h"
#include "Net/Http/Client/HttpClient.h"

#include <Core/EventLoop/EventLoop.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace EdgePulse::Auth
{
    namespace
    {
        using AsynGyanis::Base::ConfigValue;

        constexpr int         kStateTtlSeconds   = 300;
        constexpr int         kCodeTtlSeconds    = 120;
        constexpr std::size_t kMaxUsernameLength = 32;

        [[nodiscard]] std::string percentEncode(const std::string_view text)
        {
            std::string encoded;
            encoded.reserve(text.size() * 3);
            for (const unsigned char character: text)
            {
                if ((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '-' ||
                    character == '_' || character == '.' || character == '~')
                {
                    encoded.push_back(static_cast<char>(character));
                } else
                {
                    static constexpr char kHex[] = "0123456789ABCDEF";
                    encoded.push_back('%');
                    encoded.push_back(kHex[character >> 4U]);
                    encoded.push_back(kHex[character & 0x0FU]);
                }
            }
            return encoded;
        }

        [[nodiscard]] std::string appendQuery(const std::string &url, const std::string_view key, const std::string_view value)
        {
            std::string result = url;
            result.push_back(url.find('?') == std::string::npos ? '?' : '&');
            result += percentEncode(key);
            result.push_back('=');
            result += percentEncode(value);
            return result;
        }

        [[nodiscard]] std::string formBody(const std::vector<std::pair<std::string, std::string>> &fields)
        {
            std::string body;
            for (const auto &[key, value]: fields)
            {
                if (!body.empty())
                {
                    body.push_back('&');
                }
                body += percentEncode(key);
                body.push_back('=');
                body += percentEncode(value);
            }
            return body;
        }

        [[nodiscard]] std::string lowercase(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
            return text;
        }

        [[nodiscard]] std::string sanitizeUsername(const std::string_view candidate)
        {
            std::string result;
            for (const unsigned char character: lowercase(std::string{candidate}))
            {
                if (result.size() >= kMaxUsernameLength)
                {
                    break;
                }
                if ((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_')
                {
                    result.push_back(static_cast<char>(character));
                } else if (character == '@' || character == '.' || character == '-' || character == '+')
                {
                    result.push_back('_');
                }
            }
            if (result.empty() || result.front() < 'a' || result.front() > 'z')
            {
                result.insert(result.begin(), 'u');
            }
            return result;
        }

        [[nodiscard]] std::string localPart(const std::string_view email)
        {
            const std::size_t at = email.find('@');
            return sanitizeUsername(at == std::string_view::npos ? email : email.substr(0, at));
        }

        [[nodiscard]] std::string redirectWithError(const std::string &frontend, const std::string &message)
        {
            return appendQuery(frontend, "oidcError", message);
        }
    } // namespace

    OidcService::OidcService(OidcConfiguration configuration, System::UserRepository &users, Cache::KeyValueStore &cache, AuthService &auth) :
        m_configuration(std::move(configuration)), m_users(users), m_cache(cache), m_auth(auth)
    {
    }

    AsynGyanis::Core::Task<OidcStartResult> OidcService::start(AsynGyanis::Core::EventLoop &loop, std::string returnPath)
    {
        OidcStartResult result;
        if (!m_configuration.enabled)
        {
            result.error = "OIDC 登录未启用";
            co_return result;
        }
        std::string                    discoveryError;
        const std::optional<Discovery> endpoint = co_await discovery(loop, discoveryError);
        if (!endpoint.has_value())
        {
            result.error = discoveryError;
            co_return result;
        }
        const std::string state    = Common::randomTokenHex(16);
        const std::string nonce    = Common::randomTokenHex(16);
        const std::string verifier = Common::randomTokenHex(32);
        const ConfigValue record   = ConfigValue::object({{"nonce", nonce}, {"verifier", verifier}, {"returnPath", returnPath}});
        if (!co_await m_cache.set(loop, "oidc:state:" + state, record.dump(), std::chrono::seconds{kStateTtlSeconds}))
        {
            result.error = "无法保存 OIDC 登录状态";
            co_return result;
        }
        std::string url    = endpoint->authorizationEndpoint;
        url                = appendQuery(url, "response_type", "code");
        url                = appendQuery(url, "client_id", m_configuration.clientId);
        url                = appendQuery(url, "redirect_uri", m_configuration.redirectUri);
        url                = appendQuery(url, "scope", m_configuration.scopes);
        url                = appendQuery(url, "state", state);
        url                = appendQuery(url, "nonce", nonce);
        url                = appendQuery(url, "code_challenge", pkceS256Challenge(verifier));
        url                = appendQuery(url, "code_challenge_method", "S256");
        result.ok          = true;
        result.redirectUrl = std::move(url);
        co_return result;
    }

    AsynGyanis::Core::Task<OidcCallbackResult> OidcService::handleCallback(AsynGyanis::Core::EventLoop &loop, std::string code, std::string state)
    {
        OidcCallbackResult result;
        if (!m_configuration.enabled)
        {
            result.redirectUrl = redirectWithError(m_configuration.frontendRedirect, "OIDC 登录未启用");
            co_return result;
        }
        if (code.empty() || state.empty())
        {
            result.error       = "OIDC 回调缺少 code 或 state";
            result.redirectUrl = redirectWithError(m_configuration.frontendRedirect, result.error);
            co_return result;
        }
        const std::string                stateKey    = "oidc:state:" + state;
        const std::optional<std::string> storedState = co_await m_cache.get(loop, stateKey);
        static_cast<void>(co_await m_cache.erase(loop, stateKey));
        std::optional<ConfigValue> stateValue;
        if (storedState.has_value())
        {
            stateValue = AsynGyanis::Base::parseConfigValue(*storedState);
        }
        if (!stateValue.has_value() || !stateValue->is_object())
        {
            result.error       = "OIDC 登录状态已过期，请重新登录";
            result.redirectUrl = redirectWithError(m_configuration.frontendRedirect, result.error);
            co_return result;
        }
        const std::string nonce      = Json::stringField(*stateValue, "nonce");
        const std::string verifier   = Json::stringField(*stateValue, "verifier");
        const std::string returnPath = Json::stringField(*stateValue, "returnPath");

        std::string                    discoveryError;
        const std::optional<Discovery> endpoint = co_await discovery(loop, discoveryError);
        if (!endpoint.has_value())
        {
            result.error       = discoveryError;
            result.redirectUrl = redirectWithError(m_configuration.frontendRedirect, result.error);
            co_return result;
        }
        std::string                      exchangeError;
        const std::optional<std::string> idToken = co_await exchangeCode(loop, *endpoint, code, verifier, exchangeError);
        if (!idToken.has_value())
        {
            result.error       = exchangeError;
            result.redirectUrl = redirectWithError(m_configuration.frontendRedirect, result.error);
            co_return result;
        }
        std::string                  jwksError;
        const std::vector<JwtRsaKey> keys = co_await fetchJwks(loop, endpoint->jwksUri, jwksError);
        if (keys.empty())
        {
            result.error       = jwksError;
            result.redirectUrl = redirectWithError(m_configuration.frontendRedirect, result.error);
            co_return result;
        }
        const auto            nowSeconds   = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const JwtVerification verification = verifyRs256Jwt(*idToken, keys, endpoint->issuer, m_configuration.clientId, nonce, static_cast<std::int64_t>(nowSeconds));
        if (!verification.ok)
        {
            result.error       = verification.error;
            result.redirectUrl = redirectWithError(m_configuration.frontendRedirect, result.error);
            co_return result;
        }
        std::string                       userError;
        const std::optional<std::int64_t> userId = co_await resolveUser(loop, verification.claims, userError);
        if (!userId.has_value())
        {
            result.error       = userError;
            result.redirectUrl = redirectWithError(m_configuration.frontendRedirect, result.error);
            co_return result;
        }
        const std::string oneTimeCode = Common::randomTokenHex(24);
        const ConfigValue loginRecord = ConfigValue::object({{"userId", *userId}, {"returnPath", returnPath}});
        if (!co_await m_cache.set(loop, "oidc:code:" + oneTimeCode, loginRecord.dump(), std::chrono::seconds{kCodeTtlSeconds}))
        {
            result.error       = "无法保存 OIDC 登录凭据";
            result.redirectUrl = redirectWithError(m_configuration.frontendRedirect, result.error);
            co_return result;
        }
        std::string redirect = appendQuery(m_configuration.frontendRedirect, "oidcCode", oneTimeCode);
        if (!returnPath.empty())
        {
            redirect = appendQuery(redirect, "return", returnPath);
        }
        result.ok          = true;
        result.redirectUrl = std::move(redirect);
        co_return result;
    }

    AsynGyanis::Core::Task<OidcExchangeResult> OidcService::exchange(AsynGyanis::Core::EventLoop &loop, std::string code, std::string clientIp, std::string traceId)
    {
        OidcExchangeResult result;
        if (code.empty())
        {
            result.error = "缺少一次性登录凭据";
            co_return result;
        }
        const std::string                key    = "oidc:code:" + code;
        const std::optional<std::string> stored = co_await m_cache.get(loop, key);
        static_cast<void>(co_await m_cache.erase(loop, key));
        std::optional<ConfigValue> value;
        if (stored.has_value())
        {
            value = AsynGyanis::Base::parseConfigValue(*stored);
        }
        if (!value.has_value() || !value->is_object())
        {
            result.error = "一次性登录凭据已过期，请重新登录";
            co_return result;
        }
        const auto userId = Json::optionalInteger(*value, "userId");
        if (!userId.has_value())
        {
            result.error = "一次性登录凭据不完整";
            co_return result;
        }
        result.returnPath = Json::stringField(*value, "returnPath");
        const auto issued = co_await m_auth.issueSessionForUser(loop, *userId, std::move(clientIp), std::move(traceId), "OIDC 登录成功");
        if (!issued.has_value())
        {
            result.error = issued.error().message;
            co_return result;
        }
        result.ok        = true;
        result.token     = issued->token;
        result.expiresIn = issued->expiresIn;
        co_return result;
    }

    AsynGyanis::Core::Task<std::optional<OidcService::Discovery>> OidcService::discovery(AsynGyanis::Core::EventLoop &loop, std::string &error)
    {
        if (m_discovery.has_value())
        {
            co_return m_discovery;
        }
        std::string url = m_configuration.issuer;
        while (!url.empty() && url.back() == '/')
        {
            url.pop_back();
        }
        url += "/.well-known/openid-configuration";
        AsynGyanis::Net::HttpClientRequest request;
        request.method = "GET";
        request.headers.emplace_back("Accept", "application/json");
        const auto response = co_await AsynGyanis::Net::HttpClient::send(loop, url, request, std::chrono::seconds{10});
        if (!response || response->statusCode != 200)
        {
            error = "无法读取 OIDC 发现文档";
            co_return std::nullopt;
        }
        const auto value = AsynGyanis::Base::parseConfigValue(response->body);
        if (!value.has_value() || !value->is_object())
        {
            error = "OIDC 发现文档不是 JSON 对象";
            co_return std::nullopt;
        }
        Discovery discovered;
        discovered.issuer                = Json::stringField(*value, "issuer");
        discovered.authorizationEndpoint = Json::stringField(*value, "authorization_endpoint");
        discovered.tokenEndpoint         = Json::stringField(*value, "token_endpoint");
        discovered.jwksUri               = Json::stringField(*value, "jwks_uri");
        std::string configuredIssuer     = m_configuration.issuer;
        while (!configuredIssuer.empty() && configuredIssuer.back() == '/')
        {
            configuredIssuer.pop_back();
        }
        std::string discoveredIssuer = discovered.issuer;
        while (!discoveredIssuer.empty() && discoveredIssuer.back() == '/')
        {
            discoveredIssuer.pop_back();
        }
        if (discoveredIssuer != configuredIssuer || discovered.authorizationEndpoint.empty() || discovered.tokenEndpoint.empty() || discovered.jwksUri.empty())
        {
            error = "OIDC 发现文档的 iss 或端点不完整";
            co_return std::nullopt;
        }
        m_discovery = discovered;
        co_return m_discovery;
    }

    AsynGyanis::Core::Task<std::vector<JwtRsaKey>> OidcService::fetchJwks(AsynGyanis::Core::EventLoop &loop, const std::string &jwksUri, std::string &error)
    {
        AsynGyanis::Net::HttpClientRequest request;
        request.method = "GET";
        request.headers.emplace_back("Accept", "application/json");
        const auto response = co_await AsynGyanis::Net::HttpClient::send(loop, jwksUri, request, std::chrono::seconds{10});
        if (!response || response->statusCode != 200)
        {
            error = "无法读取 OIDC JWKS";
            co_return std::vector<JwtRsaKey>{};
        }
        const auto value = AsynGyanis::Base::parseConfigValue(response->body);
        if (!value.has_value() || !value->is_object())
        {
            error = "OIDC JWKS 不是 JSON 对象";
            co_return std::vector<JwtRsaKey>{};
        }
        const auto keys = value->find("keys");
        if (keys == value->end() || !keys->is_array())
        {
            error = "OIDC JWKS 缺少 keys";
            co_return std::vector<JwtRsaKey>{};
        }
        std::vector<JwtRsaKey> result;
        for (const ConfigValue &entry: *keys)
        {
            if (!entry.is_object() || Json::stringField(entry, "kty") != "RSA")
            {
                continue;
            }
            JwtRsaKey key;
            key.keyId    = Json::stringField(entry, "kid");
            key.modulus  = Json::stringField(entry, "n");
            key.exponent = Json::stringField(entry, "e");
            if (!key.keyId.empty() && !key.modulus.empty() && !key.exponent.empty())
            {
                result.push_back(std::move(key));
            }
        }
        if (result.empty())
        {
            error = "OIDC JWKS 中没有可用的 RSA 公钥";
        }
        co_return result;
    }

    AsynGyanis::Core::Task<std::optional<std::string>> OidcService::exchangeCode(AsynGyanis::Core::EventLoop &loop, const Discovery &discovery, const std::string_view code,
                                                                                 const std::string_view verifier, std::string &error)
    {
        std::vector<std::pair<std::string, std::string>> fields{{"grant_type", "authorization_code"},
                                                                {"code", std::string{code}},
                                                                {"redirect_uri", m_configuration.redirectUri},
                                                                {"client_id", m_configuration.clientId},
                                                                {"code_verifier", std::string{verifier}}};
        if (!m_configuration.clientSecret.empty())
        {
            fields.emplace_back("client_secret", m_configuration.clientSecret);
        }
        const std::string                  body = formBody(fields);
        AsynGyanis::Net::HttpClientRequest request;
        request.method      = "POST";
        request.contentType = "application/x-www-form-urlencoded";
        request.body        = body;
        request.headers.emplace_back("Accept", "application/json");
        const auto response = co_await AsynGyanis::Net::HttpClient::send(loop, discovery.tokenEndpoint, request, std::chrono::seconds{10});
        if (!response || response->statusCode != 200)
        {
            error = "OIDC 令牌端点拒绝换取 ID Token";
            co_return std::nullopt;
        }
        const auto value = AsynGyanis::Base::parseConfigValue(response->body);
        if (!value.has_value() || !value->is_object())
        {
            error = "OIDC 令牌响应不是 JSON 对象";
            co_return std::nullopt;
        }
        const std::string idToken = Json::stringField(*value, "id_token");
        if (idToken.empty())
        {
            error = "OIDC 令牌响应缺少 id_token";
            co_return std::nullopt;
        }
        co_return idToken;
    }

    AsynGyanis::Core::Task<std::optional<std::int64_t>> OidcService::resolveUser(AsynGyanis::Core::EventLoop &loop, const JwtClaims &claims, std::string &error)
    {
        if (claims.email.empty())
        {
            error = "ID Token 缺少 email，无法映射本地账号";
            co_return std::nullopt;
        }
        const std::string email = lowercase(claims.email);
        if (!emailDomainAllowed(email))
        {
            error = "该邮箱域不允许登录";
            co_return std::nullopt;
        }
        const auto existing = co_await m_users.findByEmail(loop, email);
        if (existing.has_value())
        {
            if (existing->status != 1)
            {
                error = "账号已停用";
                co_return std::nullopt;
            }
            co_return existing->id;
        }
        if (!m_configuration.autoProvision)
        {
            error = "账号未开通，请联系管理员";
            co_return std::nullopt;
        }
        std::string username = sanitizeUsername(claims.preferredUsername.empty() ? localPart(email) : claims.preferredUsername);
        if (co_await m_users.usernameExists(loop, username))
        {
            username += "_oidc";
            if (co_await m_users.usernameExists(loop, username))
            {
                username += "_" + Common::randomTokenHex(2);
            }
        }
        System::UserWrite write;
        write.username = username;
        write.nickname = claims.displayName.empty() ? (claims.preferredUsername.empty() ? username : claims.preferredUsername) : claims.displayName;
        write.email    = email;
        write.deptId   = m_configuration.defaultDepartmentId;
        write.status   = 1;
        if (m_configuration.defaultRoleId > 0)
        {
            write.roleIds = std::vector<std::int64_t>{m_configuration.defaultRoleId};
        }
        // 外部身份不走口令登录：随机口令哈希后即弃，数据库里不存在可用的明文口令
        const std::string  passwordHash = Common::hashPassword(Common::randomTokenHex(32));
        const std::int64_t userId       = co_await m_users.create(loop, write, passwordHash, 0);
        if (userId <= 0)
        {
            error = "自动创建本地账号失败";
            co_return std::nullopt;
        }
        co_return userId;
    }

    bool OidcService::emailDomainAllowed(const std::string_view email) const
    {
        if (m_configuration.allowedEmailDomains.empty())
        {
            return true;
        }
        const std::size_t at = email.rfind('@');
        if (at == std::string_view::npos)
        {
            return false;
        }
        const std::string domain = lowercase(std::string{email.substr(at + 1)});
        std::size_t       begin  = 0;
        while (begin <= m_configuration.allowedEmailDomains.size())
        {
            const std::size_t comma = m_configuration.allowedEmailDomains.find(',', begin);
            std::string       entry = m_configuration.allowedEmailDomains.substr(begin, comma == std::string::npos ? std::string::npos : comma - begin);
            entry.erase(0, entry.find_first_not_of(" \t"));
            entry.erase(entry.find_last_not_of(" \t") + 1);
            if (lowercase(entry) == domain)
            {
                return true;
            }
            if (comma == std::string::npos)
            {
                break;
            }
            begin = comma + 1;
        }
        return false;
    }
} // namespace EdgePulse::Auth
