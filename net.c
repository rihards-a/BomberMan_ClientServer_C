#include <sys/socket.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>

#include "net.h"
#include "msg_protocol.h"
#include "config.h"

#define RECV_FIXED_CASE(MSG_ID, TYPE) \
    case MSG_ID: \
        return recv_fixed_message(fd, MSG_ID, payload, payload_len, sizeof(TYPE))

#define DEFINE_SEND_FN(name, msg_const, type)                              \
int send_##name(int fd,                                                    \
                uint8_t sender_id,                                         \
                uint8_t target_id,                                         \
                const type *payload)                                       \
{                                                                          \
    type payload_be = *payload;                                            \
    convert_to_be(msg_const, &payload_be);                                 \
    return send_protocol_message(                                          \
        fd,                                                                \
        msg_const,                                                         \
        sender_id,                                                         \
        target_id,                                                         \
        sizeof(*payload),                                                  \
        &payload_be                                                        \
    );                                                                     \
}

static ssize_t send_all(int fd, const void *buf, size_t len)
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

static ssize_t recv_all(int fd, void *buf, size_t len)
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

/* wrapper for recv_all for easier error handling, probably can remove later */
static int recv_exact(int fd, void *buf, size_t len)
{
    ssize_t n = recv_all(fd, buf, len);

    if (n < 0)
        return -1;
    if ((size_t)n != len)
        return 1; /* peer closed before full message */
    return 0;
}

