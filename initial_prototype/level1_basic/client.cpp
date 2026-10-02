// LEVEL 1: Simple TCP client to talk to server.cpp
// Type a message, hit enter, see it echoed back. Type "quit" to exit.

#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 8080
#define BUFFER_SIZE 1024

int main() {
    // 1. socket() - create the client's endpoint
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket failed");
        return 1;
    }

    // 2. Build the server address to connect to
    sockaddr_in server_address{};
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(PORT);

    // Change this IP if connecting to a server on a different machine.
    // 127.0.0.1 = localhost (same machine)
    if (inet_pton(AF_INET, "127.0.0.1", &server_address.sin_addr) <= 0) {
        std::cerr << "Invalid address\n";
        return 1;
    }

    // 3. connect() - establish the connection to the server
    if (connect(sock, (struct sockaddr*)&server_address, sizeof(server_address)) < 0) {
        perror("connect failed");
        return 1;
    }

    std::cout << "Connected to server. Type a message (or 'quit' to exit):\n";

    char buffer[BUFFER_SIZE];
    std::string input;

    while (true) {
        std::cout << "> ";
        std::getline(std::cin, input);

        if (input == "quit") break;

        // 4. send() - push our message to the server
        send(sock, input.c_str(), input.size(), 0);

        // 5. recv() - wait for the echoed response
        memset(buffer, 0, BUFFER_SIZE);
        ssize_t bytes_read = recv(sock, buffer, BUFFER_SIZE - 1, 0);
        if (bytes_read <= 0) {
            std::cout << "Server closed the connection.\n";
            break;
        }

        std::cout << "Echo: " << buffer << "\n";
    }

    close(sock);
    return 0;
}
