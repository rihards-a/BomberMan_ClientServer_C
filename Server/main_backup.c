#include <stdlib.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include <sys/socket.h>
#include <arpa/inet.h>

#include "../net.h"
#include "../config.h"
#include "../msg_protocol.h"

msg_map_t *GAME_MAP;
int CLIENT_FD;

uint8_t is_player_on_bomb[8];
uint8_t bombs_active_for_player[8]; /* > 0 means player has bombs on the map */
uint16_t BOMB_DETONATION_TICKS = 40; /* 1 second, should be set on map init later */
uint8_t player_move_cooldown[8]; /* ticks until player can move again, based on their speed */
uint8_t player_dead[8]; /* 0 = alive, 1 = dead */


void init_players(void) { // maybe put with the other functions
    for (int i = 0; i < MAX_PLAYERS; i++) {
        player_move_cooldown[i] = 0;
        player_dead[i] = 0;
    }
}

typedef struct {
    bomb_t bombs[MAX_BOMBS];
    size_t size;
} BombArray;
static int bomb_array_push(BombArray *a, bomb_t bomb);
static void bomb_array_explode(BombArray *a, size_t i);
static void bomb_array_tick(BombArray *a);
static void decrement_move_cooldown(void);
static inline int is_on_laser(char c);
static void kill_player(uint8_t player_id);

BombArray ACTIVE_BOMBS = { .size = 0 };

/* later create a pool of players and find by sender id or by binding sockets to ids */
player_t test_player = {
    .id = 1,
    .name = "Test Player",
    .row = 0,
    .col = 1,
    .lives = 1,
    .ready = true,
    .bomb_count = 2,
    .bomb_radius = 1,
    .bomb_timer_ticks = 20, /* 1 second */
    .speed = 3
};

player_t players[MAX_PLAYERS];
uint8_t player_count = 0;


static void handle_client_messages(int fd);
int main() {  
    int server_fd;  
    struct sockaddr_in server_addr, client_addr;  


    server_fd = socket(AF_INET, SOCK_STREAM, 0);  
    if (server_fd == -1) {  
        perror("Socket creation failed");  
        return EXIT_FAILURE;  
    }  
    server_addr.sin_family = AF_INET;  
    server_addr.sin_addr.s_addr = INADDR_ANY;  
    server_addr.sin_port = htons(SERVER_PORT);  

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {  
        perror("Bind failed");  
        return EXIT_FAILURE;  
    }  

    listen(server_fd, MAX_PLAYERS);  
    printf("Server listening on port 6969\n");  

    socklen_t addr_len = sizeof(client_addr);


    CLIENT_FD = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);  
    if (CLIENT_FD < 0) {  
        perror("Accept failed");  
        return EXIT_FAILURE;  
    }

    uint8_t new_id = player_count++;
    players[new_id] = (player_t){
        .id             = new_id  + 1,
        .name           = "Test Player " + new_id, /* just for testing, should be set on map init later */
        .row            = 1*new_id +1, // just for testing, should be set on map init later
        .col            = 1*new_id +1,
        .lives          = 1,
        .ready          = true,
        .bomb_count     = 2,
        .bomb_radius    = 1,
        .bomb_timer_ticks = 20,
        .speed          = 3
    };

    /* send the assigned id to the client so it knows who it is */
    send_welcome(CLIENT_FD, new_id);
    printf("Client connected, assigned id %d\n", new_id);


    /* -----------------test-input-map-------------------- */
    uint8_t height, width, c;
    /* make sure you're running it from the right directory (as ./server)*/
    FILE *fp = fopen("maps/test_map_1.txt", "r");
    if (!fp) return 1;

    fscanf(fp, "%hhd %hhd", &height, &width);
    GAME_MAP = malloc(sizeof(GAME_MAP) + height * width);
    GAME_MAP->width = width;
    GAME_MAP->height = height;

    /* skip 4 characters while testing */
    for (int i = 0; i < 4; i++) {
        fscanf(fp, " %c", &c);
    }

    for (int i = 0; i < height * width; i++)
        fscanf(fp, " %c", &GAME_MAP->cells[i]);
    
    fclose(fp);

    usleep(1000000); /* wait ; check if the map will change from client loop */

    /* send MSG_SYNC_BOARD from TARGET_SERVER (255) to everyone (254) - but actually just the CLIENT_FD */
    printf("Sending map message to client...\n");
    if (send_map_message(CLIENT_FD, TARGET_SERVER, TARGET_BROADCAST, GAME_MAP) < 0) {
        perror("Failed to send map message");
        return EXIT_FAILURE;
    }

    init_players();

    /* ------------------ main loop ------------------ */
    while (1) {  
        decrement_move_cooldown();

        handle_client_messages(CLIENT_FD);

        bomb_array_tick(&ACTIVE_BOMBS);

        usleep(1000000 / TICKS_PER_SECOND); /* 1e6 for microseconds */
    }

    return 0;  
}


