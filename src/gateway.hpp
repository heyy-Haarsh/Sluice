#pragma once
#include "config.hpp"

// Runs the gateway event loop. Takes the config by value, owns it (routes carry
// mutable runtime state). config_path is used by the dashboard's save-config feature.
// Blocks until SIGINT/SIGTERM; throws std::runtime_error on a fatal setup error.
void run_gateway(GatewayConfig config, const std::string& config_path = "");
