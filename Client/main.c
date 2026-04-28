#include <ncurses.h>
#include <stdlib.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include <stdarg.h> // for log file

#include <sys/socket.h>
#include <arpa/inet.h>  

#include "../net.h"
#include "../config.h"
#include "../msg_protocol.h"

/*  2 to use '.' symbols in between chars for padding, so the image doesn't look squished */
#define DOT_PADDING 2 
/*  max amount of ASCII symbols representing a single element on the map - lower means more efficient bounds checking on resize */
#define MAX_BLOCK_SIZE 8 
enum {
    SPACING_WIDTH = 4,

    /* player name +(4chars) character symbol on the map (1 char) +(4chars) their score (10 chars) + 4 for borders */
    SIDE_BAR_WIDTH =
        MAX_NAME_LEN
        + SPACING_WIDTH
        + 1
        + SPACING_WIDTH
        + 10
        + SPACING_WIDTH,

    /* max 8 player names 1 for a line + 6 for control info + 4 for spacing */
    SIDE_BAR_HEIGHT =
        8
        + 1
        + 8
        + 2
};

void game_log(const char *format, ...) {
    // Open in "a" (append) mode so we don't overwrite previous logs
    FILE *log_file = fopen("debug.log", "a");
    if (log_file == NULL) {
        return; 
    }

    // Initialize the variadic argument list
    va_list args;
    va_start(args, format);

    // vfprintf is the magic function that lets you pass a va_list 
    // to a file, essentially acting like printf for files.
    vfprintf(log_file, format, args);

    va_end(args);

    // Add a newline and close to ensure data is flushed to disk
    fprintf(log_file, "\n");
    fclose(log_file);
}

WINDOW *TERMINAL_WIN = NULL, *SIDEBAR_WIN = NULL, *MAP_WIN = NULL;
uint8_t BLOCK_SIZE;
int CLIENT_FD;
GameMap GAME_MAP;
char MAP_FILE_PATH[255]; /* for choosing the map */
player_t *OTHER_PLAYERS;

volatile sig_atomic_t resized = 1; /* 1 to enter loop on start */
void resize_handler(int sig) {
    (void)sig; /* compiler happy :) */
    resized = 1;
}

player_t SELF_PLAYER = {
    .id = 0,
    .name = "Test Player",
    .row = -1,
    .col = -1,
    .lives = 1,
    .ready = false,
    .bomb_count = 2,
    .bomb_radius = 1,
    .bomb_timer_ticks = 20,
    .speed = 3
};

int player_bonus_count[8] = {0}; // track how many bonuses each player has retrieved for display purposes

static void handle_user_input(int ch);
static void draw_game_board();
static void resize_game_board();
static void dispatch(int fd, const msg_generic_t *header, const void *payload);
static void handle_server_messages(int fd);