/* -------------------------- function declarations --------------------------- */

static int bomb_array_push(BombArray *a, bomb_t bomb)
{
    if (a->size >= MAX_BOMBS)
        return -1;
    a->bombs[a->size++] = bomb;
    return 0;
}

static void bomb_array_explode(BombArray *a, size_t i)
{
    uint16_t cell_index = make_cell_index(a->bombs[i].row, a->bombs[i].col, GAME_MAP->width);
    if (!a->bombs[i].active) {
        a->bombs[i].active = true;
        /* find the player by owner_id, but for now we have constant tester 
            uint8_t owner_id = a->bombs[i].owner_id;
            +1 added because the while loop that ticks these bombs decrements 1 on check */
        a->bombs[i].timer_ticks = test_player.bomb_timer_ticks + 1; 

        GAME_MAP->cells[cell_index] = '@';

        uint8_t blocked_up = 0, blocked_down = 0, blocked_left = 0, blocked_right = 0;
        int32_t     total = GAME_MAP->width * GAME_MAP->height;
        int32_t     ci    = cell_index;
        int32_t     crow  = ci / GAME_MAP->width;

        /* draw the explosion */
        for (uint8_t r = 1; r <= a->bombs[i].radius; r++) {
            uint8_t tip = (r == a->bombs[i].radius);
        
            /* UP */
            if (!blocked_up) {
                int32_t idx = ci - r * GAME_MAP->width;
                if (idx < 0)                                blocked_up = 1;
                else if (GAME_MAP->cells[idx] == 'H')       blocked_up = 1;
                else if (GAME_MAP->cells[idx] == 'S')       blocked_up = 1; /* TODO: soft wall block destroyed message? we don't need it really */
                else if (GAME_MAP->cells[idx] == '@') {     
                    printf("Chain reaction triggered at cell index %d!\n", idx);
                    blocked_up = 1;
                }
                else if (GAME_MAP->cells[idx] == 'B') {
                    /* chain reaction, explode this bomb immediately -
                        could create a queue so nothing weird happens when bombs 
                        get chained in a circle and the first one will finish 
                        execution by overwriting one of the last bombs laser,
                        but that seems overkill for an edge case like that. */
                    for (size_t j = 0; j < ACTIVE_BOMBS.size; j++) {
                        if (ACTIVE_BOMBS.bombs[j].row == idx / GAME_MAP->width &&
                            ACTIVE_BOMBS.bombs[j].col == idx % GAME_MAP->width &&
                            !ACTIVE_BOMBS.bombs[j].active) {
                                bomb_array_explode(&ACTIVE_BOMBS, j);
                                break;
                        }
                    }
                    blocked_up = 1;
                }
                else if (GAME_MAP->cells[idx] >= '1' && GAME_MAP->cells[idx] <= '8') {
                    uint8_t hit_id = GAME_MAP->cells[idx] - '0';
                    kill_player(hit_id);
                    GAME_MAP->cells[idx] = tip ? '^' : '|';
                    blocked_up = 1;
                }
                else GAME_MAP->cells[idx] = tip ? '^' : '|';
            }
            /* DOWN */
            if (!blocked_down) {
                int32_t idx = ci + r * GAME_MAP->width;
                if (idx >= total)                           blocked_down = 1;
                else if (GAME_MAP->cells[idx] == 'H')       blocked_down = 1;
                else if (GAME_MAP->cells[idx] == 'S')       blocked_down = 1;
                else if (GAME_MAP->cells[idx] == '@')       blocked_down = 1;
                else if (GAME_MAP->cells[idx] == 'B') {
                    /* chain reaction, explode this bomb immediately */
                    for (size_t j = 0; j < ACTIVE_BOMBS.size; j++) {
                        if (ACTIVE_BOMBS.bombs[j].row == idx / GAME_MAP->width &&
                            ACTIVE_BOMBS.bombs[j].col == idx % GAME_MAP->width &&
                            !ACTIVE_BOMBS.bombs[j].active) {
                                bomb_array_explode(&ACTIVE_BOMBS, j);
                                break;
                        }
                    }
                    blocked_down = 1;
                }
                else if (GAME_MAP->cells[idx] >= '1' && GAME_MAP->cells[idx] <= '8') {
                    uint8_t hit_id = GAME_MAP->cells[idx] - '0';
                    kill_player(hit_id);
                    GAME_MAP->cells[idx] = tip ? 'v' : '|';
                    blocked_down = 1;
                }
                else GAME_MAP->cells[idx] = tip ? 'v' : '|';
            }
            /* LEFT */
            if (!blocked_left) {
                int32_t idx = ci - r;
                if (idx < 0 || idx / GAME_MAP->width != crow)    blocked_left = 1;
                else if (GAME_MAP->cells[idx] == 'H')            blocked_left = 1;
                else if (GAME_MAP->cells[idx] == 'S')            blocked_left = 1;
                else if (GAME_MAP->cells[idx] == '@')            blocked_left = 1;
                else if (GAME_MAP->cells[idx] == 'B') {
                    /* chain reaction, explode this bomb immediately */
                    for (size_t j = 0; j < ACTIVE_BOMBS.size; j++) {
                        if (ACTIVE_BOMBS.bombs[j].row == idx / GAME_MAP->width &&
                            ACTIVE_BOMBS.bombs[j].col == idx % GAME_MAP->width &&
                            !ACTIVE_BOMBS.bombs[j].active) {
                                bomb_array_explode(&ACTIVE_BOMBS, j);
                                break;
                        }
                    }
                    blocked_left = 1;
                }
                else if (GAME_MAP->cells[idx] >= '1' && GAME_MAP->cells[idx] <= '8') {
                    uint8_t hit_id = GAME_MAP->cells[idx] - '0';
                    kill_player(hit_id);
                    GAME_MAP->cells[idx] = tip ? '<' : '|';
                    blocked_left = 1;
                }
                else GAME_MAP->cells[idx] = tip ? '<' : '-';
            }
            /* RIGHT */
            if (!blocked_right) {
                int32_t idx = ci + r;
                if (idx >= total || idx / GAME_MAP->width != crow)   blocked_right = 1;
                else if (GAME_MAP->cells[idx] == 'H')                blocked_right = 1;
                else if (GAME_MAP->cells[idx] == 'S')                blocked_right = 1;
                else if (GAME_MAP->cells[idx] == '@')                blocked_right = 1;
                else if (GAME_MAP->cells[idx] == 'B') {
                    /* chain reaction, explode this bomb immediately */
                    for (size_t j = 0; j < ACTIVE_BOMBS.size; j++) {
                        if (ACTIVE_BOMBS.bombs[j].row == idx / GAME_MAP->width &&
                            ACTIVE_BOMBS.bombs[j].col == idx % GAME_MAP->width &&
                            !ACTIVE_BOMBS.bombs[j].active) {
                                bomb_array_explode(&ACTIVE_BOMBS, j);
                                break;
                        }
                    }
                    blocked_right = 1;
                }
                else if (GAME_MAP->cells[idx] >= '1' && GAME_MAP->cells[idx] <= '8') {
                    uint8_t hit_id = GAME_MAP->cells[idx] - '0';
                    kill_player(hit_id);
                    GAME_MAP->cells[idx] = tip ? '>' : '|';
                    blocked_right = 1;
                }
                else GAME_MAP->cells[idx] = tip ? '>' : '-';
            }
        }  

        /* send explosion start message to clients */
        printf("Bomb at (%d, %d) started exploding!\n", a->bombs[i].row, a->bombs[i].col);
        if (send_explosion_start(CLIENT_FD, TARGET_SERVER, TARGET_BROADCAST, &(msg_explosion_start_t){
            .cell_index = cell_index,
            .radius = a->bombs[i].radius
        }) < 0) {
            perror("Failed to send explosion start message");
        }
        return;
    }
    printf("Bomb at (%d, %d) exploded!\n", a->bombs[i].row, a->bombs[i].col);

    uint8_t blocked_up = 0, blocked_down = 0, blocked_left = 0, blocked_right = 0;
    int32_t     total = GAME_MAP->width * GAME_MAP->height;
    int32_t     ci    = cell_index;
    int32_t     crow  = ci / GAME_MAP->width;
    
    GAME_MAP->cells[ci] = '.';

    /* TODO: 
        check for explosion chains (hit B tile)
        check for players (hit 1-8 tile) create function for damaging */
    for (int r = 1; r <= a->bombs[i].radius; r++) {
        /* UP */
        if (!blocked_up) {
            int32_t idx = ci - r * GAME_MAP->width;
            if (idx < 0 || GAME_MAP->cells[idx] == 'H')        blocked_up = 1;
            else if (GAME_MAP->cells[idx] == 'S')             { GAME_MAP->cells[idx] = '.'; blocked_up = 1; }
            else if (GAME_MAP->cells[idx] == '-' ||
                     GAME_MAP->cells[idx] == '<' ||
                     GAME_MAP->cells[idx] == '>')             { /* these are from a newer explosion */ }
            else if (GAME_MAP->cells[idx] == 'v')             { blocked_up = 1; /* newer explosion starts here */}
            else                                               GAME_MAP->cells[idx] = '.';
        }
        /* DOWN */
        if (!blocked_down) {
            int32_t idx = ci + r * GAME_MAP->width;
            if (idx >= total || GAME_MAP->cells[idx] == 'H')   blocked_down = 1;
            else if (GAME_MAP->cells[idx] == 'S')             { GAME_MAP->cells[idx] = '.'; blocked_down = 1; }
            else if (GAME_MAP->cells[idx] == '-' ||
                     GAME_MAP->cells[idx] == '<' ||
                     GAME_MAP->cells[idx] == '>')             { /* these are from a newer explosion */ }
            else if (GAME_MAP->cells[idx] == '^')             { blocked_up = 1; /* newer explosion starts here */}
            else                                               GAME_MAP->cells[idx] = '.';
        }
        /* LEFT */
        if (!blocked_left) {
            int32_t idx = ci - r;
            if (idx < 0 || idx / GAME_MAP->width != crow ||
                GAME_MAP->cells[idx] == 'H')                    blocked_left = 1;
            else if (GAME_MAP->cells[idx] == 'S')             { GAME_MAP->cells[idx] = '.'; blocked_left = 1; }
            else if (GAME_MAP->cells[idx] == '|' ||
                     GAME_MAP->cells[idx] == '^' ||
                     GAME_MAP->cells[idx] == 'v')             { /* these are from a newer explosion */ }
            else if (GAME_MAP->cells[idx] == '>')             { blocked_up = 1; /* newer explosion starts here */}
            else                                               GAME_MAP->cells[idx] = '.';
        }
        /* RIGHT */
        if (!blocked_right) {
            int32_t idx = ci + r;
            if (idx >= total || idx / GAME_MAP->width != crow ||
                GAME_MAP->cells[idx] == 'H')                    blocked_right = 1;
            else if (GAME_MAP->cells[idx] == 'S')             { GAME_MAP->cells[idx] = '.'; blocked_right = 1; }
            else if (GAME_MAP->cells[idx] == '|' ||
                     GAME_MAP->cells[idx] == '^' ||
                     GAME_MAP->cells[idx] == 'v')             { /* these are from a newer explosion */ }
            else if (GAME_MAP->cells[idx] == '<')             { blocked_up = 1; /* newer explosion starts here */}
            else                                               GAME_MAP->cells[idx] = '.';
        }
    }

    bombs_active_for_player[a->bombs[i].owner_id]--;
    a->bombs[i] = a->bombs[a->size - 1];
    a->size--;
    
    if (send_explosion_end(CLIENT_FD, TARGET_SERVER, TARGET_BROADCAST, &(msg_explosion_end_t){
        .cell_index = cell_index,
        .radius = a->bombs[i].radius
    }) < 0) {
        perror("Failed to send explosion end message");
    }
}

