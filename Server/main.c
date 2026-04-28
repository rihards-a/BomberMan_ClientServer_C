#include <stdlib.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include <sys/socket.h>
#include <arpa/inet.h>

#include "../net.h"
#include "../config.h"
#include "../msg_protocol.h"


#define BROADCAST_TO_EVERYONE(SEND_EXPR)                      \
do {                                                          \
    for (int _i = 0; _i < MAX_PLAYERS; ++_i) {                   \
        int target_fd = client_sockets[_i];                    \
        if (target_fd > 0) {                                  \
            if ((SEND_EXPR) < 0) {                            \
                perror("Failed to broadcast message");        \
            }                                                 \
        }                                                     \
    }                                                         \
} while (0)

char MAP_FILE_PATH[256] = "maps/test_map_1.txt"; /* default map, can be set by first player in lobby later */
msg_map_t *GAME_MAP;
int CLIENT_FD; // TODO: remove after cleaning up server->client message handling

uint8_t is_player_on_bomb[8];
uint8_t bombs_active_for_player[8]; /* > 0 means player has bombs on the map */
uint16_t BOMB_DETONATION_TICKS = 20; /* 1 second, set on map init later again */
uint8_t player_move_cooldown[8] = {0}; /* ticks until player can move again, based on their speed */


typedef struct {
    bomb_t bombs[MAX_BOMBS];
    size_t size;
} BombArray;
static int bomb_array_push(BombArray *a, bomb_t bomb);
static void bomb_array_explode(BombArray *a, size_t i);
static void bomb_array_tick(BombArray *a);
BombArray ACTIVE_BOMBS = { .size = 0 };

int client_sockets[MAX_PLAYERS] = {0};
player_t players[MAX_PLAYERS];
uint8_t player_count = 0;

game_status_t game_status = GAME_LOBBY;
uint8_t last_alive_id = 0;

static void decrement_move_cooldown(void);
static void handle_client_messages(int fd);
static void init_map_from_file(const char *filename);
bool add_to_client_list(int new_fd);
static void check_game_finished(void);
static void finish_game(void);
static void reset_to_lobby(void);

int main() {  
    int server_fd;  
    struct sockaddr_in server_addr, client_addr;  

    server_fd = socket(AF_INET, SOCK_STREAM, 0);  
    if (server_fd == -1) {  
        perror("Socket creation failed");  
        return EXIT_FAILURE;  
    }

    // Allow the server to bind to the port even if it's in TIME_WAIT state
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    server_addr.sin_family = AF_INET;  
    server_addr.sin_addr.s_addr = INADDR_ANY;  
    server_addr.sin_port = htons(SERVER_PORT);  

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {  
        perror("Bind failed");  
        return EXIT_FAILURE;  
    }  

    listen(server_fd, MAX_PLAYERS);  
    printf("Server listening on port %d\n", SERVER_PORT);  


    while (1) {  
        /* Note well: Upon return, each of the file descriptor sets is
            modified in place to indicate which file descriptors are currently
            "ready".  Thus, if using select() within a loop, the sets must be
            reinitialized before each call. - linux-man-pages */
        fd_set read_fds;
        FD_ZERO(&read_fds);
         /* only reinitialize server during the lobby phase 
            for accepting new connections to the server. */
        int max_fd = 0;
        if (game_status == GAME_LOBBY) {
            FD_SET(server_fd, &read_fds);
            max_fd = server_fd;
        }
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (client_sockets[i] > 0) {
                FD_SET(client_sockets[i], &read_fds);
                if (client_sockets[i] > max_fd) {
                    max_fd = client_sockets[i];
                }
            }
        }
        struct timeval tv = { 0, 0 }; /* select active fds to act upon */
        if (select(max_fd + 1, &read_fds, NULL, NULL, &tv) < 0) {
            perror("select failed");
            return EXIT_FAILURE;
        }

        /* --- 1. HANDLE NEW CONNECTIONS (LOBBY) --- */
        if (game_status == GAME_LOBBY && FD_ISSET(server_fd, &read_fds)) {
            socklen_t addr_len = sizeof(client_addr);
            int new_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
            
            if (new_fd >= 0) {
                if (!add_to_client_list(new_fd)) {
                    printf("Server full! Rejecting connection on FD %d\n", new_fd);
                    send_disconnect(new_fd, TARGET_SERVER, 0);
                    close(new_fd);
                }
            }
        }

        /* --- 2. HANDLE INCOMING MESSAGES --- */
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (client_sockets[i] > 0 && FD_ISSET(client_sockets[i], &read_fds)) {
                handle_client_messages(client_sockets[i]);
            }
        }

        /* --- 3. LOBBY LOGIC: START GAME? --- */
        if (game_status == GAME_LOBBY && player_count > 0) {
            bool everyone_ready = true;
            for (int i = 0; i < 8; i++) {
                if (client_sockets[i] > 0 && !players[i].ready) {
                    everyone_ready = false;
                    break;
                }
            }
            /* currently allow single player, otherwise check for player_count > 1 */
            if (everyone_ready) {
                printf("All players ready! Starting game...\n");
                init_map_from_file(MAP_FILE_PATH);
                
                BROADCAST_TO_EVERYONE(send_set_status(target_fd, TARGET_SERVER, TARGET_BROADCAST, &(msg_set_status_t){ .game_status = 1 }));
                game_status = GAME_RUNNING;

                BROADCAST_TO_EVERYONE(send_map_message(target_fd, TARGET_SERVER, TARGET_BROADCAST, GAME_MAP));
            }
        }

        /* --- 4. GAME TICKS --- */
        if (game_status == GAME_RUNNING) {
            decrement_move_cooldown();
            bomb_array_tick(&ACTIVE_BOMBS);
        }

        check_game_finished();


        usleep(1000000 / TICKS_PER_SECOND); /* 1e6 for microsecond to second */
    }

    // TODO: server cleanup, though currently unreachable due to infinite loop and no disconnect handling
    printf("Closing client sockets...\n");
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (client_sockets[i] > 0) {
            close(client_sockets[i]);
        }
    }

    printf("Closing server socket...\n");
    close(server_fd);

    return 0;  
}


