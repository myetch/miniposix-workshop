#include "miniposix/pipe.hpp"

#include <cerrno>
#include <fcntl.h>
#include <system_error>
#include <unistd.h>

namespace miniposix {

Pipe Pipe::create() {
    int fds[2];
    // pipe2 creates both descriptors or none: on failure fds is untouched and
    // nothing needs to be closed. O_CLOEXEC keeps the ends out of exec'd children.
    if(::pipe2(fds, O_CLOEXEC) == -1) {
        throw std::system_error(errno, std::system_category(), "pipe2");
    }

    // Everything below is noexcept, so the two descriptors cannot leak.
    Pipe pipe;
    pipe.read_end_.reset(fds[0]);
    pipe.write_end_.reset(fds[1]);
    return pipe;
}

} // namespace miniposix
