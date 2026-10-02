#include "log_buffer.hpp"

LogBuffer::LogBuffer(size_t cap) : cap_(cap > 0 ? cap : 1) {}

void LogBuffer::push(const std::string& line) {
    std::lock_guard<std::mutex> lock(mtx_);
    buf_.push_back(line);
    if (buf_.size() > cap_) {
        buf_.pop_front();
    }
}

std::vector<std::string> LogBuffer::snapshot() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return std::vector<std::string>(buf_.begin(), buf_.end());
}
