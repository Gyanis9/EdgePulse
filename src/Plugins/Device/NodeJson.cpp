#include "Plugins/Device/NodeJson.h"

#include <optional>
#include <utility>

namespace EdgePulse::Device
{
    AsynGyanis::Base::ConfigValue nodeToJson(const NodeRecord &node)
    {
        AsynGyanis::Base::ConfigValue labels = nullptr;
        if (!node.labelsJson.empty())
        {
            // 非法 JSON 不伪造对象：回 null，让前端显示“无标签”而不是崩在解析上
            if (std::optional<AsynGyanis::Base::ConfigValue> parsed = AsynGyanis::Base::parseConfigValue(node.labelsJson); parsed.has_value() && parsed->is_object())
            {
                labels = std::move(*parsed);
            }
        }
        return AsynGyanis::Base::ConfigValue::object({{"id", node.id},
                                                      {"tenantId", node.tenantId},
                                                      {"name", node.name},
                                                      {"status", node.status},
                                                      {"agentVersion", node.agentVersion},
                                                      {"installedFirmwareVersion", node.installedFirmwareVersion},
                                                      {"certFingerprint", node.certFingerprint},
                                                      {"certNotAfter", node.certNotAfter},
                                                      {"certRevokedAt", node.certRevokedAt},
                                                      {"previousCertFingerprint", node.previousCertFingerprint},
                                                      {"previousCertNotAfter", node.previousCertNotAfter},
                                                      {"labels", std::move(labels)},
                                                      {"lastSeen", node.lastSeen},
                                                      {"groupId", node.groupId},
                                                      {"groupName", node.groupName},
                                                      {"version", node.version}});
    }
} // namespace EdgePulse::Device
