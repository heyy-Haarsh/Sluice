#pragma once

#include <chrono>
#include <string>

#include <sys/socket.h>   // sockaddr_storage, socklen_t

using namespace std;

struct Backend;  // in config.hpp

// One synthesized UDP "session". UDP is connectionless, so there is no accept()
// and no FIN; we invent this session keyed by the client's source endpoint
// (addr+port) the first time we see a datagram from it, pin it to one backend,
// and keep it until the idle sweep reclaims it. Unlike a TCP Connection there
// are no per-direction byte buffers — datagrams are relayed immediately, never
// queued (a datagram that the socket can't take right now is simply dropped,
// which is exactly UDP's own delivery contract).
struct UdpSession {
    // Raw client source address, kept verbatim so replies sendto() exactly it.
    sockaddr_storage client_addr{};
    socklen_t        client_len = 0;
    string           client_ip;         // normalized text, for logs + access keying
    int              client_port = 0;   // for logs + the session key

    int      client_listen_fd = -1;     // the UDP listen fd replies go back out of
    int      backend_fd = -1;           // connected SOCK_DGRAM socket to the backend
    Backend* backend = nullptr;         // for LB counting / stats
    string   route_name;                // for logs

    // Idle timeout resolved once at creation (always > 0 for UDP — floored in
    // config). The periodic sweep destroys sessions idle longer than this.
    int idle_timeout_seconds = 0;
    chrono::steady_clock::time_point last_activity;

    // Per-session datagram rate limit (client->backend direction only), a token
    // bucket with capacity == rate: it refills continuously up to `rate_limit`
    // tokens and each forwarded datagram spends one, so a session can never
    // sustain more than `rate_limit` datagrams/sec with at most ~1s of burst —
    // effectively a leaky bucket. Resolved once at creation, like the timeout
    // above. 0 = unlimited (no bucket accounting).
    int rate_limit_per_second = 0;
    double tokens = 0;                              // fractional tokens currently available
    chrono::steady_clock::time_point bucket_updated; // last time tokens[] was refilled
    bool rate_limit_logged = false;  // edge-trigger: log first throttle only; resets when bucket refills

    // PROXY Protocol v2: when true, each datagram forwarded to the backend is
    // prefixed with a 16-byte (IPv4) or 28-byte (IPv6) binary PROXY v2 header
    // carrying the real client source address. Resolved once at session creation
    // from backend->proxy_protocol. False = forward raw datagrams (default).
    bool proxy_protocol_v2 = false;

    // ---- per-session datagram counters (event-loop thread only, no atomics needed) ----
    uint64_t datagrams_sent     = 0;  // client -> backend forwarded datagrams
    uint64_t datagrams_received = 0;  // backend -> client forwarded datagrams
    uint64_t datagrams_dropped_rate = 0;  // dropped by the per-session token bucket
};
