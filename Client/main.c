#include <ncurses.h>
#include <stdlib.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

#include "../config.h"
#include "../msg_protocol.h"

/*  2 to use '.' symbols in between chars for padding, only option */
#define DOT_PADDING 2 
/*  max amount of ASCII symbols representing a single element on the map 
    - lower means more efficient bounds checking */
#define MAX_BLOCK_SIZE 8 
/* player name +(4chars) character symbol on the map (1 char) +(4chars) their score (10 chars) */
#define SIDE_BAR_WIDTH MAX_NAME_LEN + 4 + 1 + 4 + 10
/* padding + max 8 player names +(2chars) start button + padding */
#define SIDE_BAR_HEIGHT 1 + 8 + 2 + 1 + 1

WINDOW *TERMINAL_WIN = NULL, *SIDEBAR_WIN = NULL, *MAP_WIN = NULL;
volatile sig_atomic_t resized = 1; /* 1 to enter loop on start */
uint8_t BLOCK_SIZE;



typedef struct {
    uint16_t width;
    uint16_t height;
    char *data;
} GameMap;

GameMap GAME_MAP;

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
    /* -----------------test-input-------------------- */

    initscr();
    cbreak();       /* disables line buffering */
    noecho();       /* don't echo user input back */
    curs_set(0);    /* hides cursor */
    TERMINAL_WIN = newwin(LINES, COLS, 0, 0);


    signal(SIGWINCH, resize_handler);
    while (1) {
        /* handle terminal/screen resizing */
        if (resized) {
            resize_game_board();
            resized = 0;
        }

        draw_game_board();
        usleep(1000000 / TICKS_PER_SECOND); /* 1e6 for microseconds */
    }


    return 0;
}




/* -------------------------- function declarations --------------------------- */

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

                    case 'S': 
                        /* soft wall -> only corners */
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
                        break;

                    default: /* give corners but don't empty the sides of the square */
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
                                    mvwaddch(MAP_WIN, y, x, cell);
                                    mvwaddch(MAP_WIN, y, x + 1, '.');
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

    wrefresh(SIDEBAR_WIN);
}
