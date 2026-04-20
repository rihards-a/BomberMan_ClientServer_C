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

ssize_t send_all(int fd, const void *buf, size_t len);
static void handle_client_messages(int fd);
static void handle_disconnect();
static void handle_error();
static void dispatch(int fd, const msg_generic_t *header, const void *payload);

/* later create a pool of players and find by sender id or socket binding */
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
    while (1) {  
        handle_client_messages(CLIENT_FD);

        usleep(1000000 / TICKS_PER_SECOND); /* 1e6 for microseconds */
    }

    return 0;  
}


/* -------------------------- function declarations --------------------------- */
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
            if (tmp_col + 1 >= GAME_MAP->width) {
                goto blocked_move;
            }
            tmp_col += 1; 
            break;
    }
    uint16_t new_pos = make_cell_index(tmp_row, tmp_col, GAME_MAP->width);
    /* check if the new position is valid (not a wall) */
    if (GAME_MAP->cells[new_pos] == '.') {
        GAME_MAP->cells[pos] = '.'; /* clear old position */
        GAME_MAP->cells[new_pos] = '0' + test_player.id; /* move player to new position */
        test_player.row = tmp_row;
        test_player.col = tmp_col;
        if (send_map_message(CLIENT_FD, TARGET_SERVER, TARGET_BROADCAST, GAME_MAP) < 0) {
            perror("Failed to send map message");
        }
    } 
    blocked_move:
        printf("Move blocked at position (%d, %d)\n", tmp_row, tmp_col);
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
