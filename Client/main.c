#include <ncurses.h>
#include <stdlib.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include <sys/socket.h>
#include <arpa/inet.h>  

#include "../config.h"
#include "../msg_protocol.h"

/*  2 to use '.' symbols in between chars for padding, only option */
#define DOT_PADDING 2 
/*  max amount of ASCII symbols representing a single element on the map 
    - lower means more efficient bounds checking */
#define MAX_BLOCK_SIZE 8 
/* player name +(4chars) character symbol on the map (1 char) +(4chars) their score (10 chars) + 4 for borders */
#define SIDE_BAR_WIDTH MAX_NAME_LEN + 4 + 1 + 4 + 10 + 4
/* padding + max 8 player names +(2chars) start button + padding */
#define SIDE_BAR_HEIGHT 1 + 8 + 2 + 1 + 1

WINDOW *TERMINAL_WIN = NULL, *SIDEBAR_WIN = NULL, *MAP_WIN = NULL;
volatile sig_atomic_t resized = 1; /* 1 to enter loop on start */
uint8_t BLOCK_SIZE;

int CLIENT_FD; /* global client socket file descriptor for sending messages to server */

typedef struct {
    uint16_t width;
    uint16_t height;
    char *data;
} GameMap;

GameMap GAME_MAP;

void handle_user_input();
void draw_game_board();
void resize_game_board();

void resize_handler(int sig) {
    resized = 1;
}



int main() {
    uint8_t c, height, width;

    /* -----------------test-input-------------------- */
    FILE *fp = fopen("maps/example_map_config.txt", "r");
    if (!fp) return 1;

    fscanf(fp, "%hhd %hhd", &height, &width);

    /* skip 4 characters while testing */
    for (int i = 0; i < 4; i++) {
        fscanf(fp, " %c", &c);
    }

    char *map_data = malloc(height * width + 1);

    for (int i = 0; i < height * width; i++)
        fscanf(fp, " %c", &map_data[i]);
    map_data[height * width] ='\0';

    fclose(fp);

    GAME_MAP.width = width;
    GAME_MAP.height = height;
    GAME_MAP.data = map_data; 

    /* debug: print the map */
    // printf("Map %dx%d:\n", GAME_MAP.width, GAME_MAP.height);
    // for (int i = 0; i < GAME_MAP.height; i++) {
    //     for (int j = 0; j < GAME_MAP.width; j++) {
    //         printf("%c ", GAME_MAP.data[i * GAME_MAP.width + j]);
    //     }
    //     printf("\n");
    // }
    // getchar(); /* wait for input before starting ncurses */
    /* -----------------test-input-------------------- */


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

    // /* send test data */
    // send(CLIENT_FD, "Hello, Server!", 14, 0);
    // send(CLIENT_FD, "Ping", 4, 0);
    // send(CLIENT_FD, "Test message 1", 14, 0);
    // send(CLIENT_FD, "How are you?", 12, 0);
    // send(CLIENT_FD, "1234567890", 10, 0);

    
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

        int ch = getch();
        handle_user_input(ch);

        draw_game_board();

        usleep(1000000 / TICKS_PER_SECOND); /* 1e6 for microseconds */
    }


    return 0;
}




/* -------------------------- function declarations --------------------------- */

void handle_movement_input(int ch) {
    uint8_t direction;
    if (ch == KEY_UP)    { direction = 'U'; }
    if (ch == KEY_DOWN)  { direction = 'D'; }
    if (ch == KEY_LEFT)  { direction = 'L'; }
    if (ch == KEY_RIGHT) { direction = 'R'; }
    
    /* TODO: wrap this in the generic message struct later */
    /* send movement attempt to server */
    msg_move_attempt_t move_msg = { .direction = direction };
    send(CLIENT_FD, &move_msg, sizeof(move_msg), 0);
}

void handle_user_input(int ch) {
    if (ch != ERR) {
        if (ch == KEY_UP   || ch == KEY_DOWN 
         || ch == KEY_LEFT || ch == KEY_RIGHT) {
            handle_movement_input(ch);
        }
        if (ch == ' ')       { /* space - place bomb */ }
        if (ch == 27)        { /* ESC */ exit(1); }
    }
}

void resize_game_board() {
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
void draw_game_board() {
    /* die.net man: When using this routine, it is necessary to call touchwin 
    or touchline on orig before calling wrefresh on the subwindow.
        - wrefresh works for me, touchline causes weird behaviour. */
    wrefresh(TERMINAL_WIN);

    /*-------------------------------------------------------------------*/
    /*                          MAIN GAME BOARD                          */
    /*-------------------------------------------------------------------*/
    uint16_t offset_y = (LINES - GAME_MAP.height) / 2;
    uint16_t offset_x = (COLS - GAME_MAP.width) / 2;

    if (BLOCK_SIZE < 5) {
        for (int i = 0; i < GAME_MAP.height; i++) {
            for (int j = 0; j < GAME_MAP.width; j++) {
                char cell = GAME_MAP.data[i * GAME_MAP.width + j];
                if (cell == '.') { continue; } /* skip empty cells */

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
                char cell = GAME_MAP.data[i * GAME_MAP.width + j];
                if (cell == '.') { continue; } /* skip empty cells */

                switch (cell) {
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
