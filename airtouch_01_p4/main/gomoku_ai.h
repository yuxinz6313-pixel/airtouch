#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "gomoku_game.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GOMOKU_AI_DIFFICULTY_EASY = 0,
    GOMOKU_AI_DIFFICULTY_NORMAL,
    GOMOKU_AI_DIFFICULTY_HARD,
} gomoku_ai_difficulty_t;

bool gomoku_ai_find_best_move(
    const gomoku_game_t *game,
    gomoku_cell_t ai_player,
    gomoku_ai_difficulty_t difficulty,
    int *out_row,
    int *out_col
);

#ifdef __cplusplus
}
#endif