/* -------------------------- function declarations --------------------------- */

bool add_to_client_list(int new_fd) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (client_sockets[i] == 0) {
            client_sockets[i] = new_fd;
            
            players[i].id = 0; 
            memset(players[i].name, 0, PLAYER_NAME_LEN);
            
            return true;
        }
    }
    return false;
}

static void handle_initial_connection(int fd, const msg_hello_t *hello_msg) {
    int idx = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (client_sockets[i] == fd) {
            idx = i;
            break;
        }
    }
    if (idx == -1) {
        printf("Error: FD %d not found in client_sockets\n", fd);
        return;
    }
    printf("Handling initial connection for FD %d at index %d\n", fd, idx);

    players[idx].id = (uint8_t)(idx + 1);
    players[idx].ready = false;
    strncpy(players[idx].name, hello_msg->player_name, PLAYER_NAME_LEN);
    players[idx].name[PLAYER_NAME_LEN - 1] = '\0';
    /* these fields get overwritten by the map info if specified */ 
    players[idx].lives            = 1;
    players[idx].speed            = 3;
    players[idx].bomb_count       = 2;
    players[idx].bomb_radius      = 1;
    players[idx].bomb_timer_ticks = 20;

    welcome_client_t* client_info = malloc(MAX_PLAYERS*sizeof(welcome_client_t));
    uint8_t client_count = 0;
    for (int j = 0; j < MAX_PLAYERS; j++) {
        if (players[j].id != 0) {
            client_info[client_count].id = players[j].id;
            strncpy(client_info[client_count].player_name, players[j].name, sizeof(client_info[client_count].player_name));
            client_info[client_count].player_name[PLAYER_NAME_LEN - 1] = '\0';
            client_info[client_count].ready = players[j].ready;
            client_count++;
        }
    }

    size_t payload_size = client_count * sizeof(welcome_client_t);
    size_t total_msg_size = sizeof(msg_welcome_t) + payload_size;

    msg_welcome_t *welcome_msg = malloc(total_msg_size);

    strncpy(welcome_msg->server_id, "TEST_SRV", SERVER_ID_LEN);
    welcome_msg->game_status = 0;
    welcome_msg->length = payload_size;
    memcpy(welcome_msg->clients, client_info, payload_size);

    printf("Broadcasting welcome message for new player %s (ID: %d)\n", players[idx].name, players[idx].id);
    send_welcome_message(fd, 255, players[idx].id, welcome_msg);

    msg_hello_t hello_msg_to_send;
    strncpy(hello_msg_to_send.client_id, hello_msg->client_id, CLIENT_ID_LEN);
    strncpy(hello_msg_to_send.player_name, hello_msg->player_name, PLAYER_NAME_LEN);

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (client_sockets[i] > 0 && client_sockets[i] != fd) {
            send_hello(client_sockets[i], players[idx].id, players[i].id, &hello_msg_to_send);
        }
    }

    free(client_info);
    free(welcome_msg);

    printf("Joined: %s (ID: %d) on FD %d\n", players[idx].name, players[idx].id, fd);
                        
    player_count++;
}

