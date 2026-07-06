#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GOMOKU_BOARD_SIZE 15

typedef enum {
    GOMOKU_CELL_EMPTY = 0,
    GOMOKU_CELL_BLACK = 1,
    GOMOKU_CELL_WHITE = 2,
} gomoku_cell_t;

typedef struct {
    gomoku_cell_t board[GOMOKU_BOARD_SIZE][GOMOKU_BOARD_SIZE];

    gomoku_cell_t current_player;
    gomoku_cell_t winner;

    bool game_over;
    uint16_t move_count;

    int last_row;
    int last_col;
} gomoku_game_t;

void gomoku_game_init(gomoku_game_t *game);
void gomoku_game_reset(gomoku_game_t *game);

bool gomoku_game_place(gomoku_game_t *game, int row, int col);

gomoku_cell_t gomoku_game_get_cell(const gomoku_game_t *game, int row, int col);
gomoku_cell_t gomoku_game_get_current_player(const gomoku_game_t *game);
gomoku_cell_t gomoku_game_get_winner(const gomoku_game_t *game);

bool gomoku_game_is_over(const gomoku_game_t *game);
bool gomoku_game_is_draw(const gomoku_game_t *game);

const char *gomoku_cell_to_text(gomoku_cell_t cell);

#ifdef __cplusplus
}
#endif