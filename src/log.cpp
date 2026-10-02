#include "log.hpp"

#include <cerrno>
#include <string>

#include <unistd.h>

#include "log_buffer.hpp"

using namespace std;

namespace {

int g_log_fd = STDERR_FILENO;
bool g_log_is_pipe = false;  // pipe target => non-blocking, drop-on-full
LogBuffer* g_buf = nullptr;  // dashboard ring (single-process only), or nullptr

}  // namespace

void log_set_fd(int fd) {
    g_log_fd = fd;
    g_log_is_pipe = true;
}

void log_set_buffer(LogBuffer* buf) { g_buf = buf; }

Log::Log() { os << "[" << getpid() << "] "; }

Log::~Log() {
    string msg = os.str();
    if (g_buf) {
        g_buf->push(msg);  // dashboard live-log ring (line without trailing newline)
    }

    string line = msg;
    line.push_back('\n');

    ssize_t r = ::write(g_log_fd, line.data(), line.size());
    if (r >= 0) {
        return;
    }
    if (g_log_is_pipe && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return;  // writer is behind: drop this line rather than block
    }
    if (g_log_is_pipe && errno == EPIPE) {
        // writer process gone: fall back to stderr for the rest of the run
        g_log_fd = STDERR_FILENO;
        g_log_is_pipe = false;
        ssize_t r2 = ::write(STDERR_FILENO, line.data(), line.size());
        (void)r2;
    }
}
