#pragma once
#include "services/bobo.h"

typedef struct { const char *name, *observation, *memory; } bobo_card_text_t;
extern const char *const bobo_destinations[BOBO_DESTINATION_COUNT];
extern const char *const bobo_tools[3];
extern const char *const bobo_item_names[5];
extern const bobo_card_text_t bobo_cards[BOBO_CARD_COUNT];
const char *bobo_title(const bobo_state_t *s);
const char *bobo_growth_name(const bobo_state_t *s);
