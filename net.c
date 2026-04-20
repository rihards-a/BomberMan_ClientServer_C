#include "net.h"
#include <sys/socket.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>

#include "msg_protocol.h"

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

int send_protocol_message(int fd,
                        uint8_t msg_type,
                        uint8_t sender_id,
                        uint8_t target_id,
                        const void *payload,
                        size_t payload_len)
{
    msg_generic_t header = {
        .msg_type = msg_type,
        .sender_id = sender_id,
        .target_id = target_id
    };

    if (send_all(fd, &header, sizeof(header)) < 0)
        return -1;

    if (payload_len == 0)
        return 0;

    if (payload == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (send_all(fd, payload, payload_len) < 0)
        return -1;

    return 0;
}

int send_map_message(int fd,
                    uint8_t msg_type,
                    uint8_t sender_id,
                    uint8_t target_id,
                    const msg_map_t *map)
{
size_t cells_len = (size_t)map->height * (size_t)map->width;
size_t body_len = sizeof(*map) + cells_len;

return send_protocol_message(fd,
                            msg_type,
                            sender_id,
                            target_id,
                            map,
                            body_len);
}

