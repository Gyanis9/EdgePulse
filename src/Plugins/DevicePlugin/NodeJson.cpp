#include "Plugins/DevicePlugin/NodeJson.h"

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
                                                      {"name", node.name},
                                                      {"status", node.status},
                                                      {"agentVersion", node.agentVersion},
                                                      {"certFingerprint", node.certFingerprint},
                                                      {"labels", std::move(labels)},
                                                      {"lastSeen", node.lastSeen},
                                                      {"version", node.version}});
    }
} // namespace EdgePulse::Device
