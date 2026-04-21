#ifndef NET_H
#define NET_H

#include <sys/types.h>
#include "msg_protocol.h"

int send_protocol_message(int fd,
    uint8_t msg_type,
    uint8_t sender_id,
    uint8_t target_id,
    size_t payload_len,
    const void *payload);

int send_map_message(int fd,
    uint8_t sender_id,
    uint8_t target_id,
    const msg_map_t *map);

int send_ping_message(int fd,
    uint8_t sender_id,
    uint8_t target_id);

int send_move_attempt(int fd,
    uint8_t sender_id,
    uint8_t target_id,
    const msg_move_attempt_t *move);

int send_moved(int fd,
    uint8_t sender_id,
    uint8_t target_id,
    const msg_moved_t *moved);

int send_bomb_attempt(int fd, 
    uint8_t sender_id, 
    uint8_t target_id, 
    const msg_bomb_attempt_t *bomb_attempt);

int send_bomb(int fd, 
    uint8_t sender_id, 
    uint8_t target_id, 
    const msg_bomb_t *bomb);

int recv_protocol_message(int fd,
    msg_generic_t *header,
    void **payload,
    size_t *payload_len);

int send_explosion_start(int fd, 
    uint8_t sender_id, 
    uint8_t target_id, 
    const msg_explosion_start_t *expl_start);

int send_explosion_end(int fd, 
    uint8_t sender_id, 
    uint8_t target_id, 
    const msg_explosion_start_t *expl_end);

#endif
