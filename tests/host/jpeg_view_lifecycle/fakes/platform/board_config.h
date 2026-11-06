#pragma once
#define PERIPH_DISPLAY 1
// platform_factory.h is the real production API. Its board-specific configuration
// is not instantiated by this display-lifecycle test.
typedef struct { int unused; } board_config_t;
