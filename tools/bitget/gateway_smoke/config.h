#ifndef AQUILA_TOOLS_BITGET_GATEWAY_SMOKE_CONFIG_H_
#define AQUILA_TOOLS_BITGET_GATEWAY_SMOKE_CONFIG_H_

#include <filesystem>

#include "core/common/result.h"
#include "core/config/instrument_catalog.h"
#include "tools/bitget/gateway_smoke/types.h"

namespace aquila::tools::bitget::gateway_smoke {

using GatewaySmokeConfigResult = Result<GatewaySmokeConfig>;

[[nodiscard]] GatewaySmokeConfigResult LoadConfig(
    const std::filesystem::path& path);

[[nodiscard]] Result<bool> ValidateInstrumentContract(
    const GatewaySmokeConfig& config, const config::InstrumentInfo& instrument);

}  // namespace aquila::tools::bitget::gateway_smoke

#endif  // AQUILA_TOOLS_BITGET_GATEWAY_SMOKE_CONFIG_H_
