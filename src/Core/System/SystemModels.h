/**
 * @file SystemModels.h
 * @brief 管理域仓储与服务共用的行模型
 * @author Gyanis
 * @date 2026-10-20
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace EdgePulse::System
{
    /**
     * @brief 角色行
     */
    struct RoleRecord
    {
        std::int64_t              id{0};             ///< 主键
        std::string               name;              ///< 角色名
        std::string               code;              ///< 角色码
        std::string               dataScope{"SELF"}; ///< 数据范围编码
        int                       sort{0};           ///< 排序
        int                       status{1};         ///< 1 启用
        bool                      builtin{false};    ///< 内置角色
        std::string               remark;            ///< 备注
        std::int64_t              userCount{0};      ///< 关联用户数
        std::string               createTime;        ///< 创建时间
        std::vector<std::int64_t> menuIds;           ///< 已授权菜单
    };

    /**
     * @brief 部门行
     */
    struct DeptRecord
    {
        std::int64_t            id{0};          ///< 主键
        std::int64_t            parentId{0};    ///< 父部门
        std::string             name;           ///< 部门名
        std::string             leader;         ///< 负责人
        std::string             phone;          ///< 联系电话
        std::string             email;          ///< 邮箱
        int                     sort{0};        ///< 排序
        int                     status{1};      ///< 1 启用
        std::int64_t            memberCount{0}; ///< 直属成员数
        std::string             createTime;     ///< 创建时间
        std::vector<DeptRecord> children;       ///< 子部门
    };

    /**
     * @brief 岗位行
     */
    struct PostRecord
    {
        std::int64_t id{0};        ///< 主键
        std::string  code;         ///< 岗位编码
        std::string  name;         ///< 岗位名
        int          sort{0};      ///< 排序
        int          status{1};    ///< 1 启用
        std::string  remark;       ///< 备注
        std::int64_t userCount{0}; ///< 在岗人数
        std::string  createTime;   ///< 创建时间
    };

    /**
     * @brief 字典类型行
     */
    struct DictTypeRecord
    {
        std::int64_t id{0};      ///< 主键
        std::string  name;       ///< 字典名称
        std::string  type;       ///< 字典类型
        int          status{1};  ///< 1 启用
        std::string  remark;     ///< 备注
        std::string  createTime; ///< 创建时间
    };

    /**
     * @brief 字典数据行
     */
    struct DictDataRecord
    {
        std::int64_t id{0};            ///< 主键
        std::string  dictType;         ///< 所属字典
        std::string  label;            ///< 标签
        std::string  value;            ///< 键值
        int          sort{0};          ///< 排序
        int          status{1};        ///< 1 启用
        std::string  tone;             ///< 前端色板
        bool         isDefault{false}; ///< 是否默认项
        std::string  remark;           ///< 备注
    };

    /**
     * @brief 参数配置行
     */
    struct ConfigRecord
    {
        std::int64_t id{0};          ///< 主键
        std::string  name;           ///< 参数名称
        std::string  key;            ///< 参数键名
        std::string  value;          ///< 参数值
        std::string  type{"N"};      ///< Y 内置、N 普通
        bool         builtin{false}; ///< 是否内置
        std::string  remark;         ///< 备注
        std::string  createTime;     ///< 创建时间
    };

    /**
     * @brief 操作日志行
     */
    struct OperLogRecord
    {
        std::int64_t id{0};       ///< 主键
        std::string  title;       ///< 标题
        std::string  module;      ///< 模块
        std::string  action;      ///< 动作
        std::string  method;      ///< HTTP 方法
        std::string  url;         ///< 请求路径
        std::string  ip;          ///< 来源地址
        std::string  operName;    ///< 操作人
        std::string  deptName;    ///< 部门
        std::int64_t operId{0};   ///< 操作人标识
        std::int64_t deptId{0};   ///< 部门标识
        std::int64_t tenantId{1}; ///< 归属租户
        std::string  params;      ///< 请求参数
        int          result{1};   ///< 1 成功、0 失败
        std::string  errorMsg;    ///< 失败原因
        std::int64_t costMs{0};   ///< 耗时毫秒
        std::string  traceId;     ///< 请求标识
        std::string  operTime;    ///< 操作时间
    };

    /**
     * @brief 登录日志行
     */
    struct LoginLogRecord
    {
        std::int64_t id{0};       ///< 主键
        std::string  username;    ///< 登录名
        std::string  ip;          ///< 来源地址
        int          status{0};   ///< 1 成功、0 失败
        std::string  message;     ///< 说明
        std::string  traceId;     ///< 请求标识
        std::string  loginTime;   ///< 登录时间
        std::string  location;    ///< 归属地
        std::string  browser;     ///< 浏览器
        std::string  os;          ///< 操作系统
        std::int64_t userId{0};   ///< 账号标识
        std::int64_t deptId{0};   ///< 部门标识
        std::int64_t tenantId{1}; ///< 归属租户
    };

    /**
     * @brief 登录统计桶
     */
    struct TimeBucket
    {
        std::string  label;     ///< 横轴标签
        std::int64_t total{0};  ///< 总数
        std::int64_t failed{0}; ///< 失败数
    };

    /**
     * @brief 用户写入字段；创建与编辑共用，未给的字段保持原值
     */
    struct UserWrite
    {
        std::optional<std::string>               username; ///< 登录名
        std::optional<std::string>               nickname; ///< 显示名
        std::optional<std::string>               phone;    ///< 手机号
        std::optional<std::string>               email;    ///< 邮箱
        std::optional<std::int64_t>              deptId;   ///< 部门
        std::optional<int>                       status;   ///< 1 启用
        std::optional<std::vector<std::int64_t>> roleIds;  ///< 角色
        std::optional<std::vector<std::int64_t>> postIds;  ///< 岗位
    };

    /**
     * @brief 角色写入字段
     */
    struct RoleWrite
    {
        std::optional<std::string>               name;      ///< 角色名
        std::optional<std::string>               code;      ///< 角色码
        std::optional<std::string>               dataScope; ///< 数据范围
        std::optional<int>                       sort;      ///< 排序
        std::optional<int>                       status;    ///< 1 启用
        std::optional<std::string>               remark;    ///< 备注
        std::optional<std::vector<std::int64_t>> menuIds;   ///< 菜单授权
        std::optional<std::vector<std::int64_t>> deptIds;   ///< 自定义数据范围的部门
    };

    /**
     * @brief 部门写入字段
     */
    struct DeptWrite
    {
        std::optional<std::int64_t> parentId; ///< 父部门
        std::optional<std::string>  name;     ///< 部门名
        std::optional<std::string>  leader;   ///< 负责人
        std::optional<std::string>  phone;    ///< 电话
        std::optional<std::string>  email;    ///< 邮箱
        std::optional<int>          sort;     ///< 排序
        std::optional<int>          status;   ///< 1 启用
    };

    /**
     * @brief 岗位写入字段
     */
    struct PostWrite
    {
        std::optional<std::string> code;   ///< 编码
        std::optional<std::string> name;   ///< 名称
        std::optional<int>         sort;   ///< 排序
        std::optional<int>         status; ///< 1 启用
        std::optional<std::string> remark; ///< 备注
    };

    /**
     * @brief 字典类型写入字段
     */
    struct DictTypeWrite
    {
        std::optional<std::string> name;   ///< 名称
        std::optional<std::string> type;   ///< 类型
        std::optional<int>         status; ///< 1 启用
        std::optional<std::string> remark; ///< 备注
    };

    /**
     * @brief 字典数据写入字段
     */
    struct DictDataWrite
    {
        std::optional<std::string> type;      ///< 所属字典
        std::optional<std::string> label;     ///< 标签
        std::optional<std::string> value;     ///< 键值
        std::optional<int>         sort;      ///< 排序
        std::optional<int>         status;    ///< 1 启用
        std::optional<std::string> tone;      ///< 色板
        std::optional<bool>        isDefault; ///< 默认项
        std::optional<std::string> remark;    ///< 备注
    };

    /**
     * @brief 参数写入字段
     */
    struct ConfigWrite
    {
        std::optional<std::string> name;    ///< 名称
        std::optional<std::string> key;     ///< 键名
        std::optional<std::string> value;   ///< 值
        std::optional<std::string> type;    ///< Y/N
        std::optional<bool>        builtin; ///< 内置
        std::optional<std::string> remark;  ///< 备注
    };

    /**
     * @brief 菜单写入字段
     */
    struct MenuWrite
    {
        std::optional<std::int64_t> parentId;   ///< 父节点
        std::optional<std::string>  name;       ///< 名称
        std::optional<std::string>  type;       ///< dir/menu/btn
        std::optional<std::string>  icon;       ///< 图标
        std::optional<std::string>  path;       ///< 路径
        std::optional<std::string>  component;  ///< 组件
        std::optional<std::string>  permission; ///< 权限码
        std::optional<int>          sort;       ///< 排序
        std::optional<int>          visible;    ///< 1 显示
        std::optional<int>          status;     ///< 1 启用
    };

    /**
     * @brief 用户列表查询条件
     */
    struct UserQuery
    {
        std::string               keyword;         ///< 账号/姓名/手机号
        std::string               status;          ///< 空、0、1
        std::int64_t              deptId{0};       ///< 部门筛选
        std::vector<std::int64_t> allowedDeptIds;  ///< 数据范围内的部门；空表示全部
        bool                      selfOnly{false}; ///< 仅本人
        std::int64_t              userId{0};       ///< 当前用户
    };

    /**
     * @brief 节点列表的数据范围
     */
    struct NodeScope
    {
        std::vector<std::int64_t> allowedDeptIds;  ///< 可见部门；空表示全部
        bool                      selfOnly{false}; ///< 仅本人创建
        std::int64_t              userId{0};       ///< 当前用户
    };
} // namespace EdgePulse::System
