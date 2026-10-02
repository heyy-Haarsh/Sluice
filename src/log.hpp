#pragma once

#include <sstream>

using namespace std;

class LogBuffer;  // in log_buffer.hpp

// Redirect all Log output to `fd` (the log-writer pipe). After this, a write that
// would block is dropped, and a broken pipe falls back to stderr. Until called,
// logs go to stderr.
void log_set_fd(int fd);

// Also mirror each log line into `buf` (the dashboard's ring). nullptr disables it.
void log_set_buffer(LogBuffer* buf);

// Buffers a whole log line and emits it in ONE atomic write() on destruction,
// prefixed with the pid. A single write() <= PIPE_BUF is atomic, so lines never
// interleave across processes.
// Usage: Log() << "accepted client fd=" << fd;   (newline added automatically)
class Log {
public:
    Log();

    template <class T>
    Log& operator<<(const T& v) {
        os << v;
        return *this;
    }

    ~Log();

private:
    ostringstream os;
};