static void init_map_from_file(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        perror("Could not open map file");
        exit(EXIT_FAILURE);
    }
    uint8_t h, w;
    if (fscanf(fp, "%hhu %hhu", &h, &w) != 2) {
        fprintf(stderr, "Error reading map dimensions\n");
        fclose(fp);
        exit(EXIT_FAILURE);
    }

    uint8_t player_speed = 0, bomb_radius = 0, bomb_timer_ticks = 0;
    fscanf(fp, "%hhu %hhu %hhu %hu", 
        &player_speed, &bomb_timer_ticks, 
        &bomb_radius, &BOMB_DETONATION_TICKS);
    printf("Map config - Player Speed: %d, Bomb Timer Ticks: %d, Bomb Radius: %d, Bomb Detonation Ticks: %d\n", 
        player_speed, bomb_timer_ticks, bomb_radius, BOMB_DETONATION_TICKS);

    GAME_MAP = malloc(sizeof(msg_map_t) + (h * w));
    if (!GAME_MAP) {
        fclose(fp);
        exit(EXIT_FAILURE);
    }
    GAME_MAP->height = h;
    GAME_MAP->width = w;
    for (int i = 0; i < (h * w); i++) {
        if (fscanf(fp, " %c", &GAME_MAP->cells[i]) != 1) {
            break;
        }
    }

    fclose(fp);
    printf("Map loaded: %dx%d\n", h, w);

    /* update player speed, timer ticks, radius and positions */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (client_sockets[i] > 0) {
            printf("Setting player %d speed to %d, bomb radius to %d, bomb timer ticks to %d based on map config\n", 
                players[i].id, player_speed, bomb_radius, bomb_timer_ticks);
            players[i].speed = player_speed;
            players[i].bomb_radius = bomb_radius;
            players[i].bomb_timer_ticks = bomb_timer_ticks;
            /* find positions by id on the map */
            for (int j = 0; j < h * w; j++) {
                if (GAME_MAP->cells[j] == ('0' + players[i].id)) {
                    players[i].row = j / w;
                    players[i].col = j % w;
                    break;
                }
            }
        }
    }

    printf("Player starting positions set based on map data.\n");
}

static inline int is_on_laser(char c) {
    return c == '-' || c == '|' || c == '^' || c == 'v' || c == '<' || c == '>';
}

