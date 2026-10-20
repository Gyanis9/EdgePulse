/**
 * @file NodeJson.h
 * @brief 节点行到 API JSON 的映射
 * @author Gyanis
 * @date 2026-10-20
 * @version 0.1.0
 * @copyright Copyright (c) . All rights reserved.
 */

#pragma once

#include "Base/Config/ConfigValue.h"
#include "Core/Device/NodeRepository.h"

namespace EdgePulse::Device
{
    /**
     * @brief 把节点转成前端消费的形状
     * @param node 节点行
     * @return {id,name,status,agentVersion,certFingerprint,labels,lastSeen,version}
     */
    [[nodiscard]] AsynGyanis::Base::ConfigValue nodeToJson(const NodeRecord &node);
} // namespace EdgePulse::Device
