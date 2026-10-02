// LEVEL 3: Multi-client, multi-service/server environment
// A gateway server which will route between the clients and the servers

#include <iostream>
#include <cstring>
#include <unordered_map>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>

#define BUFFER_SIZE 1024
#define MAX_EVENTS 64


// Makes a socket non-blocking
void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int main() {
    return 0;
}