static void kill_player(uint8_t player_id) {
    players[player_id - 1].lives = 0;
    players[player_id - 1].row = -1;
    players[player_id - 1].col = -1;

    if (players[player_id - 1].lives == 0) {
        printf("Player %d has been killed by a laser!\n", player_id);
        /* send player death message to clients */
        BROADCAST_TO_EVERYONE(send_player_death(target_fd, TARGET_SERVER, TARGET_BROADCAST, &(msg_death_t){ .player_id = player_id }));
    }
}

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
        a->bombs[i].timer_ticks = players[a->bombs[i].owner_id - 1].bomb_timer_ticks + 1; 

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
                else if (GAME_MAP->cells[idx] == 'S')       blocked_up = 1;
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
                    GAME_MAP->cells[idx] = tip ? '<' : '-';
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
                    GAME_MAP->cells[idx] = tip ? '>' : '-';
                }
                else GAME_MAP->cells[idx] = tip ? '>' : '-';
            }
        }  

        /* send explosion start message to clients */
        printf("Bomb at (%d, %d) started exploding!\n", a->bombs[i].row, a->bombs[i].col);
        u_int8_t cur_radius = a->bombs[i].radius;
        BROADCAST_TO_EVERYONE(send_explosion_start(target_fd, TARGET_SERVER, TARGET_BROADCAST, &(msg_explosion_start_t){
            .cell_index = cell_index,
            .radius = cur_radius
        }));
        return;
    }
    printf("Bomb at (%d, %d) exploded!\n", a->bombs[i].row, a->bombs[i].col);

    uint8_t blocked_up = 0, blocked_down = 0, blocked_left = 0, blocked_right = 0;
    int32_t     total = GAME_MAP->width * GAME_MAP->height;
    int32_t     ci    = cell_index;
    int32_t     crow  = ci / GAME_MAP->width;
    
    GAME_MAP->cells[ci] = '.';

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

    bombs_active_for_player[a->bombs[i].owner_id - 1]--;
    a->bombs[i] = a->bombs[a->size - 1];
    a->size--;
    
    // if (send_explosion_end(CLIENT_FD, TARGET_SERVER, TARGET_BROADCAST, &(msg_explosion_end_t){
    //     .cell_index = cell_index,
    //     .radius = a->bombs[i].radius
    // }) < 0) {
    //     perror("Failed to send explosion end message");
    // }
    u_int8_t cur_radius = a->bombs[i].radius;
    BROADCAST_TO_EVERYONE(send_explosion_end(target_fd, TARGET_SERVER, TARGET_BROADCAST, &(msg_explosion_end_t){
        .cell_index = cell_index,
        .radius = cur_radius
    }));
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
    if (GAME_MAP->cells[bomb_msg->cell_index] == '0' + players[header->sender_id - 1].id) {
        if (bombs_active_for_player[header->sender_id - 1] >= players[header->sender_id - 1].bomb_count) {
            printf("Player %d cannot place more bombs (active bombs: %d)\n", header->sender_id, bombs_active_for_player[header->sender_id - 1]);
            return;
        }

        bombs_active_for_player[header->sender_id - 1]++;
        is_player_on_bomb[header->sender_id - 1] = 1;

        bomb_array_push(&ACTIVE_BOMBS, (bomb_t){
            .active = false, /* active means it is currently exploding, useful for tracking in seconds */
            .owner_id = header->sender_id,
            .row = players[header->sender_id - 1].row,
            .col = players[header->sender_id - 1].col,
            .radius = players[header->sender_id - 1].bomb_radius,
            .timer_ticks = players[header->sender_id - 1].bomb_timer_ticks
        });

        /* figure out overlay later - doesn't work on low resolution 
            currently vizualize the bomb on top of the player */
        GAME_MAP->cells[bomb_msg->cell_index] = 'B'; /* mark bomb on the map */
    
        printf("Player %d placed a bomb at cell index %d\n", players[header->sender_id - 1].id, bomb_msg->cell_index);
        
        // if (send_bomb(CLIENT_FD, TARGET_SERVER, TARGET_BROADCAST, &(msg_bomb_t){ .player_id = players[header->sender_id - 1].id, .cell_index = bomb_msg->cell_index }) < 0) {
        //     perror("Failed to send bomb message");
        // }
        BROADCAST_TO_EVERYONE(send_bomb(target_fd, TARGET_SERVER, TARGET_BROADCAST, &(msg_bomb_t){ .player_id = players[header->sender_id - 1].id, .cell_index = bomb_msg->cell_index }));
    } else {
        printf("Invalid bomb placement by player %d (header id: %d) at cell index %d\n", players[header->sender_id - 1].id, header->sender_id, bomb_msg->cell_index);
    }
}

int is_bonus_cell(char cell) {
    return cell == 'A' || cell == 'T' || cell == 'R' || cell == 'N';
}

