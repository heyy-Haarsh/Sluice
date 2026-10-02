#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <string>

using namespace std;

struct Backend;  // in config.hpp

// One live client<->backend pair. Created on accept, both fds closed together.
struct Connection {
    int client_fd;
    int backend_fd;
    bool backend_connected = false;  // false while connect() is still pending
    string route_name;               // for logging
    Backend* backend = nullptr;      // which backend this pair uses (for LB counting)

    // Idle timeout: resolved once at creation from the route/global config
    // (0 = disabled). last_activity advances on every byte read from either
    // side; a periodic sweep closes pairs that have been silent too long.
    int idle_timeout_seconds = 0;
    chrono::steady_clock::time_point last_activity;

    // PROXY protocol v1 header to emit as the first bytes on the backend stream
    // (empty = backend isn't configured for it). Built at accept, consumed once
    // the backend connection is established.
    string proxy_header;

    // Bytes read from one side but not yet written to the other. Unbounded for now.
    deque<char> client_to_backend_buffer;  // client bytes waiting for backend
    deque<char> backend_to_client_buffer;  // backend bytes waiting for client

    // Half-close state
    bool client_eof = false;    // read EOF on client_fd (client sent FIN)
    bool backend_eof = false;   // read EOF on backend_fd (backend sent FIN)
    bool client_shut = false;   // called shutdown(client_fd, SHUT_WR)
    bool backend_shut = false;  // called shutdown(backend_fd, SHUT_WR)

    // Current epoll interest mask per fd, so we only MOD when it changes
    uint32_t client_events = 0;
    uint32_t backend_events = 0;
};
