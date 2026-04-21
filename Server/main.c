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

typedef struct {
    bomb_t bombs[MAX_BOMBS];
    size_t size;
} BombArray;
static int bomb_array_push(BombArray *a, bomb_t bomb);
static void bomb_array_explode(BombArray *a, size_t i);
static void bomb_array_tick_second(BombArray *a);

BombArray ACTIVE_BOMBS = { .size = 0 };

/* later create a pool of players and find by sender id or by binding sockets to ids */
player_t test_player = {
    .id = 1,
    .name = "Test Player",
    .row = 0,
    .col = 1,
    .alive = true,
    .ready = true,
    .bomb_count = 2,
    .bomb_radius = 3,
    .bomb_timer_ticks = 5,
    .speed = 1
};


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

    listen(server_fd, 5);  
    printf("Server listening on port 6969\n");  

    socklen_t addr_len = sizeof(client_addr);  
    CLIENT_FD = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);  
    if (CLIENT_FD < 0) {  
        perror("Accept failed");  
        return EXIT_FAILURE;  
    }  

    printf("Client connected\n");  

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

    /* ------------------ main loop ------------------ */
    uint8_t tick_count = 0;
    while (1) {  
        handle_client_messages(CLIENT_FD);

        if (++tick_count >= TICKS_PER_SECOND) {
            bomb_array_tick_second(&ACTIVE_BOMBS);
            tick_count = 0;
        }

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
    /* for now just remove the bomb */
    printf("Bomb at (%d, %d) exploded!\n", a->bombs[i].row, a->bombs[i].col);
    GAME_MAP->cells[make_cell_index(a->bombs[i].row, a->bombs[i].col, GAME_MAP->width)] = '.';

    bombs_active_for_player[a->bombs[i].owner_id]--;
    a->bombs[i] = a->bombs[a->size - 1];
    a->size--;
    /* send map resync to see */
    if (send_map_message(CLIENT_FD, TARGET_SERVER, TARGET_BROADCAST, GAME_MAP) < 0) {
        perror("Failed to send map message");
    }
}

static void bomb_array_tick_second(BombArray *a)
{
    size_t i = 0;

    while (i < a->size) {
        a->bombs[i].timer_ticks--;

        if (a->bombs[i].timer_ticks <= 0) {
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
            .active = true,
            .owner_id = test_player.id,
            .row = test_player.row,
            .col = test_player.col,
            .radius = test_player.bomb_radius,
            .timer_ticks = test_player.bomb_timer_ticks
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

static void handle_move_attempt(const msg_generic_t *header, const msg_move_attempt_t *move_msg) {
    printf("Received MOVE_ATTEMPT from client: %d! Direction: %c\n", header->sender_id, move_msg->direction);
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
    if (GAME_MAP->cells[new_pos] == '.') {
        if (!is_player_on_bomb[test_player.id]) {
            GAME_MAP->cells[pos] = '.'; /* clear old position */
        } else {
            /* player is moving off the bomb, keep it on screen */
            is_player_on_bomb[test_player.id] = 0;
        }
        GAME_MAP->cells[new_pos] = '0' + test_player.id; /* move player to new position */
        test_player.row = tmp_row;
        test_player.col = tmp_col;
        
        if (send_moved(CLIENT_FD, TARGET_SERVER, TARGET_BROADCAST, &(msg_moved_t){ .player_id = test_player.id, .cell_index = new_pos }) < 0) {
            perror("Failed to send moved message");
        }
    } 
    else {
        blocked_move:
            printf("Move blocked at position (%d, %d)\n", tmp_row, tmp_col);
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
