#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <list>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "config.hpp"

using namespace std;

// ---- IP address / CIDR helpers (IPv4 + IPv6) -------------------------------
// Shared by config.cpp (validation), gateway.cpp (affinity matching), and
// access.cpp (whitelist/blacklist CIDR matching). Every address is normalized
// to a 16-byte big-endian form; IPv4 is stored IPv4-mapped (::ffff:a.b.c.d) so
// v4 and v6 share a single 128-bit matching path.

// Parse an IPv4 or IPv6 literal into 16-byte form. Returns false if malformed.
bool parse_ip(const string& s, array<uint8_t, 16>& out);

// True if `s` is a syntactically valid IPv4 or IPv6 literal.
bool is_ip(const string& s);

// Canonical text form (compresses IPv6, normalizes IPv4). If `s` is not a valid
// IP literal it is returned unchanged.
string canonical_ip(const string& s);

// One CIDR range. `prefix_len` is measured in the 128-bit space, so an IPv4
// "/n" is stored as "/(96 + n)"; `network` is already masked.
struct CidrRange {
    array<uint8_t, 16> network{};
    int prefix_len = 0;
};

// Parse "addr/prefix" (IPv4 "/0-32" or IPv6 "/0-128"). False if malformed.
bool parse_cidr(const string& s, CidrRange& out);

// True if `ip` (from parse_ip) falls within `range`.
bool cidr_contains(const CidrRange& range, const array<uint8_t, 16>& ip);

// Per-worker access control: static whitelist/blacklist enforcement plus per-IP
// rate limiting via a dynamic blacklist with exponential backoff. All state is
// in-memory and per-process (not shared across workers) — under multi-process the
// limits are divided by worker_processes so the aggregate approximates the config.
// Touched only by the event-loop thread, so no locking.
class AccessControl {
public:
    // Build from config. `live_routes` supplies the stable Backend addresses used
    // as keys (call after gs.routes is set). `now` is startup time (for static
    // blacklist timeouts).
    void init(const GatewayConfig& cfg, const list<Route>& live_routes,
              chrono::steady_clock::time_point now);

    // Global gate: false => reject the client outright.
    bool allowed_global(const string& ip, chrono::steady_clock::time_point now);
    // Count one accepted connection from ip globally (may add a backoff block).
    void record_global(const string& ip, chrono::steady_clock::time_point now);

    // Per-backend gate (whitelist + dynamic blacklist).
    bool allowed_backend(const Backend* b, const string& ip,
                         chrono::steady_clock::time_point now);
    // Count one connection routed to backend b (may add a backoff block).
    void record_backend(const Backend* b, const string& ip,
                        chrono::steady_clock::time_point now);

private:
    struct DynBlock {
        bool permanent = false;
        chrono::steady_clock::time_point expiry;  // when the block lifts (if !permanent)
        int violations = 0;                       // rate-limit violation count (for backoff)
    };
    // Sliding-window counter: keep the current and previous fixed windows and
    // weight the previous one by how much of it still overlaps the trailing
    // minute. Smooths the 2x burst a plain fixed window allows at boundaries.
    struct RateWindow {
        chrono::steady_clock::time_point start;  // start of the current window
        int curr = 0;                            // count in the current window
        int prev = 0;                            // count in the previous window
    };
    // A static CIDR blacklist entry. Unlike the exact-IP blacklist, a CIDR range
    // never accrues rate-limit violations (those are always keyed by a single
    // client IP), so it has no dynamic component and is safe to fully rebuild
    // on every seed_static() call — including removals taking effect on reload.
    struct CidrBlock {
        CidrRange range;
        bool permanent = false;
        chrono::steady_clock::time_point expiry;
    };
    struct Scope {
        set<string> whitelist;                     // non-empty (with cidrs) => only these allowed
        vector<CidrRange> whitelist_cidrs;          // CIDR entries from the same whitelist field
        unordered_map<string, DynBlock> blacklist; // static seeds + dynamic blocks (exact IP only)
        vector<CidrBlock> blacklist_cidrs;          // static CIDR blacklist entries
        unordered_map<string, RateWindow> rate;
        int limit = 0;    // effective per-worker limit, 0 = unlimited
        string desc;      // for logs, e.g. "global" or "backend 1.2.3.4:80"
    };

    int backoff_base_ = 60;
    int backoff_max_ = 3600;
    Scope global_;
    unordered_map<const Backend*, Scope> backend_;

    static void seed_static(Scope& s, const AccessList& list,
                            chrono::steady_clock::time_point start);
    bool blocked(Scope& s, const string& ip, chrono::steady_clock::time_point now);
    bool allowed(Scope& s, const string& ip, chrono::steady_clock::time_point now);
    void record(Scope& s, const string& ip, chrono::steady_clock::time_point now);
    int backoff_seconds(int violations) const;
};
