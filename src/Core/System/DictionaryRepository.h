/**
 * @file DictionaryRepository.h
 * @brief 字典与参数配置仓储
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Api/Pagination.h"
#include "Base/Database/Database.h"
#include "Core/System/SystemModels.h"

#include <Core/Coroutine/Task.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace EdgePulse::System
{
    /**
     * @brief 字典与参数仓储
     */
    class DictionaryRepository
    {
    public:
        /**
         * @brief 构造
         * @param database 数据库封装
         */
        explicit DictionaryRepository(std::shared_ptr<Common::Database> database);

        /**
         * @brief 分页查询字典类型
         * @param loop 恢复协程用的事件循环
         * @param keyword 关键字
         * @param status 空、0、1
         * @param page 分页
         * @return Common::PageRows<DictTypeRecord> 当前页
         */
        AsynGyanis::Core::Task<Common::PageRows<DictTypeRecord>> listDictTypes(AsynGyanis::Core::EventLoop &loop, std::string keyword, std::string status,
                                                                               const Common::PageParameters &page);

        /**
         * @brief 导出字典类型
         * @param loop 恢复协程用的事件循环
         * @param keyword 关键字
         * @param status 状态
         * @return std::vector<DictTypeRecord> 行
         */
        AsynGyanis::Core::Task<std::vector<DictTypeRecord>> exportDictTypes(AsynGyanis::Core::EventLoop &loop, std::string keyword, std::string status);

        /**
         * @brief 字典类型是否占用
         * @param loop 恢复协程用的事件循环
         * @param type 类型编码
         * @param excludeId 排除项
         * @return bool 是否占用
         */
        AsynGyanis::Core::Task<bool> dictTypeExists(AsynGyanis::Core::EventLoop &loop, std::string type, std::int64_t excludeId = 0);

        /**
         * @brief 创建字典类型
         * @param loop 恢复协程用的事件循环
         * @param write 字段
         * @param operatorId 操作人
         * @return std::int64_t 新标识
         */
        AsynGyanis::Core::Task<std::int64_t> createDictType(AsynGyanis::Core::EventLoop &loop, const DictTypeWrite &write, std::int64_t operatorId);

        /**
         * @brief 更新字典类型
         * @param loop 恢复协程用的事件循环
         * @param typeId 类型标识
         * @param write 字段
         * @param operatorId 操作人
         * @return bool 是否命中一行
         */
        AsynGyanis::Core::Task<bool> updateDictType(AsynGyanis::Core::EventLoop &loop, std::int64_t typeId, const DictTypeWrite &write, std::int64_t operatorId);

        /**
         * @brief 删除字典类型及其数据
         * @param loop 恢复协程用的事件循环
         * @param typeId 类型标识
         * @return bool 是否命中一行
         */
        AsynGyanis::Core::Task<bool> removeDictType(AsynGyanis::Core::EventLoop &loop, std::int64_t typeId);

        /**
         * @brief 取某字典的全部数据
         * @param loop 恢复协程用的事件循环
         * @param type 字典类型
         * @return std::vector<DictDataRecord> 数据
         */
        AsynGyanis::Core::Task<std::vector<DictDataRecord>> dictData(AsynGyanis::Core::EventLoop &loop, std::string type);

        /**
         * @brief 取某字典的启用选项
         * @param loop 恢复协程用的事件循环
         * @param type 字典类型
         * @return std::vector<DictDataRecord> 选项
         */
        AsynGyanis::Core::Task<std::vector<DictDataRecord>> dictOptions(AsynGyanis::Core::EventLoop &loop, std::string type);

        /**
         * @brief 创建字典数据
         * @param loop 恢复协程用的事件循环
         * @param write 字段
         * @param operatorId 操作人
         * @return std::int64_t 新标识
         */
        AsynGyanis::Core::Task<std::int64_t> createDictData(AsynGyanis::Core::EventLoop &loop, const DictDataWrite &write, std::int64_t operatorId);

        /**
         * @brief 更新字典数据
         * @param loop 恢复协程用的事件循环
         * @param dataId 数据标识
         * @param write 字段
         * @param operatorId 操作人
         * @return bool 是否命中一行
         */
        AsynGyanis::Core::Task<bool> updateDictData(AsynGyanis::Core::EventLoop &loop, std::int64_t dataId, const DictDataWrite &write, std::int64_t operatorId);

        /**
         * @brief 删除字典数据
         * @param loop 恢复协程用的事件循环
         * @param dataId 数据标识
         * @return bool 是否命中一行
         */
        AsynGyanis::Core::Task<bool> removeDictData(AsynGyanis::Core::EventLoop &loop, std::int64_t dataId);

        /**
         * @brief 分页查询参数
         * @param loop 恢复协程用的事件循环
         * @param keyword 关键字
         * @param page 分页
         * @return Common::PageRows<ConfigRecord> 当前页
         */
        AsynGyanis::Core::Task<Common::PageRows<ConfigRecord>> listConfigs(AsynGyanis::Core::EventLoop &loop, std::string keyword, const Common::PageParameters &page);

        /**
         * @brief 导出参数
         * @param loop 恢复协程用的事件循环
         * @param keyword 关键字
         * @return std::vector<ConfigRecord> 行
         */
        AsynGyanis::Core::Task<std::vector<ConfigRecord>> exportConfigs(AsynGyanis::Core::EventLoop &loop, std::string keyword);

        /**
         * @brief 参数键名是否占用
         * @param loop 恢复协程用的事件循环
         * @param key 键名
         * @param excludeId 排除项
         * @return bool 是否占用
         */
        AsynGyanis::Core::Task<bool> configKeyExists(AsynGyanis::Core::EventLoop &loop, std::string key, std::int64_t excludeId = 0);

        /**
         * @brief 创建参数
         * @param loop 恢复协程用的事件循环
         * @param write 字段
         * @param operatorId 操作人
         * @return std::int64_t 新标识
         */
        AsynGyanis::Core::Task<std::int64_t> createConfig(AsynGyanis::Core::EventLoop &loop, const ConfigWrite &write, std::int64_t operatorId);

        /**
         * @brief 更新参数
         * @param loop 恢复协程用的事件循环
         * @param configId 参数标识
         * @param write 字段
         * @param operatorId 操作人
         * @return bool 是否命中一行
         */
        AsynGyanis::Core::Task<bool> updateConfig(AsynGyanis::Core::EventLoop &loop, std::int64_t configId, const ConfigWrite &write, std::int64_t operatorId);

        /**
         * @brief 删除参数
         * @param loop 恢复协程用的事件循环
         * @param configId 参数标识
         * @return bool 是否命中一行
         */
        AsynGyanis::Core::Task<bool> removeConfig(AsynGyanis::Core::EventLoop &loop, std::int64_t configId);

        /**
         * @brief 按标识查参数
         * @param loop 恢复协程用的事件循环
         * @param configId 参数标识
         * @return std::optional<ConfigRecord> 参数
         */
        AsynGyanis::Core::Task<std::optional<ConfigRecord>> findConfig(AsynGyanis::Core::EventLoop &loop, std::int64_t configId);

    private:
        std::shared_ptr<Common::Database> m_database; ///< 数据库封装
    };
} // namespace EdgePulse::System
