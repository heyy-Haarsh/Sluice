#include "access.hpp"

#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>

#include "log.hpp"

using namespace std;

namespace {
constexpr auto RATE_WINDOW = chrono::seconds(60);

// Store a network-order IPv4 address as an IPv4-mapped IPv6 address.
void map_v4(const in_addr& a4, array<uint8_t, 16>& out) {
    out.fill(0);
    out[10] = 0xff;
    out[11] = 0xff;
    memcpy(&out[12], &a4.s_addr, 4);
}

// Zero every bit of `b` beyond the first `prefix` bits.
void mask_bytes(array<uint8_t, 16>& b, int prefix) {
    for (int i = 0; i < 16; ++i) {
        int bits = prefix - i * 8;
        if (bits >= 8) continue;                 // whole byte inside the prefix
        if (bits <= 0) { b[i] = 0; continue; }   // whole byte outside the prefix
        b[i] &= static_cast<uint8_t>(0xFF << (8 - bits));
    }
}
}  // namespace

bool parse_ip(const string& s, array<uint8_t, 16>& out) {
    in6_addr a6;
    if (inet_pton(AF_INET6, s.c_str(), &a6) == 1) {
        memcpy(out.data(), &a6, 16);
        return true;
    }
    in_addr a4;
    if (inet_pton(AF_INET, s.c_str(), &a4) == 1) {
        map_v4(a4, out);
        return true;
    }
    return false;
}

bool is_ip(const string& s) {
    array<uint8_t, 16> tmp;
    return parse_ip(s, tmp);
}

string canonical_ip(const string& s) {
    // IPv4 stays dotted-quad (matching inet_ntop on accepted v4 peers); IPv6 is
    // compressed. Try v4 first so "1.2.3.4" isn't emitted in mapped form.
    in_addr a4;
    if (inet_pton(AF_INET, s.c_str(), &a4) == 1) {
        char buf[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &a4, buf, sizeof(buf))) return string(buf);
    }
    in6_addr a6;
    if (inet_pton(AF_INET6, s.c_str(), &a6) == 1) {
        char buf[INET6_ADDRSTRLEN];
        if (inet_ntop(AF_INET6, &a6, buf, sizeof(buf))) return string(buf);
    }
    return s;
}

bool parse_cidr(const string& s, CidrRange& out) {
    auto slash = s.find('/');
    if (slash == string::npos) {
        return false;
    }
    string addr = s.substr(0, slash);
    string prefix_str = s.substr(slash + 1);
    if (prefix_str.empty()) {
        return false;
    }
    for (char c : prefix_str) {
        if (c < '0' || c > '9') return false;
    }

    array<uint8_t, 16> net{};
    int max_bits, base;
    in6_addr a6;
    in_addr a4;
    if (inet_pton(AF_INET6, addr.c_str(), &a6) == 1) {
        memcpy(net.data(), &a6, 16);
        max_bits = 128;
        base = 0;
    } else if (inet_pton(AF_INET, addr.c_str(), &a4) == 1) {
        map_v4(a4, net);
        max_bits = 32;
        base = 96;   // IPv4 lives in the low 32 bits of the mapped form
    } else {
        return false;
    }

    int p = atoi(prefix_str.c_str());
    if (p < 0 || p > max_bits) {
        return false;
    }
    int full = base + p;
    mask_bytes(net, full);
    out.network = net;
    out.prefix_len = full;
    return true;
}

bool cidr_contains(const CidrRange& range, const array<uint8_t, 16>& ip) {
    array<uint8_t, 16> masked = ip;
    mask_bytes(masked, range.prefix_len);
    return masked == range.network;
}

