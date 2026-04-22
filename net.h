#ifndef NET_H
#define NET_H

#include <sys/types.h>
#include "msg_protocol.h"

#define DECL_SEND_FN(name, type)                         \
int send_##name(int fd,                                  \
                uint8_t sender_id,                       \
                uint8_t target_id,                       \
                const type *name)

int send_protocol_message(int fd,
    uint8_t msg_type,
    uint8_t sender_id,
    uint8_t target_id,
    size_t payload_len,
    const void *payload);

DECL_SEND_FN(map_message, msg_map_t);
DECL_SEND_FN(move_attempt, msg_move_attempt_t);
DECL_SEND_FN(moved, msg_moved_t);
DECL_SEND_FN(bomb_attempt, msg_bomb_attempt_t);
DECL_SEND_FN(bomb, msg_bomb_t);
DECL_SEND_FN(explosion_start, msg_explosion_start_t);
DECL_SEND_FN(explosion_end, msg_explosion_end_t);
DECL_SEND_FN(bonus_available, msg_bonus_available_t);
DECL_SEND_FN(bonus_retrieved, msg_bonus_retrieved_t);
DECL_SEND_FN(block_destroyed, msg_block_destroyed_t);

int send_ping_message(int fd,
    uint8_t sender_id,
    uint8_t target_id);

int recv_protocol_message(int fd,
    msg_generic_t *header,
    void **payload,
    size_t *payload_len);

#endif
