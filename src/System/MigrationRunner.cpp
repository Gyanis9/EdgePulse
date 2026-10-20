#include "System/MigrationRunner.h"

#include "Common/EdgePulseException.h"
#include "Common/TimeFormat.h"
#include "Core/Crypto/Digest.h"
#include "Database/Common/DatabaseConnection.h"
#include "Database/Common/DatabaseValue.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace EdgePulse::System
{
    namespace
    {
        struct MigrationFile
        {
            std::int64_t          version{0};  ///< 版本号
            std::string           description; ///< 文件名里的说明
            std::filesystem::path path;        ///< 文件路径
        };

        /**
         * @brief 去掉首尾空白
         */
        [[nodiscard]] std::string trimCopy(const std::string_view text)
        {
            const auto begin = text.find_first_not_of(" \t\r\n");
            if (begin == std::string_view::npos)
            {
                return {};
            }
            const auto end = text.find_last_not_of(" \t\r\n");
            return std::string(text.substr(begin, end - begin + 1));
        }

        /**
         * @brief 从文件名解析版本与说明
         * @param path 文件名
         * @return 符合 V<数字>__<说明>.sql 时返回解析结果
         */
        [[nodiscard]] std::optional<MigrationFile> parseMigrationFileName(const std::filesystem::path &path)
        {
            const std::string fileName = path.filename().string();
            // 最小形状是 V1__x.sql；长度不够的直接忽略，避免把临时文件当迁移
            if (fileName.size() < 8 || fileName.front() != 'V' || path.extension() != ".sql")
            {
                return std::nullopt;
            }
            const std::size_t separator = fileName.find("__");
            if (separator == std::string::npos || separator <= 1)
            {
                return std::nullopt;
            }
            std::int64_t version    = 0;
            const auto [end, error] = std::from_chars(fileName.data() + 1, fileName.data() + separator, version);
            // 版本必须是完整数字：V1x__ 这类拼写错误在启动期就该暴露，而不是被跳过
            if (error != std::errc{} || end != fileName.data() + separator || version <= 0)
            {
                return std::nullopt;
            }
            const std::size_t descriptionBegin = separator + 2;
            const std::size_t descriptionEnd   = fileName.size() - std::string{".sql"}.size();
            if (descriptionEnd <= descriptionBegin)
            {
                return std::nullopt;
            }
            return MigrationFile{version, fileName.substr(descriptionBegin, descriptionEnd - descriptionBegin), path};
        }

        /**
         * @brief 读整个迁移文件
         */
        [[nodiscard]] std::string readFile(const std::filesystem::path &path)
        {
            std::ifstream stream(path, std::ios::binary);
            if (!stream)
            {
                throw Common::MigrationException("读不到迁移文件：" + path.string());
            }
            return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        }

        /**
         * @brief 创建版本表（幂等）
         */
        void ensureSchemaMigrations(AsynGyanis::Database::DatabaseConnection &connection)
        {
            const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result = connection.execute("CREATE TABLE IF NOT EXISTS schema_migrations ("
                                                                                                    "version BIGINT PRIMARY KEY, "
                                                                                                    "description VARCHAR(255) NOT NULL, "
                                                                                                    "checksum VARCHAR(64) NOT NULL, "
                                                                                                    "applied_at DATETIME NOT NULL)");
            if (!result)
            {
                throw Common::MigrationException("创建 schema_migrations 失败：" + connection.lastError());
            }
        }

        /**
         * @brief 读出已应用的版本与校验和
         */
        [[nodiscard]] std::unordered_map<std::int64_t, std::string> readAppliedVersions(AsynGyanis::Database::DatabaseConnection &connection)
        {
            std::unordered_map<std::int64_t, std::string>               applied;
            const std::unique_ptr<AsynGyanis::Database::DatabaseResult> result = connection.execute("SELECT version, checksum FROM schema_migrations");
            if (!result)
            {
                throw Common::MigrationException("读取 schema_migrations 失败：" + connection.lastError());
            }
            while (result->next())
            {
                const AsynGyanis::Database::DatabaseValue versionValue  = result->getValue("version");
                const AsynGyanis::Database::DatabaseValue checksumValue = result->getValue("checksum");
                if (const auto *version = std::get_if<std::int64_t>(&versionValue); version != nullptr)
                {
                    if (const auto *checksum = std::get_if<std::string>(&checksumValue); checksum != nullptr)
                    {
                        applied.emplace(*version, *checksum);
                    }
                }
            }
            return applied;
        }

        /**
         * @brief 把一条语句的错误拼成可定位的中文原因
         */
        void throwStatementFailure(const MigrationFile &file, const std::size_t statementIndex, const std::string &lastError)
        {
            throw Common::MigrationException("迁移 V" + std::to_string(file.version) + "（" + file.description + "）第 " + std::to_string(statementIndex + 1) +
                                             " 条语句执行失败：" + lastError);
        }
    } // namespace

    std::vector<std::string> splitSqlStatements(const std::string_view script)
    {
        std::vector<std::string> statements;
        std::string              current;
        bool                     inSingleQuote  = false;
        bool                     inDoubleQuote  = false;
        bool                     inLineComment  = false;
        bool                     inBlockComment = false;

        for (std::size_t index = 0; index < script.size(); ++index)
        {
            const char character = script[index];
            const char next      = index + 1 < script.size() ? script[index + 1] : '\0';

            if (inLineComment)
            {
                if (character == '\n')
                {
                    inLineComment = false;
                }
                continue;
            }
            if (inBlockComment)
            {
                if (character == '*' && next == '/')
                {
                    inBlockComment = false;
                    ++index;
                }
                continue;
            }
            if (inSingleQuote || inDoubleQuote)
            {
                current.push_back(character);
                const char quote = inSingleQuote ? '\'' : '"';
                if (character == quote)
                {
                    // 两个连续引号是转义，不结束字符串
                    if (next == quote)
                    {
                        current.push_back(next);
                        ++index;
                    } else
                    {
                        inSingleQuote = false;
                        inDoubleQuote = false;
                    }
                }
                continue;
            }

            if (character == '-' && next == '-')
            {
                inLineComment = true;
                ++index;
                continue;
            }
            if (character == '/' && next == '*')
            {
                inBlockComment = true;
                ++index;
                continue;
            }
            if (character == '\'')
            {
                inSingleQuote = true;
                current.push_back(character);
                continue;
            }
            if (character == '"')
            {
                inDoubleQuote = true;
                current.push_back(character);
                continue;
            }
            if (character == ';')
            {
                if (std::string statement = trimCopy(current); !statement.empty())
                {
                    statements.push_back(std::move(statement));
                }
                current.clear();
                continue;
            }
            current.push_back(character);
        }

        // 引号或块注释未闭合是迁移文件写坏了，不能把半条语句交给数据库
        if (inSingleQuote || inDoubleQuote || inBlockComment)
        {
            throw Common::MigrationException("迁移 SQL 存在未闭合的引号或块注释");
        }
        if (std::string statement = trimCopy(current); !statement.empty())
        {
            statements.push_back(std::move(statement));
        }
        return statements;
    }

    void MigrationRunner::apply(AsynGyanis::Database::DatabaseConnection &connection, const std::filesystem::path &directory)
    {
        std::vector<MigrationFile> files;
        std::error_code            filesystemError;
        for (const std::filesystem::directory_entry &entry: std::filesystem::directory_iterator(directory, filesystemError))
        {
            if (!entry.is_regular_file(filesystemError))
            {
                continue;
            }
            if (std::optional<MigrationFile> file = parseMigrationFileName(entry.path()); file.has_value())
            {
                files.push_back(std::move(*file));
            }
        }
        if (filesystemError)
        {
            throw Common::MigrationException("遍历迁移目录失败：" + directory.string() + "：" + filesystemError.message());
        }

        std::sort(files.begin(), files.end(), [](const MigrationFile &left, const MigrationFile &right) { return left.version < right.version; });
        for (std::size_t index = 1; index < files.size(); ++index)
        {
            // 同版本两份文件时执行顺序取决于目录遍历顺序，必须当场拒绝
            if (files[index].version == files[index - 1].version)
            {
                throw Common::MigrationException("迁移版本重复：V" + std::to_string(files[index].version));
            }
        }

        ensureSchemaMigrations(connection);
        const std::unordered_map<std::int64_t, std::string> applied = readAppliedVersions(connection);

        for (const MigrationFile &file: files)
        {
            const std::string script   = readFile(file.path);
            const std::string checksum = AsynGyanis::Core::Digest::sha256Hex(script);
            if (const auto existing = applied.find(file.version); existing != applied.end())
            {
                // 已应用的版本内容被改过：拒绝启动，避免“库里的结构与文件已经不是一回事”
                if (existing->second != checksum)
                {
                    throw Common::MigrationException("迁移 V" + std::to_string(file.version) + " 的校验和不一致：文件被修改过，请新增一版迁移而不是改旧文件");
                }
                continue;
            }

            const std::vector<std::string> statements = splitSqlStatements(script);
            for (std::size_t statementIndex = 0; statementIndex < statements.size(); ++statementIndex)
            {
                if (!connection.execute(statements[statementIndex]))
                {
                    throwStatementFailure(file, statementIndex, connection.lastError());
                }
            }

            const std::array<AsynGyanis::Database::DatabaseValue, 4> parameters{file.version, file.description, checksum, Common::nowIso8601Utc()};
            if (!connection.execute("INSERT INTO schema_migrations (version, description, checksum, applied_at) VALUES (?, ?, ?, ?)", parameters))
            {
                throw Common::MigrationException("记录迁移 V" + std::to_string(file.version) + " 失败：" + connection.lastError());
            }
        }
    }
} // namespace EdgePulse::System