int send_protocol_message(int fd,
                        uint8_t msg_type,
                        uint8_t sender_id,
                        uint8_t target_id,
                        uint16_t payload_len,
                        const void *payload)
{
    /* these do not need endian conversion since 
        computers work in bytes and we only have uint8_t fields.
        The payload_len field does not get sent over. */
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


/* --------------------------------------------------------------------- */
/*             beginning of converting payload endianness                */
/* --------------------------------------------------------------------- */

/* convert from big endian (network layer) to 
    host machine (likely little endian) */
int convert_from_be(uint8_t msg_type, void *payload) {
    switch (msg_type) {
        case MSG_HELLO: {
            /* no endianness conversion needed for 
                messages with only char[] fields */
            break;
        }
        case MSG_WELCOME: {
            /* welcome message has no fields that 
                require endianness conversion */
            break;
        }
        case MSG_DISCONNECT:
        case MSG_PING:
        case MSG_PONG:
        case MSG_LEAVE: 
            /* no endianness conversion needed for 
                messages with no payload */
            break;
        case MSG_ERROR: {
            msg_error_t *error_msg = (msg_error_t *)payload;
            error_msg->length = ntohs(error_msg->length);
            break;
        }
        case MSG_SET_READY:
        case MSG_SET_STATUS:
        case MSG_WINNER:
        case MSG_MOVE_ATTEMPT:
        case MSG_BOMB_ATTEMPT: {
            /* no endianness conversion needed for
                uint8_t fields or no payload structs */
            break;
        }
        case MSG_MOVED: {
            msg_moved_t *moved = (msg_moved_t *)payload;
            moved->cell_index = ntohs(moved->cell_index);
            break;
        }
        case MSG_BOMB: {
            msg_bomb_t *bomb = (msg_bomb_t *)payload;
            bomb->cell_index = ntohs(bomb->cell_index);
            break;
        }
        case MSG_EXPLOSION_START: {
            msg_explosion_start_t *expl_start = (msg_explosion_start_t *)payload;
            expl_start->cell_index = ntohs(expl_start->cell_index);
            break;
        }
        case MSG_EXPLOSION_END: {
            msg_explosion_end_t *expl_end = (msg_explosion_end_t *)payload;
            expl_end->cell_index = ntohs(expl_end->cell_index);
            break;
        }
        case MSG_DEATH: {
            /* no endianness conversion needed for uint8 fields */
            break;
        }
        case MSG_BONUS_AVAILABLE: {
            msg_bonus_available_t *bonus_avail = (msg_bonus_available_t *)payload;
            bonus_avail->cell_index = ntohs(bonus_avail->cell_index);
            break;
        }
        case MSG_BONUS_RETRIEVED: {
            msg_bonus_retrieved_t *bonus_retrieved = (msg_bonus_retrieved_t *)payload;
            bonus_retrieved->cell_index = ntohs(bonus_retrieved->cell_index);
            break;
        }
        case MSG_BLOCK_DESTROYED: {
            msg_block_destroyed_t *block_destroyed = (msg_block_destroyed_t *)payload;
            block_destroyed->cell_index = ntohs(block_destroyed->cell_index);
            break;
        }
        default:
            /* for messages with no payload or only uint8_t fields, no conversion needed */
            break;
    }

    return 0;
}

/* convert from host endianness to network endianness (big endian) */
int convert_to_be(uint8_t msg_type, void *payload) {
    switch (msg_type) {
        case MSG_HELLO:
            /* no endianness conversion needed for messages with only char[] fields */
            break;
        case MSG_WELCOME:
            /* welcome message has no fields that require endianness conversion */
            break;
        case MSG_DISCONNECT:
        case MSG_PING:
        case MSG_PONG:
        case MSG_LEAVE:
            /* no endianness conversion needed for messages with no payload */
            break;
        case MSG_ERROR: {
            msg_error_t *error_msg = (msg_error_t *)payload;
            error_msg->length = htons(error_msg->length);
            break;
        }
        case MSG_SET_READY:
        case MSG_SET_STATUS:
        case MSG_WINNER:
        case MSG_MOVE_ATTEMPT:
        case MSG_BOMB_ATTEMPT:
            /* no endianness conversion needed for uint8_t fields or no-payload structs */
            break;
        case MSG_MOVED: {
            msg_moved_t *moved = (msg_moved_t *)payload;
            moved->cell_index = htons(moved->cell_index);
            break;
        }
        case MSG_BOMB: {
            msg_bomb_t *bomb = (msg_bomb_t *)payload;
            bomb->cell_index = htons(bomb->cell_index);
            break;
        }
        case MSG_EXPLOSION_START: {
            msg_explosion_start_t *expl_start = (msg_explosion_start_t *)payload;
            expl_start->cell_index = htons(expl_start->cell_index);
            break;
        }
        case MSG_EXPLOSION_END: {
            msg_explosion_end_t *expl_end = (msg_explosion_end_t *)payload;
            expl_end->cell_index = htons(expl_end->cell_index);
            break;
        }
        case MSG_DEATH: {
            /* no endianness conversion needed for uint8 fields */
            break;
        }
        case MSG_BONUS_AVAILABLE: {
            msg_bonus_available_t *bonus_avail = (msg_bonus_available_t *)payload;
            bonus_avail->cell_index = htons(bonus_avail->cell_index);
            break;
        }
        case MSG_BONUS_RETRIEVED: {
            msg_bonus_retrieved_t *bonus_retrieved = (msg_bonus_retrieved_t *)payload;
            bonus_retrieved->cell_index = htons(bonus_retrieved->cell_index);
            break;
        }
        case MSG_BLOCK_DESTROYED: {
            msg_block_destroyed_t *block_destroyed = (msg_block_destroyed_t *)payload;
            block_destroyed->cell_index = htons(block_destroyed->cell_index);
            break;
        }
        default:
            break;
    }

    return 0;
}

/* --------------------------------------------------------------------- */
/*              beginning of sending different prot msgs                 */
/* --------------------------------------------------------------------- */

int send_map_message(int fd, 
    uint8_t sender_id, 
    uint8_t target_id, 
    const msg_map_t *map)
{
    uint16_t cells_len = map->height * map->width;
    uint16_t map_len = sizeof(*map) + cells_len;

    return send_protocol_message(fd,
        MSG_MAP,
        sender_id,
        target_id,
        map_len,
        map); /* cells consist of bytes - no endianness conversion necessary */
}

int send_choose_map(int fd, 
    uint8_t sender_id, 
    uint8_t target_id, 
    const msg_choose_map_t *map_name)
{
    uint16_t map_len = sizeof(*map_name) + map_name->length;
    return send_protocol_message(fd,
        MSG_CHOOSE_MAP,
        sender_id,
        target_id,
        map_len,
        map_name); /* cells consist of bytes - no endianness conversion necessary */
}

int send_ping_message(int fd, 
    uint8_t sender_id, 
    uint8_t target_id)
{
    return send_protocol_message(fd, 
        MSG_PING, 
        sender_id, 
        target_id, 
        0, 
        NULL);
}

int send_disconnect(int fd, 
    uint8_t sender_id, 
    uint8_t target_id)
{
    return send_protocol_message(fd, 
        MSG_DISCONNECT, 
        sender_id, 
        target_id, 
        0, 
        NULL);
}

int send_leave_message(int fd, 
    uint8_t sender_id, 
    uint8_t target_id)
{
    return send_protocol_message(fd, 
        MSG_LEAVE, 
        sender_id, 
        target_id, 
        0, 
        NULL);
}

int send_ready_message(int fd, 
    uint8_t sender_id, 
    uint8_t target_id)
{
    return send_protocol_message(fd, 
        MSG_SET_READY, 
        sender_id, 
        target_id, 
        0, 
        NULL);
}

int send_welcome_message(int fd, 
                         uint8_t sender_id, 
                         uint8_t target_id, 
                         const msg_welcome_t *welcome_message) 
{
    size_t welcome_len = sizeof(msg_welcome_t) + welcome_message->length;

    return send_protocol_message(
        fd,
        MSG_WELCOME,
        sender_id,
        target_id,
        welcome_len,
        welcome_message
    );
}

/* --------------------------------------------------------------------- */
DEFINE_SEND_FN(hello, MSG_HELLO, msg_hello_t);
DEFINE_SEND_FN(set_status, MSG_SET_STATUS, msg_set_status_t);
DEFINE_SEND_FN(move_attempt, MSG_MOVE_ATTEMPT, msg_move_attempt_t);
DEFINE_SEND_FN(moved, MSG_MOVED, msg_moved_t);
DEFINE_SEND_FN(bomb_attempt, MSG_BOMB_ATTEMPT, msg_bomb_attempt_t);
DEFINE_SEND_FN(bomb, MSG_BOMB, msg_bomb_t);
DEFINE_SEND_FN(explosion_start, MSG_EXPLOSION_START, msg_explosion_start_t);
DEFINE_SEND_FN(explosion_end, MSG_EXPLOSION_END, msg_explosion_end_t);
DEFINE_SEND_FN(bonus_available, MSG_BONUS_AVAILABLE, msg_bonus_available_t);
DEFINE_SEND_FN(bonus_retrieved, MSG_BONUS_RETRIEVED, msg_bonus_retrieved_t);
DEFINE_SEND_FN(block_destroyed, MSG_BLOCK_DESTROYED, msg_block_destroyed_t);
DEFINE_SEND_FN(player_death, MSG_DEATH, msg_death_t);
DEFINE_SEND_FN(winner, MSG_WINNER, msg_winner_t);

/* --------------------------------------------------------------------- */
/*                      beginning of receiver declaration                */
/* --------------------------------------------------------------------- */

/* for receiving messages of known constant length */
static int recv_fixed_message(int fd, uint8_t msg_type, void **payload, size_t *payload_len, size_t size)
{
    void *msg = malloc(size);
    if (!msg)
        return -1;

    int rc = recv_exact(fd, msg, size);
    if (rc != 0) {
        free(msg);
        return rc;
    }

    convert_from_be(msg_type, msg);

    *payload = msg;
    *payload_len = size;
    return 0;
}

/*
 * Returns:
 *   0   success          – header filled, payload malloc'd (or NULL)
 *   1   peer closed      – clean EOF before a new message began
 *   2   would block      – nothing in the buffer right now, try next tick
 *  -1   error            – I/O error or truncated message mid-stream
 */
 int recv_protocol_message(int fd,
                        msg_generic_t *header,
                        void **payload,
                        size_t *payload_len)
{
    if (!header || !payload || !payload_len) {
        errno = EINVAL;
        return -1;
    }

    *payload     = NULL;
    *payload_len = 0;

    /* --- non-blocking gate: bail if nothing is waiting --- */
    fd_set rfds;                    /* create a set of readable fds */
    FD_ZERO(&rfds);                 /* to clear the set just in case */
    FD_SET(fd, &rfds);              /* add the fd to the set */
    struct timeval tv = { 0, 0 };   /* for how long to wait (0) */
    /* select monitors file descriptors to check if they're ready
        for their corresponding I/O operation, read in this case. */
    int ready = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (ready == 0) return 2;   /* nothing there this tick */
    if (ready < 0)  return -1;  /* select itself failed */
    /* --- non-blocking gate: bail if nothing is waiting --- */

    int rc = recv_exact(fd, header, sizeof(*header));
    if (rc != 0)
        return rc;

    switch (header->msg_type) {
        case MSG_DISCONNECT:
        case MSG_PING:
        case MSG_PONG:
        case MSG_LEAVE:
        case MSG_SET_READY:
            return 0;

    RECV_FIXED_CASE(MSG_HELLO,           msg_hello_t);
    RECV_FIXED_CASE(MSG_SET_STATUS,      msg_set_status_t);
    RECV_FIXED_CASE(MSG_WINNER,          msg_winner_t);
    RECV_FIXED_CASE(MSG_MOVE_ATTEMPT,    msg_move_attempt_t);
    RECV_FIXED_CASE(MSG_MOVED,           msg_moved_t);
    RECV_FIXED_CASE(MSG_BOMB_ATTEMPT,    msg_bomb_attempt_t);
    RECV_FIXED_CASE(MSG_BOMB,            msg_bomb_t);
    RECV_FIXED_CASE(MSG_EXPLOSION_START, msg_explosion_start_t);
    RECV_FIXED_CASE(MSG_EXPLOSION_END,   msg_explosion_end_t);
    RECV_FIXED_CASE(MSG_DEATH,           msg_death_t);
    RECV_FIXED_CASE(MSG_BONUS_AVAILABLE, msg_bonus_available_t);
    RECV_FIXED_CASE(MSG_BONUS_RETRIEVED, msg_bonus_retrieved_t);
    RECV_FIXED_CASE(MSG_BLOCK_DESTROYED, msg_block_destroyed_t);

    case MSG_MAP: {
        msg_map_t prefix;
        if (recv_exact(fd, &prefix, sizeof(prefix)) != 0)
            return -1;

        size_t cells_len = (size_t)prefix.height * (size_t)prefix.width;
        size_t total_len = sizeof(msg_map_t) + cells_len;

        msg_map_t *msg = malloc(total_len);
        if (!msg) return -1;

        msg->height = prefix.height;
        msg->width  = prefix.width;

        if (cells_len > 0) {
            if (recv_exact(fd, msg->cells, cells_len) != 0) {
                free(msg);
                return -1;
            }
        }

        *payload     = msg;
        *payload_len = total_len;
        return 0;
    }

    case MSG_WELCOME: {
        msg_welcome_t prefix;
        if (recv_exact(fd, &prefix, sizeof(prefix)) != 0)
            return -1;

        size_t clients_len = (size_t)prefix.length;
        size_t total_len   = sizeof(msg_welcome_t) + clients_len;

        msg_welcome_t *msg = malloc(total_len);
        if (!msg) return -1;

        memcpy(msg, &prefix, sizeof(prefix));

        if (clients_len > 0) {
            if (recv_exact(fd, msg->clients, clients_len) != 0) {
                free(msg);
                return -1;
            }
        }

        *payload     = msg;
        *payload_len = total_len;
        return 0;
    }

    case MSG_ERROR: {
        uint16_t len;
        if (recv_exact(fd, &len, sizeof(len)) != 0)
            return -1;

        len = ntohs(len); /* convert length from big endian to host endianness */
        size_t total_len = sizeof(msg_error_t) + (size_t)len;
        msg_error_t *msg = malloc(total_len);
        if (!msg) return -1;

        msg->length = len;

        if (len > 0) {
            if (recv_exact(fd, msg->message, len) != 0) {
                free(msg);
                return -1;
            }
        }

        *payload     = msg;
        *payload_len = total_len;
        return 0;
    }

    case MSG_CHOOSE_MAP: {
        uint8_t len;
        if (recv_exact(fd, &len, sizeof(len)) != 0)
            return -1;

        size_t total_len = sizeof(msg_choose_map_t) + (size_t)len;
        msg_choose_map_t *map_choice = malloc(total_len);
        if (!map_choice) return -1;

        map_choice->length = len;

        if (len > 0) {
            if (recv_exact(fd, map_choice->map_name, len) != 0) {
                free(map_choice);
                return -1;
            }
        }

        *payload     = map_choice;
        *payload_len = total_len;
        return 0;
    }

    default:
        errno = EPROTO;
        return -1;
    }
}

