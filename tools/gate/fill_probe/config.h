#ifndef AQUILA_TOOLS_GATE_FILL_PROBE_CONFIG_H_
#define AQUILA_TOOLS_GATE_FILL_PROBE_CONFIG_H_

#include <filesystem>

#include "core/common/result.h"
#include "tools/gate/fill_probe/types.h"

namespace aquila::tools::gate::fill_probe {

using FillProbeConfigResult = Result<FillProbeConfig>;

[[nodiscard]] FillProbeConfigResult LoadConfig(
    const std::filesystem::path& path);

}  // namespace aquila::tools::gate::fill_probe

#endif  // AQUILA_TOOLS_GATE_FILL_PROBE_CONFIG_H_