// Applies `list` onto `s`. Safe to call repeatedly on the SAME Scope (startup,
// and again on every hot reload):
//   - whitelist has no dynamic component, so it's cleared and rebuilt wholesale
//     each call — this is what makes a reload actually able to REMOVE a
//     whitelist entry, not just add one (a no-op the first time, since the set
//     starts empty). CIDR whitelist entries behave the same way.
//   - the exact-IP blacklist is merge-only: existing entries are updated in
//     place (get-or-create), never cleared, so an in-progress dynamic
//     rate-limit block and its `violations` count survive a reload untouched.
//     CIDR blacklist entries are always static (a subnet can't accrue a
//     per-client violation count), so — like the whitelist — they're cleared
//     and rebuilt wholesale each call: removing a CIDR blacklist entry via the
//     dashboard and reloading DOES un-block that range, unlike a single-IP
//     blacklist entry.
void AccessControl::seed_static(Scope& s, const AccessList& list,
                                chrono::steady_clock::time_point start) {
    auto sys_now = chrono::system_clock::now();

    s.whitelist.clear();
    s.whitelist_cidrs.clear();
    for (const string& ip : list.whitelist) {
        CidrRange range;
        if (parse_cidr(ip, range)) {
            s.whitelist_cidrs.push_back(range);
        } else {
            // Canonicalize so config text matches the client IP string, which
            // comes from inet_ntop (canonical) — important for IPv6, whose
            // literals have many equivalent spellings.
            s.whitelist.insert(canonical_ip(ip));
        }
    }

    s.blacklist_cidrs.clear();
    for (const BlacklistEntry& e : list.blacklist) {
        // Absolute "until" -> monotonic deadline, recomputed relative to `start`
        // every time (startup or reload), so the block ends at the same
        // wall-clock instant regardless of restarts.
        bool permanent = !e.has_until;
        chrono::steady_clock::time_point expiry = start;
        if (!permanent) {
            auto remaining = e.until - sys_now;
            expiry = (remaining.count() <= 0)
                ? start  // already past -> not blocking
                : start + chrono::duration_cast<chrono::steady_clock::duration>(remaining);
        }
        CidrRange range;
        if (parse_cidr(e.ip, range)) {
            s.blacklist_cidrs.push_back({range, permanent, expiry});
        } else {
            DynBlock& b = s.blacklist[canonical_ip(e.ip)];  // get-or-create: preserves `violations`
            b.permanent = permanent;
            b.expiry = expiry;
        }
    }
}

void AccessControl::init(const GatewayConfig& cfg, const list<Route>& live_routes,
                         chrono::steady_clock::time_point now) {
    backoff_base_ = cfg.rate_limit.backoff_base_seconds;
    backoff_max_ = cfg.rate_limit.backoff_max_seconds;
    int n = cfg.worker_processes > 0 ? cfg.worker_processes : 1;

    // Split each limit across workers so the aggregate ~= the configured value.
    auto per_worker = [n](int limit) -> int {
        if (limit <= 0) return 0;
        int v = limit / n;
        return v < 1 ? 1 : v;
    };

    global_.desc = "global";
    global_.limit = per_worker(cfg.rate_limit.max_per_minute);
    seed_static(global_, cfg.access, now);

    // Reuse an existing Scope (found by Backend* identity) if this backend was
    // already known — this is what lets a hot reload (init() is also called on
    // reload — see gateway.cpp's apply_reload()) survive without wiping a
    // backend's dynamic rate-limit state (sliding-window counters, active
    // blocks, violation counts). A brand-new Backend* (freshly added
    // route/backend) gets a fresh, empty Scope, same as before.
    for (const Route& route : live_routes) {
        for (const Backend& b : route.backends) {
            Scope& s = backend_[&b];  // get-or-create in place
            s.desc = "backend " + b.host + ":" + to_string(b.port);
            s.limit = per_worker(b.rate_max_per_minute);
            seed_static(s, b.access, now);
        }
    }
}

