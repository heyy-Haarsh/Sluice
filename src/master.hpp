#pragma once

#include "config.hpp"

// Run the master supervisor: fork `worker_processes` workers, each running the
// gateway event loop (with SO_REUSEPORT so the kernel balances connections),
// respawn any worker that dies, and forward shutdown to all of them.
// Blocks until SIGINT/SIGTERM.
void run_master(GatewayConfig config);
