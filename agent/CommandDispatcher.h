/**
 * @file CommandDispatcher.h
 * @brief 命令去重、执行与结果上报
 * @author Gyanis
 * @date 2026-10-24
 * @version 1.0.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Executors/CommandExecutor.h"

#include "Core/Coroutine/Task.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace EdgePulse::Agent
{
    class AgentClient;
    class AgentState;

    /**
     * @brief 一次结果队列上报的结论
     */
    struct ResultFlush
    {
        std::size_t delivered{0};  ///< 已被服务端确认并删除的条数
        bool        failed{false}; ///< 有一条结果未确认，队列保留待重试
        bool        busy{false};   ///< 已有一次上报在进行，本次没有执行
    };

    /**
     * @brief 把服务端命令分发给执行器，并把结果留在本地队列直到服务端确认
     * @details 所有入口都是协程，并且只在事件循环线程上运行；同一命令标识只执行一次，
     *          判据是正在执行集合、已执行集合与状态库里已有的结果行。命令按到达顺序串行执行：
     *          WebSocket 推送、心跳补拉与轮询兜底可能同时交来命令，后到的会排队，不会并发执行。
     */
    class CommandDispatcher
    {
    public:
        /// 结果入队并尝试上报之后执行的服务重启动作
        using RestartHandler = std::function<AsynGyanis::Core::Task<>()>;

        /**
         * @brief 装配命令分发
         * @param client 与服务端通信的客户端
         * @param state 本地状态库
         * @param executors 命令执行器；类型没有对应执行器时按失败上报
         * @param restartHandler 结果入队与上报尝试之后执行的重启处理器；为空表示不支持重启
         */
        CommandDispatcher(AgentClient &client, AgentState &state, std::vector<std::shared_ptr<CommandExecutor>> executors, RestartHandler restartHandler = {});

        /**
         * @brief 处理一条来自 WebSocket 的命令
         * @param command 已解析的命令
         * @details 重复投递只记日志并跳过；已有命令在执行时按到达顺序排队，等当前命令上报完成后
         *          再执行。执行结果入队后立即尝试上报一次。
         *          结果标记要求重启时，在上报尝试之后调用重启处理器，队列里剩下的命令不再执行。
         */
        AsynGyanis::Core::Task<> onWebSocketCommand(AgentCommand command);

        /**
         * @brief 上报一条无法解析的命令为失败结果
         * @param commandId 仍可使用的命令标识
         * @param message 中文原因
         */
        AsynGyanis::Core::Task<> failCommand(std::int64_t commandId, std::string message);

        /**
         * @brief 拉取一条待执行命令并处理
         * @details 空响应只结束本轮；解析失败时，标识可用的按失败上报，否则记日志跳过。
         *          若拉取期间已有命令开始执行，本次取回的命令会进入等待队列，当前命令结束后再运行。
         */
        AsynGyanis::Core::Task<> pollOnce();

        /**
         * @brief 是否已有命令在执行或等待执行
         * @return bool 有未完成的命令时为 true
         * @note 供轮询循环在命令执行期间跳过拉取；WebSocket 与心跳补拉仍可调用 pollOnce，
         *       取回的命令会排队而不是并发执行
         */
        [[nodiscard]] bool hasInFlight() const;

        /**
         * @brief 把本地结果队列逐条上报，服务端确认后删除
         * @return 本次上报的结论；失败时未确认的行留在队列里等下一轮
         * @note 同一时刻只允许一次上报在跑，重入调用返回 busy
         */
        AsynGyanis::Core::Task<ResultFlush> flushResults();

    private:
        /**
         * @brief 串行执行一条命令，并在结束后继续排空等待队列
         * @param command 已去重的命令
         * @return 协程任务
         */
        AsynGyanis::Core::Task<> runExclusive(AgentCommand command);

        /**
         * @brief 执行一条已去重的命令并把结果写入队列
         * @param command 已解析的命令
         * @return 结果是否要求在本轮上报之后重启服务
         */
        AsynGyanis::Core::Task<bool> execute(AgentCommand command);

        /**
         * @brief 在结果入队并尝试上报之后调用重启处理器
         * @details 处理器抛出的异常就地记 ERROR，不允许带走事件循环
         */
        AsynGyanis::Core::Task<> triggerRestart();

        /**
         * @brief 取状态库里的节点标识
         * @return 有正整数节点标识时返回它；缺失或格式不对时为空
         */
        [[nodiscard]] std::optional<std::int64_t> nodeId();

        /**
         * @brief 把一条失败结果写入本地队列
         * @param commandId 命令标识
         * @param message 中文原因
         */
        void enqueueFailure(std::int64_t commandId, const std::string &message);

        /**
         * @brief 判断命令是否已经执行过
         * @param commandId 命令标识
         * @return bool 正在执行、排队等待、结果仍在队列或已在本次运行内执行完成为 true
         */
        [[nodiscard]] bool alreadyHandled(std::int64_t commandId) const;

        /**
         * @brief 记住一条已执行完的命令
         * @param commandId 命令标识
         * @note 结果行被服务端确认后会从状态库删除；WebSocket 推送与轮询兜底仍可能各交来一份，
         *       因此已执行标识必须在本进程内保留一段时间，不能只靠结果队列去重
         */
        void rememberCompleted(std::int64_t commandId);

        static constexpr std::size_t kCompletedCommandLimit{1024}; ///< 已执行标识的保留条数

        AgentClient                                  &m_client;           ///< 服务端通信客户端
        AgentState                                   &m_state;            ///< 本地状态库
        std::vector<std::shared_ptr<CommandExecutor>> m_executors;        ///< 命令执行器
        RestartHandler                                m_restartHandler;   ///< 结果上报后的重启动作
        std::unordered_set<std::int64_t>              m_inFlight;         ///< 正在执行或等待上报的命令标识
        std::deque<AgentCommand>                      m_pending;          ///< 已有命令执行时按到达顺序排队的命令
        bool                                          m_executing{false}; ///< 是否在串行执行批次里：含上报与队列排空
        // 本次运行内已执行完成的命令标识：结果行被服务端确认后会从状态库删除，
        // 而 WebSocket 推送与轮询兜底仍可能各交来一份，必须靠这份内存记录拦住第二份
        std::unordered_set<std::int64_t> m_completed;
        std::deque<std::int64_t>         m_completedOrder;  ///< 已执行标识的插入顺序，用于有界淘汰
        bool                             m_flushing{false}; ///< 是否有一次结果上报正在进行
    };
} // namespace EdgePulse::Agent