static void bomb_array_tick(BombArray *a)
{
    size_t i = 0;

    while (i < a->size) {
        a->bombs[i].timer_ticks--;

        if (a->bombs[i].timer_ticks <= 0) {
            printf("bomb timer: %d\n", a->bombs[i].timer_ticks);
            bomb_array_explode(a, i);
        } else {
            i++;
        }
    }
}

static void handle_bomb_attempt(const msg_generic_t *header, const msg_bomb_attempt_t *bomb_msg) {
    if (GAME_MAP->cells[bomb_msg->cell_index] == '0' + test_player.id) {
        if (bombs_active_for_player[test_player.id] >= test_player.bomb_count) {
            printf("Player %d cannot place more bombs (active bombs: %d)\n", test_player.id, bombs_active_for_player[test_player.id]);
            return;
        }

        bombs_active_for_player[test_player.id]++;
        is_player_on_bomb[test_player.id] = 1;

        bomb_array_push(&ACTIVE_BOMBS, (bomb_t){
            .active = false, /* active means it is currently exploding, useful for tracking in seconds */
            .owner_id = test_player.id,
            .row = test_player.row,
            .col = test_player.col,
            .radius = test_player.bomb_radius,
            .timer_ticks = BOMB_DETONATION_TICKS
        });

        /* figure out overlay later - doesn't work on low resolution 
            currently vizualize the bomb on top of the player */
        GAME_MAP->cells[bomb_msg->cell_index] = 'B'; /* mark bomb on the map */
    
        printf("Player %d placed a bomb at cell index %d\n", test_player.id, bomb_msg->cell_index);
        
        if (send_bomb(CLIENT_FD, TARGET_SERVER, TARGET_BROADCAST, &(msg_bomb_t){ .player_id = test_player.id, .cell_index = bomb_msg->cell_index }) < 0) {
            perror("Failed to send bomb message");
        }
    } else {
        printf("Invalid bomb placement by player %d (header id: %d) at cell index %d\n", test_player.id, header->sender_id, bomb_msg->cell_index);
    }
}