int main() {
    /* connect to server */
    struct sockaddr_in server_addr;  

    CLIENT_FD = socket(AF_INET, SOCK_STREAM, 0);  
    if (CLIENT_FD == -1) {  
        perror("Socket creation failed");  
        return EXIT_FAILURE;  
    }

    server_addr.sin_family = AF_INET;  
    server_addr.sin_port = htons(SERVER_PORT);  
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);  

    if (connect(CLIENT_FD, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {  
        perror("Connection failed");  
        return EXIT_FAILURE;  
    }  
    printf("Server connection successful\n"); 
    
    // get the name from the user 
    printf("Enter your name: ");
    fgets(SELF_PLAYER.name, PLAYER_NAME_LEN, stdin);
    SELF_PLAYER.name[PLAYER_NAME_LEN+1] = '\0';

    //send HELLO to server
    msg_hello_t hello_msg;
    strncpy(hello_msg.client_id, "TEST_CLT_1.0", CLIENT_ID_LEN);
    strncpy(hello_msg.player_name, SELF_PLAYER.name, PLAYER_NAME_LEN);

    if (send_hello(CLIENT_FD, 0, TARGET_SERVER, &hello_msg) < 0) {
        fprintf(stderr, "Failed to send HELLO message to server.\n");
        return EXIT_FAILURE;
    }

    /* TITLE SCREEN ASCII ART */
    const char *title =
    "..................."
    ".HH...H...H.H..HH.."
    ".H.H.H.H..H.H..H.H."
    ".HH..H.H.H.H.H.HH.."
    ".H.H.H.H.H.H.H.H.H."
    ".HH...H..H.H.H.HH.."
    "..................."
    ".HHH.HH......T....."
    ".H...H.H....HHH...."
    ".HHH.HH....HHHHH..."
    ".H...H.H...HHHHH..."
    ".HHH.H.H....HHH...."
    "..................."
    "..H.H....H...H..H.."
    "..H.H...H.H..HH.H.."
    ".H.H.H.HHHHH.HHHH.."
    ".H.H.H.H...H.H.HH.."
    ".H.H.H.H...H.H..H.."
    "...................";
    GAME_MAP.cells = malloc(MAX_HEIGHT * MAX_WIDTH);
    GAME_MAP.height = 19;
    GAME_MAP.width = 19;
    memcpy(GAME_MAP.cells, title, GAME_MAP.height*GAME_MAP.width);

    /* init other player storage */
    OTHER_PLAYERS = (player_t*)malloc(7*sizeof(player_t));
    // OTHER_PLAYERS[0] = (player_t){
    //     .id = 1,
    //     .name = "Other Test Player",
    //     .row = 1,
    //     .col = 1,
    //     .lives = 1,
    //     .ready = true,
    //     .bomb_count = 2,
    //     .bomb_radius = 1,
    //     .bomb_timer_ticks = 20,
    //     .speed = 4
    // };


    initscr();
    cbreak();       /* disables line buffering */
    noecho();       /* don't echo user input back */
    curs_set(0);    /* hides cursor */
    nodelay(stdscr, TRUE);  /* make getch non-blocking */
    keypad(stdscr, TRUE);   /* enable arrow keys and function keys */
    TERMINAL_WIN = newwin(LINES, COLS, 0, 0);

    signal(SIGWINCH, resize_handler);
    while (1) {
        /* handle terminal/screen resizing */
        if (resized) {
            resize_game_board();
            resized = 0;
        }

        handle_server_messages(CLIENT_FD);

        int ch = getch();
        handle_user_input(ch);

        draw_game_board();

        usleep(1000000 / TICKS_PER_SECOND); /* 1e6 for microseconds */
    }

    return 0;
}




/* -------------------------- function declarations --------------------------- */

static void handle_explosion_end(const msg_generic_t *header, const msg_explosion_end_t *expl_end) {
    (void)header; /* might be useful later */
    
    GAME_MAP.cells[expl_end->cell_index] = '.';

    uint8_t blocked_up = 0, blocked_down = 0, blocked_left = 0, blocked_right = 0;
    int32_t     total = GAME_MAP.width * GAME_MAP.height;
    int32_t     ci    = expl_end->cell_index;
    int32_t     crow  = ci / GAME_MAP.width;

    for (int r = 1; r <= expl_end->radius; r++) {
        /* UP */
        if (!blocked_up) {
            int32_t idx = ci - r * GAME_MAP.width;
            if (idx < 0 || GAME_MAP.cells[idx] == 'H')        blocked_up = 1;
            else if (GAME_MAP.cells[idx] == 'S')             { GAME_MAP.cells[idx] = '.'; blocked_up = 1; }
            else if (GAME_MAP.cells[idx] == '-' ||
                     GAME_MAP.cells[idx] == '<' ||
                     GAME_MAP.cells[idx] == '>')             { /* these are from a newer explosion */ }
            else if (GAME_MAP.cells[idx] == 'v')             { blocked_up = 1; /* newer explosion starts here */}
            else                                               GAME_MAP.cells[idx] = '.';
        }
        /* DOWN */
        if (!blocked_down) {
            int32_t idx = ci + r * GAME_MAP.width;
            if (idx >= total || GAME_MAP.cells[idx] == 'H')   blocked_down = 1;
            else if (GAME_MAP.cells[idx] == 'S')             { GAME_MAP.cells[idx] = '.'; blocked_down = 1; }
            else if (GAME_MAP.cells[idx] == '-' ||
                     GAME_MAP.cells[idx] == '<' ||
                     GAME_MAP.cells[idx] == '>')             { /* these are from a newer explosion */ }
            else if (GAME_MAP.cells[idx] == '^')             { blocked_up = 1; /* newer explosion starts here */}
            else                                               GAME_MAP.cells[idx] = '.';
        }
        /* LEFT */
        if (!blocked_left) {
            int32_t idx = ci - r;
            if (idx < 0 || idx / GAME_MAP.width != crow ||
                GAME_MAP.cells[idx] == 'H')                    blocked_left = 1;
            else if (GAME_MAP.cells[idx] == 'S')             { GAME_MAP.cells[idx] = '.'; blocked_left = 1; }
            else if (GAME_MAP.cells[idx] == '|' ||
                     GAME_MAP.cells[idx] == '^' ||
                     GAME_MAP.cells[idx] == 'v')             { /* these are from a newer explosion */ }
            else if (GAME_MAP.cells[idx] == '>')             { blocked_up = 1; /* newer explosion starts here */}
            else                                               GAME_MAP.cells[idx] = '.';
        }
        /* RIGHT */
        if (!blocked_right) {
            int32_t idx = ci + r;
            if (idx >= total || idx / GAME_MAP.width != crow ||
                GAME_MAP.cells[idx] == 'H')                    blocked_right = 1;
            else if (GAME_MAP.cells[idx] == 'S')             { GAME_MAP.cells[idx] = '.'; blocked_right = 1; }
            else if (GAME_MAP.cells[idx] == '|' ||
                     GAME_MAP.cells[idx] == '^' ||
                     GAME_MAP.cells[idx] == 'v')             { /* these are from a newer explosion */ }
            else if (GAME_MAP.cells[idx] == '<')             { blocked_up = 1; /* newer explosion starts here */}
            else                                               GAME_MAP.cells[idx] = '.';
        }
    }
}

static void handle_explosion_start(const msg_generic_t *header, const msg_explosion_start_t *expl_start) {
    (void)header; /* might be useful later */
    
    GAME_MAP.cells[expl_start->cell_index] = '@';

    uint8_t blocked_up = 0, blocked_down = 0, blocked_left = 0, blocked_right = 0;
    int32_t     total = GAME_MAP.width * GAME_MAP.height;
    int32_t     ci    = expl_start->cell_index;
    int32_t     crow  = ci / GAME_MAP.width;

    /* draw the explosion */
    for (uint8_t r = 1; r <= expl_start->radius; r++) {
        uint8_t tip = (r == expl_start->radius);
    
        /* UP */
        if (!blocked_up) {
            int32_t idx = ci - r * GAME_MAP.width;
            if (idx < 0)                               blocked_up = 1;
            else if (GAME_MAP.cells[idx] == 'H' || 
                     GAME_MAP.cells[idx] == 'S' ||
                     GAME_MAP.cells[idx] == 'B' ||     /* server should send explosion soon */
                     GAME_MAP.cells[idx] == '@')       blocked_up = 1;
            else GAME_MAP.cells[idx] = tip ? '^' : '|';
        }
        /* DOWN */
        if (!blocked_down) {
            int32_t idx = ci + r * GAME_MAP.width;
            if (idx >= total)                          blocked_down = 1;
            else if (GAME_MAP.cells[idx] == 'H' || 
                     GAME_MAP.cells[idx] == 'S' ||
                     GAME_MAP.cells[idx] == 'B' ||     
                     GAME_MAP.cells[idx] == '@')       blocked_down = 1;
            else GAME_MAP.cells[idx] = tip ? 'v' : '|';
        }
        /* LEFT */
        if (!blocked_left) {
            int32_t idx = ci - r;
            if (idx < 0 || idx / GAME_MAP.width != crow)    blocked_left = 1;
            else if (GAME_MAP.cells[idx] == 'H' || 
                     GAME_MAP.cells[idx] == 'S' ||
                     GAME_MAP.cells[idx] == 'B' ||     
                     GAME_MAP.cells[idx] == '@')            blocked_left = 1;
            else GAME_MAP.cells[idx] = tip ? '<' : '-';
        }
        /* RIGHT */
        if (!blocked_right) {
            int32_t idx = ci + r;
            if (idx >= total || idx / GAME_MAP.width != crow)   blocked_right = 1;
            else if (GAME_MAP.cells[idx] == 'H' || 
                     GAME_MAP.cells[idx] == 'S' ||
                     GAME_MAP.cells[idx] == 'B' ||     
                     GAME_MAP.cells[idx] == '@')                blocked_right = 1;
            else GAME_MAP.cells[idx] = tip ? '>' : '-';
        }
    }  

}

static void handle_bomb(const msg_generic_t *header, const msg_bomb_t *bomb_msg) {
    (void)header; /* might be useful later */
    /* for testing, just mark the bomb on the map with a 'B' */
    GAME_MAP.cells[bomb_msg->cell_index] = 'B';
}

static void handle_moved(const msg_generic_t *header, const msg_moved_t *moved_msg) {
    (void)header;
    uint8_t player_id = moved_msg->player_id;
    uint16_t new_index = moved_msg->cell_index;

    if (player_id == SELF_PLAYER.id) {
        uint16_t old_pos = make_cell_index(SELF_PLAYER.row, 
            SELF_PLAYER.col, GAME_MAP.width);
        if (GAME_MAP.cells[old_pos] == '0' + player_id) {
            GAME_MAP.cells[old_pos] = '.';
        }
        SELF_PLAYER.row = new_index / GAME_MAP.width;
        SELF_PLAYER.col = new_index % GAME_MAP.width;
    }
    else {
        for (int i = 0; i < 7; i++) {
            if (OTHER_PLAYERS[i].id == player_id) {
                uint16_t old_pos = make_cell_index(OTHER_PLAYERS[i].row, 
                    OTHER_PLAYERS[i].col, GAME_MAP.width);
                if (GAME_MAP.cells[old_pos] == '0' + player_id) {
                    GAME_MAP.cells[old_pos] = '.';
                }
                OTHER_PLAYERS[i].row = new_index / GAME_MAP.width;
                OTHER_PLAYERS[i].col = new_index % GAME_MAP.width;
                break;
            }
        }
    }

    GAME_MAP.cells[new_index] = player_id + '0';
}

static void handle_death(const msg_generic_t *header, const msg_death_t *death_msg) {
    (void)header; /* might be useful later */
    u_int8_t dead_player_id = death_msg->player_id;
    u_int16_t dead_p_row = -1, dead_p_col = -1;
    bool found = false;

    if (dead_player_id == SELF_PLAYER.id) {
        dead_p_row = SELF_PLAYER.row;
        dead_p_col = SELF_PLAYER.col;
        found = true;
    } else {
        for (int i = 0; i < 7; i++) {
            if (OTHER_PLAYERS[i].id == dead_player_id) {
                dead_p_row = OTHER_PLAYERS[i].row;
                dead_p_col = OTHER_PLAYERS[i].col;
                found = true;
                break;
            }
        }
    }

    if (found) {
        uint16_t cur_pos = make_cell_index(dead_p_row, dead_p_col, GAME_MAP.width);
        if (GAME_MAP.cells[cur_pos] == '0' + dead_player_id) {
            GAME_MAP.cells[cur_pos] = '.';
        }
    }

    if (dead_player_id == SELF_PLAYER.id) {
        SELF_PLAYER.row = -1;
        SELF_PLAYER.col = -1;
        SELF_PLAYER.lives = 0;
    } else {
        for (int i = 0; i < 7; i++) {
            if (OTHER_PLAYERS[i].id == dead_player_id) {
                OTHER_PLAYERS[i].row = -1;
                OTHER_PLAYERS[i].col = -1;
                OTHER_PLAYERS[i].lives = 0;
                break;
            }
        }
    }
}

static void handle_map_message(const msg_generic_t *header, const msg_map_t *map_msg) {
    (void)header; /* might be useful later */
    /* update local game map with new data */
    GAME_MAP.width = map_msg->width;
    GAME_MAP.height = map_msg->height;
    size_t cells_len = (size_t)map_msg->width * (size_t)map_msg->height;
    memcpy(GAME_MAP.cells, map_msg->cells, cells_len);
    resized = 1; /* trigger resize to redraw the board with new dimensions */

    for (uint16_t i = 0; i < cells_len; i++) {
        char cell = GAME_MAP.cells[i];
        if (cell >= '1' && cell <= '8') {
            uint8_t player_id = cell - '0';
            if (player_id == SELF_PLAYER.id) {
                SELF_PLAYER.row = i / GAME_MAP.width;
                SELF_PLAYER.col = i % GAME_MAP.width;
            } else {
                for (int j = 0; j < 7; j++) {
                    // game_log("Checking OTHER_PLAYERS[%d] with id %d against player_id %d", j, OTHER_PLAYERS[j].id, player_id);
                    if (OTHER_PLAYERS[j].id == player_id) {
                        OTHER_PLAYERS[j].row = i / GAME_MAP.width;
                        OTHER_PLAYERS[j].col = i % GAME_MAP.width;
                        // game_log("Setting player %d position to (%d, %d)", OTHER_PLAYERS[j].id, OTHER_PLAYERS[j].row, OTHER_PLAYERS[j].col);
                        break;
                    }
                }
            }
        }
    }

}

static void handle_hello(const msg_generic_t *header, const msg_hello_t *hello_msg) {
    (void)header; /* might be useful later */
    // (void)hello_msg; /* to silence unused parameter warning for now */
    for (int i = 0; i < 7; i++) {
        if (OTHER_PLAYERS[i].id == 0) {
            OTHER_PLAYERS[i].id = header->sender_id;
            strncpy(OTHER_PLAYERS[i].name, hello_msg->player_name, MAX_NAME_LEN);
            OTHER_PLAYERS[i].name[MAX_NAME_LEN] = '\0';
            OTHER_PLAYERS[i].lives = 1;
            break;
        }
    }

}

static void handle_welcome(const msg_generic_t *header, const msg_welcome_t *welcome_msg) {
    (void)header; /* might be useful later */
    // (void)welcome_msg; /* to silence unused parameter warning for now */

    // SELF_PLAYER.id = header->target_id;
    if (SELF_PLAYER.id == 0) {
        // game_log("Setting SELF_PLAYER.id to %d based on welcome message target_id %d", header->target_id, header->target_id);
        SELF_PLAYER.id = header->target_id;
    }

    // handle other players struct info
    int num_clients = welcome_msg->length / sizeof(welcome_client_t);
    // game_log("Received welcome message with %d clients", num_clients);
    for (int i = 0; i < num_clients; i++) {
        welcome_client_t client_info = welcome_msg->clients[i];
        if (client_info.id == SELF_PLAYER.id) {
            continue;
        }
        for (int j = 0; j < 7; j++) {
            if (OTHER_PLAYERS[j].id == client_info.id || OTHER_PLAYERS[j].id == 0) {
                OTHER_PLAYERS[j].id = client_info.id;
                // game_log("Updating OTHER_PLAYERS[%d] with id %d", j, OTHER_PLAYERS[j].id);
                strncpy(OTHER_PLAYERS[j].name, client_info.player_name, MAX_NAME_LEN);
                OTHER_PLAYERS[j].name[MAX_NAME_LEN] = '\0';
                OTHER_PLAYERS[j].ready = client_info.ready;
                OTHER_PLAYERS[j].lives = 1;
                break;
            }
        }
    }

}

static void handle_bonus_retrieved(const msg_generic_t *header, const msg_bonus_retrieved_t *bonus_msg) {
    (void)header; /* might be useful later */
    uint8_t player_id = bonus_msg->player_id;
    player_bonus_count[player_id - 1]++; // increment the bonus count for this player (player_id starts at 1)
}

static void handle_disconnect() {
    endwin();
    exit(0);
}

static void handle_error() {
    endwin();
    fprintf(stderr, "An error occurred while receiving data from the server.\n");
    exit(1);
}

static void dispatch(int fd, const msg_generic_t *header, const void *payload) {
    (void)fd; /* not needed for now, but might be useful later for messages that require a response */

    switch (header->msg_type) {
        case MSG_HELLO:
            handle_hello(header, (const msg_hello_t *)payload);
            break;
        case MSG_MAP:
            handle_map_message(header, (const msg_map_t *)payload);
            break;
        case MSG_MOVED:
            handle_moved(header, (const msg_moved_t *)payload);
            break;
        case MSG_BOMB:
            handle_bomb(header, (const msg_bomb_t *)payload);
            break;
        
        case MSG_EXPLOSION_START:
            handle_explosion_start(header, (const msg_explosion_start_t *)payload);
            break;

        case MSG_EXPLOSION_END:
            handle_explosion_end(header, (const msg_explosion_end_t *)payload);
            break;
        case MSG_DEATH:
            handle_death(header, (const msg_death_t *)payload);
            break;
        case MSG_WELCOME:
            handle_welcome(header, (const msg_welcome_t *)payload);
            break;
        case MSG_DISCONNECT:
            handle_disconnect();
            break;
        case MSG_BONUS_RETRIEVED:
            handle_bonus_retrieved(header, (const msg_bonus_retrieved_t *)payload);
            break;
    }
}

static void handle_server_messages(int fd)
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

static void handle_movement_input(int ch) {
    uint8_t direction;
    if (ch == KEY_UP)    { direction = 'U'; }
    if (ch == KEY_DOWN)  { direction = 'D'; }
    if (ch == KEY_LEFT)  { direction = 'L'; }
    if (ch == KEY_RIGHT) { direction = 'R'; }
    
    msg_move_attempt_t move_msg = { .direction = direction };
    send_move_attempt(CLIENT_FD, SELF_PLAYER.id, TARGET_SERVER, &move_msg);
}

static void handle_user_input(int ch) {
    if (ch != ERR) {
        if (ch == KEY_UP   || ch == KEY_DOWN 
         || ch == KEY_LEFT || ch == KEY_RIGHT) {
            handle_movement_input(ch);
        }
        else if (ch == ' ') {
            msg_bomb_attempt_t bomb_msg = { .cell_index = make_cell_index(SELF_PLAYER.row, SELF_PLAYER.col, GAME_MAP.width) };
            send_bomb_attempt(CLIENT_FD, SELF_PLAYER.id, TARGET_SERVER, &bomb_msg);
        }
        else if (ch == 'r' || ch == 'R') {
            SELF_PLAYER.ready = !SELF_PLAYER.ready;
            send_ready_message(CLIENT_FD, SELF_PLAYER.id, TARGET_SERVER);
        }
        else if (ch == 'q' || ch == 'Q') {
            send_leave_message(CLIENT_FD, SELF_PLAYER.id, TARGET_SERVER);
        }
        else if (ch == '1' || ch == '2' || ch == '3') {
            int n = snprintf(MAP_FILE_PATH, sizeof MAP_FILE_PATH, "maps/test_map_%c.txt", ch);
            if (n < 0 || n > 254) return;

            uint8_t len = (uint8_t)(n + 1);

            msg_choose_map_t *msg = malloc(sizeof(*msg) + len);
            if (!msg) return;

            msg->length = len;
            memcpy(msg->map_name, MAP_FILE_PATH, len);

            send_choose_map(CLIENT_FD, SELF_PLAYER.id, TARGET_SERVER, msg);
            free(msg);
        }
    }
}

static void resize_game_board() {
    BLOCK_SIZE = MAX_BLOCK_SIZE; /* reset block size to max on each resize */

    endwin();
    refresh();
    werase(TERMINAL_WIN);

    /* calculate maximum possible BLOCK_SIZE */
    while (BLOCK_SIZE > 1) {
        /* extra padding (+6 +8) for pleasant warping */
        if (COLS  < SIDE_BAR_WIDTH  + GAME_MAP.width * BLOCK_SIZE * DOT_PADDING + 8
         || LINES < SIDE_BAR_HEIGHT + 6
         || LINES < GAME_MAP.height * BLOCK_SIZE + 6) {
            BLOCK_SIZE -= 1;
            continue;
        }
        break;
    }

    int16_t map_offset_x = (COLS - (GAME_MAP.width * BLOCK_SIZE * DOT_PADDING + SIDE_BAR_WIDTH)) / 2;
    int16_t map_offset_y = (LINES - GAME_MAP.height * BLOCK_SIZE) / 2;
    int16_t sidebar_offset_x = map_offset_x + GAME_MAP.width * BLOCK_SIZE * DOT_PADDING + 2;
    int16_t sidebar_offset_y = (LINES - SIDE_BAR_HEIGHT) / 2;
    int16_t shared_offset_y = map_offset_y < sidebar_offset_y ? map_offset_y : sidebar_offset_y;
  
    /* box around terminal */
    box(TERMINAL_WIN, 0, 0);

    /* create derived window for map */
    MAP_WIN = derwin(
        TERMINAL_WIN, 
        GAME_MAP.height * BLOCK_SIZE + 2, 
        GAME_MAP.width * BLOCK_SIZE * DOT_PADDING + 2, 
        shared_offset_y, 
        map_offset_x
    );
    box(MAP_WIN, 0, 0);

    /* create derived window for sidebar (player info) */
    SIDEBAR_WIN = derwin(
        TERMINAL_WIN, 
        SIDE_BAR_HEIGHT, 
        SIDE_BAR_WIDTH, 
        shared_offset_y, 
        sidebar_offset_x
    );
    box(SIDEBAR_WIN, 0, 0);
}

/*  separate from resize handler, because doesn't resize the box or symbol size,
    more efficient performance wise. */
static void draw_game_board() {
    /* die.net man: When using this routine, it is necessary to call touchwin 
    or touchline on orig before calling wrefresh on the subwindow.
        - wrefresh works for me, touchline causes weird behaviour. */
    wrefresh(TERMINAL_WIN);

    /*-------------------------------------------------------------------*/
    /*                          MAIN GAME BOARD                          */
    /*-------------------------------------------------------------------*/

    if (BLOCK_SIZE < 5) {
        for (int i = 0; i < GAME_MAP.height; i++) {
            for (int j = 0; j < GAME_MAP.width; j++) {
                char cell = GAME_MAP.cells[i * GAME_MAP.width + j];
                if (cell == '.') { cell = ' '; } /* clear empty cells */

                for (int k = 0; k < BLOCK_SIZE; k++) {
                    for (int l = 0; l < BLOCK_SIZE; l++) {
                        int y = i * BLOCK_SIZE + k + 1;
                        int x = j * BLOCK_SIZE * 2 + l * 2 + 1;
        
                        mvwaddch(MAP_WIN, y, x, cell);     // main char
                        mvwaddch(MAP_WIN, y, x + 1, ' ');  // spacer
                    }
                }
            }
        }
    } else { /* draw special objects */
        for (int i = 0; i < GAME_MAP.height; i++) {
            for (int j = 0; j < GAME_MAP.width; j++) {
                char cell = GAME_MAP.cells[i * GAME_MAP.width + j];
                // if (cell == '.') { continue; } /* skip empty cells */

                switch (cell) {
                    case '.':
                        /* can't skip empty cells, they have to clear old state */
                        for (int k = 0; k < BLOCK_SIZE; k++) {
                            for (int l = 0; l < BLOCK_SIZE; l++) {
                                int y = i * BLOCK_SIZE + k + 1;
                                int x = j * BLOCK_SIZE * 2 + l * 2 + 1;

                                mvwaddch(MAP_WIN, y, x,     ' ');
                                mvwaddch(MAP_WIN, y, x + 1, ' ');
                            }
                        }
                        break; 

                    /* BOMB CASES */
                    case '@': 
                        /* empty corners, it's the center of the explosion */
                        for (int k = 0; k < BLOCK_SIZE; k++) {
                            for (int l = 0; l < BLOCK_SIZE; l++) {
                                int is_corner = (k == 0 && l == 0) || (k == 0 && l == BLOCK_SIZE - 1) ||
                                                (k == BLOCK_SIZE - 1 && l == 0) || (k == BLOCK_SIZE - 1 && l == BLOCK_SIZE - 1);

                                int y = i * BLOCK_SIZE + k + 1;
                                int x = j * BLOCK_SIZE * 2 + l * 2 + 1;

                                if (is_corner) {
                                    mvwaddch(MAP_WIN, y, x,     ' ');
                                    mvwaddch(MAP_WIN, y, x + 1, ' ');
                                } else {
                                    mvwaddch(MAP_WIN, y, x,     ' ' | A_REVERSE);
                                    mvwaddch(MAP_WIN, y, x + 1, ' ' | A_REVERSE);
                                }
                            }
                        }
                        break;
                    case '-':
                        /* bomb laser horizontally, slim up and below */
                        for (int k = 0; k < BLOCK_SIZE; k++) {
                            for (int l = 0; l < BLOCK_SIZE; l++) {
                                int is_top_or_bottom = (k == 0 || k == BLOCK_SIZE - 1);

                                int y = i * BLOCK_SIZE + k + 1;
                                int x = j * BLOCK_SIZE * 2 + l * 2 + 1;

                                if (is_top_or_bottom) {
                                    mvwaddch(MAP_WIN, y, x,     ' ');
                                    mvwaddch(MAP_WIN, y, x + 1, ' ');
                                } else {
                                    mvwaddch(MAP_WIN, y, x,     ' ' | A_REVERSE);
                                    mvwaddch(MAP_WIN, y, x + 1, ' ' | A_REVERSE);
                                }
                            }
                        }
                        break;
                    case '>':
                        /* bomb laser terminator to the right */
                        for (int k = 0; k < BLOCK_SIZE; k++) {
                            for (int l = 0; l < BLOCK_SIZE; l++) {
                                int is_top_bottom_right = (k == 0 || k == BLOCK_SIZE - 1 || l == BLOCK_SIZE - 1);

                                int y = i * BLOCK_SIZE + k + 1;
                                int x = j * BLOCK_SIZE * 2 + l * 2 + 1;

                                if (is_top_bottom_right) {
                                    mvwaddch(MAP_WIN, y, x,     ' ');
                                    mvwaddch(MAP_WIN, y, x + 1, ' ');
                                } else {
                                    mvwaddch(MAP_WIN, y, x,     ' ' | A_REVERSE);
                                    mvwaddch(MAP_WIN, y, x + 1, ' ' | A_REVERSE);
                                }
                            }
                        }
                        break;
                    case '<':	
                        /* bomb laser terminator to the left */
                        for (int k = 0; k < BLOCK_SIZE; k++) {
                            for (int l = 0; l < BLOCK_SIZE; l++) {
                                int is_top_bottom_left = (k == 0 || k == BLOCK_SIZE - 1 || l == 0);

                                int y = i * BLOCK_SIZE + k + 1;
                                int x = j * BLOCK_SIZE * 2 + l * 2 + 1;

                                if (is_top_bottom_left) {
                                    mvwaddch(MAP_WIN, y, x,     ' ');
                                    mvwaddch(MAP_WIN, y, x + 1, ' ');
                                } else {
                                    mvwaddch(MAP_WIN, y, x,     ' ' | A_REVERSE);
                                    mvwaddch(MAP_WIN, y, x + 1, ' ' | A_REVERSE);
                                }
                            }
                        }
                        break;
                    case '|':
                        /* bomb laser vertically, slim right and left */
                        for (int k = 0; k < BLOCK_SIZE; k++) {
                            for (int l = 0; l < BLOCK_SIZE; l++) {
                                int is_left_or_right = (l == 0 || l == BLOCK_SIZE - 1);

                                int y = i * BLOCK_SIZE + k + 1;
                                int x = j * BLOCK_SIZE * 2 + l * 2 + 1;

                                if (is_left_or_right) {
                                    mvwaddch(MAP_WIN, y, x,     ' ');
                                    mvwaddch(MAP_WIN, y, x + 1, ' ');
                                } else {
                                    mvwaddch(MAP_WIN, y, x,     ' ' | A_REVERSE);
                                    mvwaddch(MAP_WIN, y, x + 1, ' ' | A_REVERSE);
                                }
                            }
                        }
                        break;
                    case '^':
                        /* bomb laser terminator above, slim left,right,above */
                        for (int k = 0; k < BLOCK_SIZE; k++) {
                            for (int l = 0; l < BLOCK_SIZE; l++) {
                                int is_left_right_or_top = (l == 0 || l == BLOCK_SIZE - 1 || k == 0);

                                int y = i * BLOCK_SIZE + k + 1;
                                int x = j * BLOCK_SIZE * 2 + l * 2 + 1;

                                if (is_left_right_or_top) {
                                    mvwaddch(MAP_WIN, y, x,     ' ');
                                    mvwaddch(MAP_WIN, y, x + 1, ' ');
                                } else {
                                    mvwaddch(MAP_WIN, y, x,     ' ' | A_REVERSE);
                                    mvwaddch(MAP_WIN, y, x + 1, ' ' | A_REVERSE);
                                }
                            }
                        }
                        break;
                    case 'v':
                        /* bomb laser terminator below, slim left,right,below */
                        for (int k = 0; k < BLOCK_SIZE; k++) {
                            for (int l = 0; l < BLOCK_SIZE; l++) {
                                int is_left_right_or_bottom = (l == 0 || l == BLOCK_SIZE - 1 || k == BLOCK_SIZE - 1);

                                int y = i * BLOCK_SIZE + k + 1;
                                int x = j * BLOCK_SIZE * 2 + l * 2 + 1;

                                if (is_left_right_or_bottom) {
                                    mvwaddch(MAP_WIN, y, x,     ' ');
                                    mvwaddch(MAP_WIN, y, x + 1, ' ');
                                } else {
                                    mvwaddch(MAP_WIN, y, x,     ' ' | A_REVERSE);
                                    mvwaddch(MAP_WIN, y, x + 1, ' ' | A_REVERSE);
                                }
                            }
                        }
                        break;

                    case 'H': 
                        /* hard wall -> full border */
                        for (int k = 0; k < BLOCK_SIZE; k++) {
                            for (int l = 0; l < BLOCK_SIZE; l++) {
                                int is_border = (k == 0 || k == BLOCK_SIZE - 1 ||
                                                l == 0 || l == BLOCK_SIZE - 1);

                                int y = i * BLOCK_SIZE + k + 1;
                                int x = j * BLOCK_SIZE * 2 + l * 2 + 1;

                                if (is_border) {
                                    mvwaddch(MAP_WIN, y, x,     ' ' | A_REVERSE);
                                    mvwaddch(MAP_WIN, y, x + 1, ' ' | A_REVERSE);
                                } else {
                                    mvwaddch(MAP_WIN, y, x, cell);
                                    mvwaddch(MAP_WIN, y, x + 1, '.');
                                }
                            }
                        }
                        break;
                    
                    case 't': 
                    /* bomb explosion laser -> no border */
                    for (int k = 0; k < BLOCK_SIZE; k++) {
                        for (int l = 0; l < BLOCK_SIZE; l++) {
                            int is_border = (k == 0 || k == BLOCK_SIZE - 1 ||
                                            l == 0 || l == BLOCK_SIZE - 1);

                            int y = i * BLOCK_SIZE + k + 1;
                            int x = j * BLOCK_SIZE * 2 + l * 2 + 1;

                            if (is_border) {
                                mvwaddch(MAP_WIN, y, x,     ' ');
                                mvwaddch(MAP_WIN, y, x + 1, ' ');
                            } else {
                                mvwaddch(MAP_WIN, y, x, ' ' | A_REVERSE);
                                mvwaddch(MAP_WIN, y, x + 1, ' ' | A_REVERSE);
                            }
                        }
                    }
                    break;

                    /* ! ? @ $ ^ & ~ < .... too hard to see, might as well use numbers */
                    case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8':
                        // const char player_symbols[] = "!?#$^&~<";
                        // char player_cell = player_symbols[cell - '1'];
                        char player_cell = cell;
                        /* player -> with corners and filled with respective ID symbol */
                        for (int k = 0; k < BLOCK_SIZE; k++) {
                            for (int l = 0; l < BLOCK_SIZE; l++) {
                                int is_corner = (k == 0 && l == 0) || (k == 0 && l == BLOCK_SIZE - 1) ||
                                                (k == BLOCK_SIZE - 1 && l == 0) || (k == BLOCK_SIZE - 1 && l == BLOCK_SIZE - 1);

                                int y = i * BLOCK_SIZE + k + 1;
                                int x = j * BLOCK_SIZE * 2 + l * 2 + 1;

                                if (is_corner) {
                                    mvwaddch(MAP_WIN, y, x,     ' ' | A_REVERSE);
                                    mvwaddch(MAP_WIN, y, x + 1, ' ' | A_REVERSE);
                                } else {
                                    mvwaddch(MAP_WIN, y, x, player_cell);
                                    mvwaddch(MAP_WIN, y, x + 1, '.');
                                }
                            }
                        }
                        break;

                    default:
                        /* only corners, don't fill in sides */
                        for (int k = 0; k < BLOCK_SIZE; k++) {
                            for (int l = 0; l < BLOCK_SIZE; l++) {
                                int is_corner = (k == 0 && l == 0) || (k == 0 && l == BLOCK_SIZE - 1) ||
                                                (k == BLOCK_SIZE - 1 && l == 0) || (k == BLOCK_SIZE - 1 && l == BLOCK_SIZE - 1);
                                int is_border = (k == 0 || k == BLOCK_SIZE - 1 ||
                                                l == 0 || l == BLOCK_SIZE - 1);

                                int y = i * BLOCK_SIZE + k + 1;
                                int x = j * BLOCK_SIZE * 2 + l * 2 + 1;

                                if (is_corner) {
                                    mvwaddch(MAP_WIN, y, x,     ' ' | A_REVERSE);
                                    mvwaddch(MAP_WIN, y, x + 1, ' ' | A_REVERSE);
                                } else if (is_border) {
                                    mvwaddch(MAP_WIN, y, x,     '.');
                                    mvwaddch(MAP_WIN, y, x + 1, '.');
                                } else {
                                    mvwaddch(MAP_WIN, y, x, cell);
                                    mvwaddch(MAP_WIN, y, x + 1, ' ');
                                }
                            }
                        }

                }
            }
        }
    }

    wrefresh(MAP_WIN);

    
    /*-------------------------------------------------------------------*/
    /*                         PLAYER NAME BOARD                         */
    /*-------------------------------------------------------------------*/

    // Reset the window
    werase(SIDEBAR_WIN);
    box(SIDEBAR_WIN, 0, 0);

    char line[SIDE_BAR_WIDTH]; 
    int current_row = 1;

    // Self players
    wattron(SIDEBAR_WIN, A_REVERSE); 
    snprintf(line, SIDE_BAR_WIDTH - 2, " %-2d %-.*s", 
             SELF_PLAYER.id, MAX_NAME_LEN, SELF_PLAYER.name);
    mvwaddstr(SIDEBAR_WIN, current_row++, 1, line);
    wattroff(SIDEBAR_WIN, A_REVERSE);

    // Self stats line
    snprintf(line, SIDE_BAR_WIDTH - 2, " [%c] Bonuses: %d", 
             SELF_PLAYER.ready ? 'R' : ' ', 
             player_bonus_count[SELF_PLAYER.id-1]);
    mvwaddstr(SIDEBAR_WIN, current_row++, 1, line);

    current_row++; 

    // Other players
    for (int i = 0; i < MAX_PLAYERS - 1; i++) {
        if (current_row >= SIDE_BAR_HEIGHT - 6) break;

        player_t p = OTHER_PLAYERS[i];
        // game_log("Rendering OTHER_PLAYERS[%d]: id=%d, name=%s, lives=%d, ready=%d", i, p.id, p.name, p.lives, p.ready);
        if (p.lives > 0) {
            snprintf(line, SIDE_BAR_WIDTH - 2, "#%d %-.*s Bonuses:%d", 
                     p.id, 
                     SIDE_BAR_WIDTH - 15, p.name,
                     player_bonus_count[p.id-1]);
            mvwaddstr(SIDEBAR_WIN, current_row++, 1, line);
        } else {
            wattron(SIDEBAR_WIN, A_DIM);
            mvwaddstr(SIDEBAR_WIN, current_row++, 1, " -- Empty --");
            wattroff(SIDEBAR_WIN, A_DIM);
        }
    }

    // Controls, bottom aligned
    int footer_start = SIDE_BAR_HEIGHT - 8;
    
    mvwhline(SIDEBAR_WIN, footer_start++, 1, ACS_HLINE, SIDE_BAR_WIDTH - 2);
    
    wattron(SIDEBAR_WIN, A_BOLD);
    mvwaddstr(SIDEBAR_WIN, footer_start++, 2, "CONTROLS:");
    wattroff(SIDEBAR_WIN, A_BOLD);

    mvwaddstr(SIDEBAR_WIN, footer_start++, 2, "R            - Ready");
    mvwaddstr(SIDEBAR_WIN, footer_start++, 2, "Q            - Leave Game");
    mvwaddstr(SIDEBAR_WIN, footer_start++, 2, "Arrow Keys   - Move");
    mvwaddstr(SIDEBAR_WIN, footer_start++, 2, "SPACE        - Place Bomb");
    mvwaddstr(SIDEBAR_WIN, footer_start++, 2, "[1|2|3]      - Choose Map");

    // // Re-draw the box in case hline or strings touched the edges
    box(SIDEBAR_WIN, 0, 0);
    wrefresh(SIDEBAR_WIN);
}
