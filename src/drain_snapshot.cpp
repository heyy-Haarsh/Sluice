#include "drain_snapshot.hpp"
#include "config.hpp"       // Route, Backend

void DrainSnapshot::update(const std::list<Route>& draining_routes) {
    // Build snapshot entirely on the relay thread (no lock held during
    // the loop — all reads are either stable strings/ints or atomic loads).
    std::vector<DrainEntry> snap;
    snap.reserve(draining_routes.size());
    for (const Route& r : draining_routes) {
        DrainEntry e;
        e.name        = r.name;
        e.listen_port = r.listen_port;
        for (const Backend& b : r.backends) {
            DrainEntry::Be be;
            be.host               = b.host;
            be.port               = b.port;
            be.active_connections = b.active_connections.load(std::memory_order_relaxed);
            e.backends.push_back(std::move(be));
        }
        snap.push_back(std::move(e));
    }
    // Hold the lock only for the swap — a few nanoseconds.
    std::lock_guard<std::mutex> lock(mtx_);
    snap_ = std::move(snap);
}

std::vector<DrainEntry> DrainSnapshot::read() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return snap_;
}
