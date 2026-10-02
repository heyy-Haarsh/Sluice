#pragma once
#include <list>
#include <mutex>
#include <string>
#include <vector>

struct Route;  // defined in config.hpp; included by drain_snapshot.cpp

// Snapshot of one draining backend — active_connections is copied from the
// atomic at update-time (relay thread), so it may lag up to one refresh cycle.
struct DrainEntry {
    struct Be {
        std::string host;
        int port = 0;
        int active_connections = 0;
    };
    std::string name;
    int listen_port = 0;
    std::vector<Be> backends;
};

// Thread-safe snapshot of GatewayState::draining_routes.
//
// Update discipline (relay thread only):
//   call update() after retire_route() and after erasing backends in cleanup_connection().
//
// Read discipline (dashboard thread only):
//   call read() — takes the lock, copies, returns.
//
// The lock is held only long enough to copy a small struct; no IO, no blocking.
class DrainSnapshot {
public:
    void update(const std::list<Route>& draining_routes);
    std::vector<DrainEntry> read() const;

private:
    mutable std::mutex        mtx_;
    std::vector<DrainEntry>   snap_;
};
