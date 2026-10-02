// LEVEL 1: Basic blocking TCP echo server
// Handles ONE client at a time. This is the "before" picture -
// once you understand this, Level 2 shows why/how we upgrade it.

#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 8080
#define BUFFER_SIZE 1024

int main() {
    // 1. socket() - create an endpoint for communication
    //    AF_INET = IPv4, SOCK_STREAM = TCP
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket failed");
        return 1;
    }

    // 2. setsockopt() - allow reusing the address/port immediately
    //    Without this, restarting the server quickly gives "Address already in use"
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 3. bind() - assign this socket a local address (IP + port)
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;  // listen on all local interfaces
    address.sin_port = htons(PORT);        // host-to-network byte order

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        return 1;
    }

    // 4. listen() - mark the socket as accepting incoming connections
    //    backlog of 5 = up to 5 pending connections queued before we start rejecting
    if (listen(server_fd, 5) < 0) {
        perror("listen failed");
        close(server_fd);
        return 1;
    }

    std::cout << "Server listening on port " << PORT << " (blocking, single client)\n";
    std::cout << "Waiting for a connection...\n";

    while (true) {
        // 5. accept() - BLOCKS until a client connects.
        //    Returns a NEW socket fd dedicated to this client.
        //    The original server_fd keeps listening for future connections.
        sockaddr_in client_address{};
        socklen_t client_len = sizeof(client_address);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_address, &client_len);
        if (client_fd < 0) {
            perror("accept failed");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_address.sin_addr, client_ip, INET_ADDRSTRLEN);
        std::cout << "Client connected: " << client_ip << ":" << ntohs(client_address.sin_port) << "\n";

        // 6. recv()/send() loop - talk to THIS client until they disconnect.
        //    NOTE: while this loop runs, no other client can connect.
        //    This is exactly the limitation Level 2 fixes.
        char buffer[BUFFER_SIZE];
        while (true) {
            memset(buffer, 0, BUFFER_SIZE);
            ssize_t bytes_read = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);

            if (bytes_read <= 0) {
                // 0 = client closed connection gracefully, <0 = error
                std::cout << "Client disconnected.\n";
                break;
            }

            std::cout << "Received: " << buffer;
            send(client_fd, buffer, bytes_read, 0);  // echo it back
        }

        close(client_fd);
    }

    close(server_fd);  // unreachable in this loop, but good practice to show it
    return 0;
}
