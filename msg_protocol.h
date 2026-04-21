#ifndef MSG_PROTOCOL_H
#define MSG_PROTOCOL_H

#include <stdint.h>
#include "config.h"

#define CLIENT_ID_LEN 20
#define SERVER_ID_LEN 20
#define PLAYER_NAME_LEN 30

#define TARGET_SERVER    255
#define TARGET_BROADCAST 254

/* no padding between fields, exactly as specified */
#pragma pack(push, 1)

/*
 * Shared header for every message.
 * It is always the first 3 bytes on the wire (as received).
 */
typedef struct {
    uint8_t msg_type;
    uint8_t sender_id;
    uint8_t target_id;   /* 255 = server, 254 = broadcast */
} msg_generic_t;

/*
 * Message 00: HELLO
 * Fields listed by the protocol after the shared header.
 */
typedef struct {
    char client_id[CLIENT_ID_LEN];
    char player_name[PLAYER_NAME_LEN];
} msg_hello_t;

/*
 * Message 01: WELCOME
 * The protocol says:
 *   Server identifier char[20]
 *   Game status uint8_t
 *   Length uint8_t
 *   Other clients (uint8_t, bool, char[30])[]
 *
 * The repeated client entry is represented exactly as one entry struct.
 */
typedef struct {
    uint8_t id;
    uint8_t ready;
    char player_name[PLAYER_NAME_LEN];
} welcome_client_t;

typedef struct {
    char server_id[SERVER_ID_LEN];
    uint8_t game_status;
    uint8_t length;
    welcome_client_t clients[];
} msg_welcome_t;

/*
 * Message 02: DISCONNECT
 * No payload.
 */

/*
 * Message 03: PING
 * No payload.
 */

/*
 * Message 04: PONG
 * No payload.
 */

/*
 * Message 05: LEAVE
 * No payload.
 */

/*
 * Message 06: ERROR
 * The protocol only says: error message char[]
 * That means the payload is just a byte string after the header.
 * No fixed fields exist here.
 */
typedef struct {
    uint8_t length;
    char message[];
} msg_error_t;

/*
 * Message 10: SET_READY
 * No payload.
 */

/*
 * Message 20: SET_STATUS
 * Game status uint8_t
 */
typedef struct {
    uint8_t game_status;
} msg_set_status_t;

/*
 * Message 23: WINNER
 * Winner ID uint8_t
 */
typedef struct {
    uint8_t winner_id;
} msg_winner_t;

/*
 * Message 07: MAP
 * Map size is two uint8_t values: H, W
 * Then H * W bytes of map data follow.
 */
typedef struct {
    uint8_t height;
    uint8_t width;
    uint8_t cells[];
} msg_map_t;

/*
 * Message 30: MOVE_ATTEMPT
 * Movement direction uint8_t
 * The spec says ASCII symbol: U, D, L, R
 */
typedef struct {
    uint8_t direction;
} msg_move_attempt_t;

/*
 * Message 40: MOVED
 * Player ID uint8_t
 * New coordinate uint16_t
 */
typedef struct {
    uint8_t player_id;
    uint16_t cell_index;
} msg_moved_t;

/*
 * Message 31: BOMB_ATTEMPT
 * Bomb cell uint16_t
 */
typedef struct {
    uint16_t cell_index;
} msg_bomb_attempt_t;

/*
 * Message 41: BOMB
 * Player ID uint8_t
 * Bomb cell uint16_t
 */
typedef struct {
    uint8_t player_id;
    uint16_t cell_index;
} msg_bomb_t;

/*
 * Message 42: EXPLOSION_START
 * Radius uint8_t
 * Bomb cell uint16_t
 */
typedef struct {
    uint8_t radius;
    uint16_t cell_index;
} msg_explosion_start_t;

/*
 * Message 43: EXPLOSION_END
 * Bomb cell uint16_t
 */
typedef struct {
    uint8_t radius;
    uint16_t cell_index;
} msg_explosion_end_t;

/*
 * Message 44: DEATH
 * Player ID uint8_t
 */
typedef struct {
    uint8_t player_id;
} msg_death_t;

/*
 * Message 45: BONUS_AVAILABLE
 * Bonus type uint8_t
 * Bonus cell uint16_t
 */
typedef struct {
    uint8_t bonus_type;
    uint16_t cell_index;
} msg_bonus_available_t;

/*
 * Message 46: BONUS_RETRIEVED
 * Player ID uint8_t
 * Bonus cell uint16_t
 */
typedef struct {
    uint8_t player_id;
    uint16_t cell_index;
} msg_bonus_retrieved_t;

/*
 * Message 47: BLOCK_DESTROYED
 * Block cell uint16_t
 */
typedef struct {
    uint16_t cell_index;
} msg_block_destroyed_t;

#pragma pack(pop)

#endif
