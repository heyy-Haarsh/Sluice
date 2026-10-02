#pragma once
#include "config.hpp"
#include "drain_snapshot.hpp"
#include "log_buffer.hpp"
#include <chrono>
#include <list>
#include <mutex>
#include <string>

// Launches the dashboard HTTP server on a detached background thread.
//
// live_routes  — pointer to gs.routes. Its structure (list nodes) is mutated by
//               the relay thread during hot-reload, so reads here must hold
//               *routes_mtx; Backend atomic fields are still read live.
// routes_mtx   — guards structural access to *live_routes (shared with the
//               relay thread's apply_reload / connection cleanup).
// drain_snap   — mutex-guarded snapshot of gs.draining_routes.
// worker_processes, log_file, log_rotation — startup-time snapshots (immutable).
void start_dashboard(
    const std::list<Route>*  live_routes,
    std::mutex*              routes_mtx,
    DrainSnapshot*           drain_snap,
    const DashboardConfig&   dash_cfg,
    LogBuffer*               log_buffer,
    const std::string&       config_path,
    std::chrono::steady_clock::time_point start_time,
    int                      worker_processes,
    const std::string&       log_file,
    const std::string&       log_rotation
);