bool AccessControl::blocked(Scope& s, const string& ip,
                            chrono::steady_clock::time_point now) {
    auto it = s.blacklist.find(ip);
    if (it != s.blacklist.end()) {
        const DynBlock& b = it->second;
        // Expired entries stay in the map (to keep the violation count for
        // backoff) but no longer block.
        if (b.permanent || now < b.expiry) {
            return true;
        }
    }
    if (!s.blacklist_cidrs.empty()) {
        array<uint8_t, 16> ip_val;
        if (parse_ip(ip, ip_val)) {
            for (const CidrBlock& cb : s.blacklist_cidrs) {
                if (!cidr_contains(cb.range, ip_val)) continue;
                if (cb.permanent || now < cb.expiry) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool AccessControl::allowed(Scope& s, const string& ip,
                            chrono::steady_clock::time_point now) {
    if (blocked(s, ip, now)) {
        return false;  // explicit deny wins over any whitelist
    }
    if (!s.whitelist.empty() || !s.whitelist_cidrs.empty()) {
        // whitelist mode: only listed IPs / CIDR ranges allowed
        if (s.whitelist.count(ip) != 0) {
            return true;
        }
        array<uint8_t, 16> ip_val;
        if (!s.whitelist_cidrs.empty() && parse_ip(ip, ip_val)) {
            for (const CidrRange& r : s.whitelist_cidrs) {
                if (cidr_contains(r, ip_val)) return true;
            }
        }
        return false;
    }
    return true;
}

int AccessControl::backoff_seconds(int violations) const {
    long secs = backoff_base_;
    for (int i = 1; i < violations && secs < backoff_max_; ++i) {
        secs *= 2;
    }
    if (secs > backoff_max_) {
        secs = backoff_max_;
    }
    return static_cast<int>(secs);
}

void AccessControl::record(Scope& s, const string& ip,
                           chrono::steady_clock::time_point now) {
    if (s.limit <= 0) {
        return;  // no rate limit for this scope
    }
    RateWindow& w = s.rate[ip];
    const auto W = chrono::duration_cast<chrono::steady_clock::duration>(RATE_WINDOW);

    // Advance the window(s). A fresh entry (start == epoch) elapses many windows
    // and just resets. Keeping start on the grid preserves the boundary phase.
    long long n = (now - w.start) / W;
    if (n >= 2) {          // 2+ windows idle: previous window is empty now
        w.prev = 0;
        w.curr = 0;
        w.start = now;
    } else if (n == 1) {   // rolled into the next window
        w.prev = w.curr;
        w.curr = 0;
        w.start += W;
    }
    // n <= 0: still in the current window

    // Weighted estimate of connections in the trailing minute. As we move through
    // the current window (frac 0->1), the previous window's contribution fades out.
    double frac = double((now - w.start).count()) / double(W.count());
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    w.curr++;
    double estimate = w.prev * (1.0 - frac) + w.curr;

    if (estimate > s.limit) {
        DynBlock& b = s.blacklist[ip];
        b.permanent = false;
        b.violations++;
        int secs = backoff_seconds(b.violations);
        b.expiry = now + chrono::seconds(secs);
        Log() << "rate limit exceeded by " << ip << " (" << s.desc
              << "): blocked for " << secs << "s (violation " << b.violations << ")";
        w.prev = 0;  // fresh window after the block lifts
        w.curr = 0;
    }
}

bool AccessControl::allowed_global(const string& ip,
                                   chrono::steady_clock::time_point now) {
    return allowed(global_, ip, now);
}

void AccessControl::record_global(const string& ip,
                                  chrono::steady_clock::time_point now) {
    record(global_, ip, now);
}

bool AccessControl::allowed_backend(const Backend* b, const string& ip,
                                    chrono::steady_clock::time_point now) {
    auto it = backend_.find(b);
    if (it == backend_.end()) {
        return true;  // no scope (shouldn't happen) => no restriction
    }
    return allowed(it->second, ip, now);
}

void AccessControl::record_backend(const Backend* b, const string& ip,
                                   chrono::steady_clock::time_point now) {
    auto it = backend_.find(b);
    if (it != backend_.end()) {
        record(it->second, ip, now);
    }
}
