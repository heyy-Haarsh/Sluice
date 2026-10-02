// LEVEL 2: Multi-client TCP echo server using epoll (Linux-only, works in WSL2)
//
// This solves the exact problem Level 1 had: while blocked on one client's
// recv(), no other client could connect or send data. Here, ONE thread
// monitors MANY sockets at once and only acts on the ones that are ready.
//
// Core idea: register every socket (the listening one AND every client one)
// with epoll. Call epoll_wait() to block until ANY of them has activity.
// It tells you exactly which ones are ready - no guessing, no polling loop.

#include <iostream>
#include <cstring>
#include <unordered_map>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>

#define PORT 8080
#define BUFFER_SIZE 1024
#define MAX_EVENTS 64

// Make a socket non-blocking. Essential for epoll: without this, a single
// slow/stuck client could still block the whole event loop on recv()/send().
void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int main() {
    // --- Set up the listening socket, same as Level 1 ---
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind failed");
        return 1;
    }
    if (listen(server_fd, SOMAXCONN) < 0) {
        perror("listen failed");
        return 1;
    }
    set_nonblocking(server_fd);

    // --- Create the epoll instance ---
    // epoll_create1(0) returns a handle representing "the set of fds I'm watching"
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1 failed");
        return 1;
    }

    // Register the listening socket with epoll.
    // EPOLLIN = "notify me when there's data to read" (for server_fd, that
    // means "a new client is trying to connect").
    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = server_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event);

    // Track each connected client's fd -> a per-client buffer, if you need one.
    // (For a pure echo server we don't need much state, but this is where
    // you'd add per-client message buffers for a real protocol.)
    std::unordered_map<int, std::string> client_names;

    epoll_event events[MAX_EVENTS];
    std::cout << "epoll server listening on port " << PORT << " (multi-client)\n";

    while (true) {
        // epoll_wait() BLOCKS until at least one registered fd is ready,
        // or the timeout expires (-1 = wait forever).
        // It returns the COUNT of ready fds, and fills `events[]` with
        // exactly which ones - no scanning through every fd yourself.
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            if (fd == server_fd) {
                // --- The listening socket is "ready to read" = new connection ---
                sockaddr_in client_addr{};
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
                if (client_fd < 0) continue;

                set_nonblocking(client_fd);

                // Register the NEW client socket with epoll too, so future
                // messages from it show up in future epoll_wait() calls.
                epoll_event client_event{};
                client_event.events = EPOLLIN;
                client_event.data.fd = client_fd;
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_event);

                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &client_addr.sin_addr, ip, INET_ADDRSTRLEN);
                client_names[client_fd] = std::string(ip) + ":" + std::to_string(ntohs(client_addr.sin_port));
                std::cout << "New client connected: " << client_names[client_fd]
                          << " (fd=" << client_fd << ")\n";

            } else {
                // --- An existing client socket has data ready ---
                char buffer[BUFFER_SIZE];
                memset(buffer, 0, BUFFER_SIZE);
                ssize_t bytes_read = recv(fd, buffer, BUFFER_SIZE - 1, 0);

                if (bytes_read <= 0) {
                    // Client disconnected (or error) - clean up.
                    std::cout << "Client disconnected: " << client_names[fd] << "\n";
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                    client_names.erase(fd);
                    close(fd);
                } else {
                    std::cout << "[" << client_names[fd] << "] " << buffer;
                    send(fd, buffer, bytes_read, 0);  // echo back to THIS client only
                }
            }
        }
    }

    close(server_fd);
    close(epoll_fd);
    return 0;
}