static void apply_bonus(uint8_t player_id, char bonus_type, uint16_t new_pos) {
    switch (bonus_type) {
        case 'A': players[player_id - 1].speed += 1;              break;
        case 'T': players[player_id - 1].bomb_timer_ticks += 10;  break;
        case 'R': players[player_id - 1].bomb_radius += 1;        break;
        case 'N': players[player_id - 1].bomb_count += 1;         break;	
    }
    // if (send_bonus_retrieved(CLIENT_FD, TARGET_SERVER, TARGET_BROADCAST, &(msg_bonus_retrieved_t){ .player_id = player_id, .cell_index = new_pos }) < 0) {
    //     perror("Failed to send bonus retrieved message");
    // }
    BROADCAST_TO_EVERYONE(send_bonus_retrieved(target_fd, TARGET_SERVER, TARGET_BROADCAST, &(msg_bonus_retrieved_t){ .player_id = player_id, .cell_index = new_pos }));


}

static void handle_move_attempt(const msg_generic_t *header, const msg_move_attempt_t *move_msg) {
    printf("Received MOVE_ATTEMPT from client: %d! Direction: %c\n", header->sender_id, move_msg->direction);
    if (player_move_cooldown[header->sender_id - 1] > 0) {
        /* player is moving too fast, silently drop or send a blocked response */
        printf("Player %d is moving too fast! Cooldown: %d ticks\n", header->sender_id, player_move_cooldown[header->sender_id - 1]);
        return;
    }
    /* reset cooldown based on this player's speed */
    /* speed 4 -> cooldown = 20/4 = 5 ticks between moves */
    player_move_cooldown[header->sender_id - 1] = (uint8_t)(TICKS_PER_SECOND / players[header->sender_id - 1].speed);

    uint16_t pos = make_cell_index(players[header->sender_id - 1].row, players[header->sender_id - 1].col, GAME_MAP->width);
    int16_t tmp_row = players[header->sender_id - 1].row;
    int16_t tmp_col = players[header->sender_id - 1].col;
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

    uint8_t p_idx = header->sender_id - 1;
    player_t *p = &players[p_idx];
    char dest_cell = GAME_MAP->cells[new_pos];

    if (dest_cell == '.' || is_bonus_cell(dest_cell) || is_on_laser(dest_cell)) {

        // clear the previous tile first
        if (is_player_on_bomb[p_idx]) {
            // If they were on a bomb, reveal the bomb again
            GAME_MAP->cells[pos] = 'B';
            is_player_on_bomb[p_idx] = 0;
        } else {
            GAME_MAP->cells[pos] = '.';
        }

        // update player position
        p->row = tmp_row;
        p->col = tmp_col;

        // determine if they stepped on a laser or bonus before moving them there
        if (is_on_laser(dest_cell)) {
            printf("Dest cell is character: %c\n", dest_cell);
            printf("Player %hhu killed by lasser at index %d!\n", p->id, new_pos);
            kill_player(p->id); 
            return; 
        } 
        
        if (is_bonus_cell(dest_cell)) {
            apply_bonus(p->id, dest_cell, new_pos);
        }

        GAME_MAP->cells[new_pos] = '0' + p->id;

        msg_moved_t move_msg = {
            .player_id = p->id,
            .cell_index = new_pos
        };
        BROADCAST_TO_EVERYONE(send_moved(target_fd, TARGET_SERVER, TARGET_BROADCAST, &move_msg));

    } else {
        blocked_move:
            printf("Move blocked for player\n");
    }
}

static void decrement_move_cooldown() {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (player_move_cooldown[i] > 0) {
            player_move_cooldown[i]--;
        }
    }
}

static void handle_disconnect(int fd) {
    printf("Client disconnected on FD %d\n", fd);
    /* find the player index and clear their info */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (client_sockets[i] == fd) {
            client_sockets[i] = 0;
            printf("Player %d (%s) has disconnected.\n", players[i].id, players[i].name);
            printf("fd %d removed from client_sockets\n", fd);
            BROADCAST_TO_EVERYONE(send_player_death(target_fd, TARGET_SERVER, TARGET_BROADCAST, &(msg_death_t){ .player_id = players[i].id }));
            players[i].id = 0;
            memset(players[i].name, 0, PLAYER_NAME_LEN);
            players[i].ready = false;
            players[i].lives = 0;
            uint16_t gi = make_cell_index(players[i].row, players[i].col, GAME_MAP->width);
            GAME_MAP->cells[gi] = '.'; /* clear player from map */
            players[i].row = -1;
            players[i].col = -1;
            player_count--;
            break;
        }
    }
}