int is_bonus_cell(char cell) {
    return cell == 'A' || cell == 'T' || cell == 'R' || cell == 'N';
}

static void apply_bonus(uint8_t player_id, char bonus_type, uint16_t new_pos) {
    switch (bonus_type) {
        case 'A': test_player.speed += 1;              break;
        case 'T': test_player.bomb_timer_ticks += 10;  break;
        case 'R': test_player.bomb_radius += 1;        break;
        case 'N': test_player.bomb_count += 1;         break;	
    }
    if (send_bonus_retrieved(CLIENT_FD, TARGET_SERVER, TARGET_BROADCAST, &(msg_bonus_retrieved_t){ .player_id = player_id, .cell_index = new_pos }) < 0) {
        perror("Failed to send bonus retrieved message");
    }

}

static void handle_move_attempt(const msg_generic_t *header, const msg_move_attempt_t *move_msg) {
    printf("Received MOVE_ATTEMPT from client: %d! Direction: %c\n", header->sender_id, move_msg->direction);
    if (player_move_cooldown[header->sender_id] > 0) {
        /* player is moving too fast, silently drop or send a blocked response */
        printf("Player %d is moving too fast! Cooldown: %d ticks\n", header->sender_id, player_move_cooldown[header->sender_id]);
        return;
    }
    /* reset cooldown based on this player's speed */
    /* speed 4 -> cooldown = 20/4 = 5 ticks between moves */
    player_move_cooldown[header->sender_id] = (uint8_t)(TICKS_PER_SECOND / test_player.speed); // TODO: replace test_player with lookup by header->sender_id when multiple players are implemented

    uint16_t pos = make_cell_index(test_player.row, test_player.col, GAME_MAP->width);
    int16_t tmp_row = test_player.row;
    int16_t tmp_col = test_player.col;
    /* update player position based on direction */
    switch (move_msg->direction) {
        case 'U': 
            tmp_row -= 1; 
            if (tmp_row < 0) {
                goto blocked_move;
            }
            break;
        case 'D': 
            tmp_row += 1; 
            if (tmp_row >= GAME_MAP->height) {
                goto blocked_move;
            }
            break;
        case 'L': 
            tmp_col -= 1; 
            if (tmp_col < 0) {
                goto blocked_move;
            }
            break;
        case 'R': 
            tmp_col += 1; 
            if (tmp_col >= GAME_MAP->width) {
                goto blocked_move;
            }
            break;
    }

    uint16_t new_pos = make_cell_index(tmp_row, tmp_col, GAME_MAP->width);
    /* check if the new position is valid (not a wall) */
    if (GAME_MAP->cells[new_pos] == '.' || 
        is_bonus_cell(GAME_MAP->cells[new_pos]) ||
        is_on_laser(GAME_MAP->cells[new_pos])) {
        if (!is_player_on_bomb[test_player.id]) {
            GAME_MAP->cells[pos] = '.'; /* clear old position */
        } else {
            /* player is moving off the bomb, keep it on screen */
            is_player_on_bomb[test_player.id] = 0;
        }
        if (is_bonus_cell(GAME_MAP->cells[new_pos])) {
            apply_bonus(test_player.id, GAME_MAP->cells[new_pos], new_pos);
        }
        if (!is_on_laser(GAME_MAP->cells[new_pos])) {
            GAME_MAP->cells[new_pos] = '0' + test_player.id; /* move player to new position */
        } else {
            printf("Player %d stepped on a laser at cell index %d!\n", test_player.id, new_pos);
            kill_player(test_player.id);
            return;
        }
        test_player.row = tmp_row;
        test_player.col = tmp_col;
        
        if (send_moved(CLIENT_FD, TARGET_SERVER, TARGET_BROADCAST, &(msg_moved_t){ .player_id = test_player.id, .cell_index = new_pos }) < 0) {
            perror("Failed to send moved message");
        }
    } else {
        blocked_move:
            printf("Move blocked to position (%d, %d)\n", tmp_row, tmp_col);
    }
}

