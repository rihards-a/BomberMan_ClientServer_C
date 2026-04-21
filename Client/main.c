#include <ncurses.h>
#include <stdlib.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include <sys/socket.h>
#include <arpa/inet.h>  

#include "../net.h"
#include "../config.h"
#include "../msg_protocol.h"

/*  2 to use '.' symbols in between chars for padding, only option */
#define DOT_PADDING 2 
/*  max amount of ASCII symbols representing a single element on the map - lower means more efficient bounds checking */
#define MAX_BLOCK_SIZE 8 
/* player name +(4chars) character symbol on the map (1 char) +(4chars) their score (10 chars) + 4 for borders */
#define SIDE_BAR_WIDTH MAX_NAME_LEN + 4 + 1 + 4 + 10 + 4
/* padding + max 8 player names +(2chars) start button + padding */
#define SIDE_BAR_HEIGHT 1 + 8 + 2 + 1 + 1

WINDOW *TERMINAL_WIN = NULL, *SIDEBAR_WIN = NULL, *MAP_WIN = NULL;
uint8_t BLOCK_SIZE;
int CLIENT_FD; /* global client socket file descriptor for sending messages to server */
GameMap GAME_MAP;

volatile sig_atomic_t resized = 1; /* 1 to enter loop on start */
void resize_handler(int sig) {
    (void)sig; /* compiler happy :) */
    resized = 1;
}

player_t TEST_PLAYER = {
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

static void handle_user_input(int ch);
static void draw_game_board();
static void resize_game_board();
static void handle_disconnect();
static void handle_error();
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

    /* receive map data */
    /* implement init receiving later, for now const until receive the server map in loop */
    /* the size has to be at least equal to the map the server will send, 
        otherwise will overflow from memcpy later on */
    GAME_MAP.height = 6;
    GAME_MAP.width = 9;
    GAME_MAP.cells = malloc(GAME_MAP.height * GAME_MAP.width);
        /* . = empty, H = hard wall, 1-8 = player IDs */
    char *test_map = "........."
                     ".H.H.H.H."
                     ".1......."
                     ".H.H.H.H."
                     ".2......."
                     ".........";
    memcpy(GAME_MAP.cells, test_map, 54);
    /* implement init receiving later, for now const until receive the server map in loop */
    

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
    /* for testing, just clear the explosion center */
    GAME_MAP.cells[expl_end->cell_index] = '.';
}

static void handle_explosion_start(const msg_generic_t *header, const msg_explosion_start_t *expl_start) {
    (void)header; /* might be useful later */
    /* for testing, just mark the explosion center */
    GAME_MAP.cells[expl_start->cell_index] = 't';
}

static void handle_bomb(const msg_generic_t *header, const msg_bomb_t *bomb_msg) {
    (void)header; /* might be useful later */
    /* for testing, just mark the bomb on the map with a 'B' */
    GAME_MAP.cells[bomb_msg->cell_index] = 'B';
}

static void handle_moved(const msg_generic_t *header, const msg_moved_t *moved_msg) {
    (void)header; /* might be useful later */
    uint16_t cur_pos = make_cell_index(TEST_PLAYER.row, TEST_PLAYER.col, GAME_MAP.width);

    /* find old position of the player and clear it */
    if (GAME_MAP.cells[cur_pos] == '0' + moved_msg->player_id) {
        GAME_MAP.cells[cur_pos] = '.';
    }

    /* check if it's for self */
    if (moved_msg->player_id == TEST_PLAYER.id) {
        /* update test player position */
        TEST_PLAYER.row = moved_msg->cell_index / GAME_MAP.width;
        TEST_PLAYER.col = moved_msg->cell_index % GAME_MAP.width;
    }
    
    uint8_t player_id = moved_msg->player_id;
    uint16_t cell_index = moved_msg->cell_index;
    /* set new position of the player */
    GAME_MAP.cells[cell_index] = '0' + player_id;
}

static void handle_sync_board(const msg_generic_t *header, const msg_map_t *map_msg) {
    (void)header; /* might be useful later */
    /* update local game map with new data */
    GAME_MAP.width = map_msg->width;
    GAME_MAP.height = map_msg->height;
    size_t cells_len = (size_t)map_msg->width * (size_t)map_msg->height;
    memcpy(GAME_MAP.cells, map_msg->cells, cells_len);
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
        case MSG_SYNC_BOARD:
            handle_sync_board(header, (const msg_map_t *)payload);
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
    send_move_attempt(CLIENT_FD, 1, 255, &move_msg);
}

static void handle_user_input(int ch) {
    if (ch != ERR) {
        if (ch == KEY_UP   || ch == KEY_DOWN 
         || ch == KEY_LEFT || ch == KEY_RIGHT) {
            handle_movement_input(ch);
        }
        if (ch == ' ') {
            msg_bomb_attempt_t bomb_msg = { .cell_index = make_cell_index(TEST_PLAYER.row, TEST_PLAYER.col, GAME_MAP.width) };
            send_bomb_attempt(CLIENT_FD, TEST_PLAYER.id, 255, &bomb_msg);
        }
        if (ch == 27)        { /* ESC */ exit(1); }
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
    // uint16_t offset_y = (LINES - GAME_MAP.height) / 2;
    // uint16_t offset_x = (COLS - GAME_MAP.width) / 2;

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

    /* TODO */
    for (int i = 1; i < MAX_PLAYERS+1; i++) {
        mvwaddnstr(SIDEBAR_WIN, i, 1, "your 30 username aaaaaaaaaaaa", 30);
        mvwaddnstr(SIDEBAR_WIN, i, 31, " __ ", 4);
        mvwaddch(SIDEBAR_WIN, i, 36, '@'); /* ! ? @ $ % & ~ < */
        mvwaddnstr(SIDEBAR_WIN, i, 37, " __ ", 4);
        mvwaddnstr(SIDEBAR_WIN, i, 42, "0123456789", 10);
    }
    mvwaddnstr(SIDEBAR_WIN, SIDE_BAR_HEIGHT-3, (SIDE_BAR_WIDTH - strlen("Latest winner: Zebiekste"))/2 -1, "Latest winner: Zebiekste", 40);
    mvwaddnstr(SIDEBAR_WIN, SIDE_BAR_HEIGHT-2, (SIDE_BAR_WIDTH - strlen("Press ESC to return / SPACE to restart"))/2 -1, "Press ESC to return / SPACE to restart", 40);

    wrefresh(SIDEBAR_WIN);
}
