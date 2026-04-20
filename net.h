#ifndef NET_H
#define NET_H

#include <sys/types.h>
#include "msg_protocol.h"

ssize_t send_all(int fd, const void *buf, size_t len);
ssize_t recv_all(int fd, void *buf, size_t len);

int send_protocol_message(int fd,
    uint8_t msg_type,
    uint8_t sender_id,
    uint8_t target_id,
    const void *payload,
    size_t payload_len);

int send_map_message(int fd,
    uint8_t msg_type,
    uint8_t sender_id,
    uint8_t target_id,
    const msg_map_t *map);
    
#endif
