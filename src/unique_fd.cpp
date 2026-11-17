#include "miniposix/unique_fd.hpp"

#include <unistd.h>

#include <utility>

namespace miniposix {

UniqueFd::UniqueFd(int fd) noexcept : fd_(fd) {}

UniqueFd::~UniqueFd() noexcept {
    // close() errors cannot be reported from a noexcept destructor. On Linux
    // the descriptor is released even when close() fails, so never retry.
    if(fd_ != -1) ::close(fd_);
}

UniqueFd::UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept {
    // Self-move is safe: release() empties *this first, so reset() has
    // nothing to close and re-adopts the same descriptor.
    reset(other.release());
    return *this;
}

int UniqueFd::get() const noexcept {
    return fd_;
}

int UniqueFd::release() noexcept {
    return std::exchange(fd_, -1);
}

void UniqueFd::reset(int fd) noexcept {
    if(fd == fd_) return; // reset(get()) is a no-op
    // Swap first so *this never holds a descriptor that has been closed.
    const int old = std::exchange(fd_, fd);
    if(old != -1) ::close(old);
}

} // namespace miniposix