static void check_game_finished() {
    if (game_status != GAME_RUNNING) {
        return;
    }
    uint8_t alive_count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (client_sockets[i] > 0 && players[i].lives > 0) {
            alive_count++;
            last_alive_id = players[i].id;
        }
    }
    if (alive_count <= 1) {
        printf("Game finished! Winner: Player %d\n", last_alive_id);
        BROADCAST_TO_EVERYONE(send_set_status(target_fd, TARGET_SERVER, TARGET_BROADCAST, &(msg_set_status_t){ .game_status = 2 }));
        game_status = GAME_END;

        finish_game();
        reset_to_lobby();
    }
}

static void finish_game() {
    // send winner, last alive
    BROADCAST_TO_EVERYONE(send_winner(target_fd, TARGET_SERVER, TARGET_BROADCAST, &(msg_winner_t){ .winner_id = last_alive_id }));

    // keep winner on screen for 10 seconds before resetting lobby
    sleep(3); // TODO: change to 10 seconds in final version
}

static void reset_to_lobby() {
    printf("Resetting game to lobby...\n");
    // reset player states
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (client_sockets[i] > 0) {
            players[i].ready = false;
            players[i].lives = 1;
            players[i].speed = 3;
            players[i].bomb_count = 2;
            players[i].bomb_radius = 1;
            players[i].bomb_timer_ticks = 20;
        }
    }
    printf("Player states reset.\n");

    // free map and bombs
    free(GAME_MAP);
    ACTIVE_BOMBS.size = 0;

    // reset game status
    game_status = GAME_LOBBY;

    // send lobby status to clients
    BROADCAST_TO_EVERYONE(send_set_status(target_fd, TARGET_SERVER, TARGET_BROADCAST, &(msg_set_status_t){ .game_status = 0 }));
}

static void handle_error() {
    fprintf(stderr, "An error occurred while receiving data from the server.\n");
    exit(1);
}

static void dispatch(int fd, const msg_generic_t *header, const void *payload) {
    (void)fd; /* not needed for now, but might be useful later for messages that require a response */

    // get player id
    uint8_t p_idx = header->sender_id - 1;
    player_t *p = &players[p_idx];

    bool is_gameplay_action = (header->msg_type == MSG_MOVE_ATTEMPT || 
                               header->msg_type == MSG_BOMB_ATTEMPT);

    // drop gameplay packets if player is dead
    if (is_gameplay_action && (p->lives <= 0 || p->row == (uint16_t)-1)) {
        printf("Ignored action from dead player %d\n", header->sender_id);
        return; 
    }

    switch (header->msg_type) {
        case MSG_HELLO: {
            printf("Received HELLO from client with FD: %d!\n", fd);
            handle_initial_connection(fd, (const msg_hello_t *)payload);
            break;
        }
        case MSG_PING: {
            printf("Received PING from client!\n");
            break;
        }
        case MSG_SET_READY: {
            printf("Received SET_READY from client %d!\n", header->sender_id);
            players[header->sender_id - 1].ready = true;
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
        case MSG_CHOOSE_MAP: {
            /* verify this is coming from the first player 
                - the player with the lowest ID */
            uint8_t lowest_id = 255;
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (client_sockets[i] > 0 && players[i].id < lowest_id) {
                    lowest_id = players[i].id;
                }
            }
            if (header->sender_id == lowest_id) {
                printf("Map successfully changed.");
                msg_choose_map_t *map_msg = (msg_choose_map_t *)payload;
                memcpy(MAP_FILE_PATH, map_msg->map_name, map_msg->length);
                MAP_FILE_PATH[map_msg->length] = '\0';
            }
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
        if (rc == 1) { handle_disconnect(fd); return; }
        if (rc == -1){ handle_error();      return; }

        dispatch(fd, &header, payload);
        free(payload);
        /* loop: drain any further messages that arrived this tick */
    }
}