static void decrement_move_cooldown() {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (player_move_cooldown[i] > 0) {
            player_move_cooldown[i]--;
        }
    }
}

static inline int is_on_laser(char c) {
    return c == '-' || c == '|' || c == '^' || c == 'v' || c == '<' || c == '>';
}

static void kill_player(uint8_t player_id) {
    test_player.lives = 0;

    if (test_player.lives == 0) {
        printf("Player %d has been killed by a laser!\n", player_id);
        /* send player death message to clients */
        if (send_player_death(CLIENT_FD, TARGET_SERVER, TARGET_BROADCAST, &(msg_death_t){ .player_id = player_id }) < 0) {
            perror("Failed to send player death message");
        }
    }
}

static void handle_disconnect() {
    exit(0);
}

static void handle_error() {
    fprintf(stderr, "An error occurred while receiving data from the server.\n");
    exit(1);
}

static void dispatch(int fd, const msg_generic_t *header, const void *payload) {
    (void)fd; /* not needed for now, but might be useful later for messages that require a response */
    (void)payload; /*compiler*/

    switch (header->msg_type) {
        case MSG_PING: {
            printf("Received PING from client!\n");
            break;
        }
        
        case MSG_MOVE_ATTEMPT: {
            handle_move_attempt(header, (const msg_move_attempt_t *)payload);
            break;
        }

        case MSG_BOMB_ATTEMPT: {
            handle_bomb_attempt(header, (const msg_bomb_attempt_t *)payload);
            break;
        }
    }
}

static void handle_client_messages(int fd)
{
    msg_generic_t header;
    void         *payload;
    size_t        payload_len;

    for (;;) {
        int rc = recv_protocol_message(fd, &header, &payload, &payload_len);

        if (rc == 2) return;               /* nothing pending this tick */
        if (rc == 1) { handle_disconnect(); return; }
        if (rc == -1){ handle_error();      return; }

        dispatch(fd, &header, payload);
        free(payload);
        /* loop: drain any further messages that arrived this tick */
    }
}
