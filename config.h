#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_PLAYERS 8
#define MAX_NAME_LEN 30
#define TICKS_PER_SECOND 20
#define SERVER_PORT 6969

typedef enum {
    GAME_LOBBY = 0,
    GAME_RUNNING = 1,
    GAME_END = 2
} game_status_t;

typedef enum {
    DIR_UP = 0,
    DIR_DOWN = 1,
    DIR_LEFT = 2,
    DIR_RIGHT = 3
} direction_t;

typedef enum {
    BONUS_NONE = 0,
    BONUS_SPEED = 1,
    BONUS_RADIUS = 2,
    BONUS_TIMER = 3
} bonus_type_t;

typedef enum {
    MSG_HELLO           = 0,
    MSG_WELCOME         = 1,
    MSG_DISCONNECT      = 2,
    MSG_PING            = 3,
    MSG_PONG            = 4,
    MSG_LEAVE           = 5,
    MSG_ERROR           = 6,
    MSG_SET_READY       = 10,
    MSG_SET_STATUS      = 20,
    MSG_WINNER          = 23,
    MSG_MOVE_ATTEMPT    = 30,
    MSG_BOMB_ATTEMPT    = 31,
    MSG_MOVED           = 40,
    MSG_BOMB            = 41,
    MSG_EXPLOSION_START = 42,
    MSG_EXPLOSION_END   = 43,
    MSG_DEATH           = 44,
    MSG_BONUS_AVAILABLE = 45,
    MSG_BONUS_RETRIEVED = 46,
    MSG_BLOCK_DESTROYED = 47,
    // MSG_MAP             = 50,
    MSG_SYNC_BOARD      = 100,/* reply contains the map */
    MSG_SYNC_REQUEST    = 101 /* reply with SYNC_BOARD */
} msg_type_t;


typedef struct {
    uint8_t id;
    char name[MAX_NAME_LEN + 1];
    uint16_t row;
    uint16_t col;
    bool alive;
    bool ready;

    uint8_t bomb_count;
    uint8_t bomb_radius;
    uint16_t bomb_timer_ticks;
    uint16_t speed;
} player_t;

typedef struct {
    bool active;
    uint8_t owner_id;
    uint16_t row;
    uint16_t col;
    uint8_t radius;
    uint16_t timer_ticks;
} bomb_t;

typedef struct {
    uint8_t height;
    uint8_t width;
    uint8_t *cells;
} GameMap;


static inline uint16_t make_cell_index(
                        uint16_t row, 
                        uint16_t col, 
                        uint16_t cols) {
    return row * cols + col;
}

#endif /* CONFIG_H */
