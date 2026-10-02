#pragma once

#include <string>
#include <vector>
#include <deque>
#include <mutex>

class LogBuffer {
public:
    explicit LogBuffer(size_t cap);
    void push(const std::string& line);
    std::vector<std::string> snapshot() const;

private:
    mutable std::mutex mtx_;
    std::deque<std::string> buf_;
    size_t cap_;
};
