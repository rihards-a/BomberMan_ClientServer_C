#include "net.h"
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>

ssize_t send_all(int fd, const void *buf, size_t len)
{
    size_t total = 0;
    ssize_t n;

    while (total < len) {
        n = send(fd, (const char *)buf + total, len - total, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return total;
        total += (size_t)n;
    }
    return (ssize_t)total;
}

ssize_t recv_all(int fd, void *buf, size_t len)
{
    size_t total = 0;
    ssize_t n;

    while (total < len) {
        n = recv(fd, (char *)buf + total, len - total, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return total;
        total += (size_t)n;
    }
    return (ssize_t)total;
}

