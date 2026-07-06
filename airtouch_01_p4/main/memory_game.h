#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEMORY_CARD_ROWS             3
#define MEMORY_CARD_COLS             4
#define MEMORY_CARD_COUNT            (MEMORY_CARD_ROWS * MEMORY_CARD_COLS)
#define MEMORY_PAIR_COUNT            (MEMORY_CARD_COUNT / 2)
#define MEMORY_MISMATCH_HIDE_MS      650

typedef enum {
    MEMORY_GAME_READY = 0,
    MEMORY_GAME_PLAYING,
    MEMORY_GAME_WIN,
} memory_game_state_t;

typedef enum {
    MEMORY_CARD_APPLE = 0,
    MEMORY_CARD_STAR,
    MEMORY_CARD_HEART,
    MEMORY_CARD_CHESS,
    MEMORY_CARD_MOON,
    MEMORY_CARD_FLOWER,
    MEMORY_CARD_TYPE_COUNT,
} memory_card_type_t;

typedef struct {
    memory_card_type_t type;
    bool revealed;
    bool matched;
} memory_card_t;

typedef struct {
    memory_card_t cards[MEMORY_CARD_COUNT];

    memory_game_state_t state;

    int first_index;
    int second_index;

    bool waiting_hide;
    uint32_t hide_time_ms;

    uint16_t move_count;
    uint8_t match_count;

    uint32_t start_ms;
    uint32_t finish_ms;

    uint32_t rng_state;
} memory_game_t;

void memory_game_init(memory_game_t *game, uint32_t seed);
void memory_game_reset(memory_game_t *game, uint32_t seed);

void memory_game_update(memory_game_t *game, uint32_t now_ms);

bool memory_game_flip_card(memory_game_t *game, uint8_t index, uint32_t now_ms);

bool memory_game_is_win(const memory_game_t *game);
bool memory_game_is_waiting_hide(const memory_game_t *game);

const memory_card_t *memory_game_get_card(const memory_game_t *game, uint8_t index);

uint16_t memory_game_get_move_count(const memory_game_t *game);
uint8_t memory_game_get_match_count(const memory_game_t *game);
uint32_t memory_game_get_elapsed_ms(const memory_game_t *game, uint32_t now_ms);

#ifdef __cplusplus
}
#endif