#pragma once

#include <sys/types.h>

#include "config.hpp"

// If config.log_file is set, create a pipe, fork a dedicated writer process that
// owns the log file, and redirect this process's logs into the pipe. Also arms
// SIGHUP (on this process) to forward to the writer for log rotation.
// Returns the writer pid, or -1 if file logging is disabled (logs stay on stderr).
pid_t log_writer_start(const GatewayConfig& config);

// Close the write end (so the writer drains and exits) and reap it.
// No-op if writer_pid <= 0.
void log_writer_stop(pid_t writer_pid);
