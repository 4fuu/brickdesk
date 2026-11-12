#ifndef BRICKDESK_TEST_SCENARIO_H
#define BRICKDESK_TEST_SCENARIO_H

#include <stdbool.h>
#include <stddef.h>

#include "brickdesk/input.h"

#define BD_SCENARIO_MAX_STEPS 1024

typedef enum bd_step_type {
  BD_STEP_WAIT = 0,
  BD_STEP_KEY,
  BD_STEP_SCREENSHOT
} bd_step_type;

typedef struct bd_step {
  bd_step_type type;
  int frames;      /* BD_STEP_WAIT */
  bd_key key;      /* BD_STEP_KEY */
  bool pressed;    /* BD_STEP_KEY */
  char screenshot[64]; /* BD_STEP_SCREENSHOT */
} bd_step;

typedef struct bd_scenario {
  char name[64];
  int width;
  int height;
  int fixed_dt_ms;
  bd_step steps[BD_SCENARIO_MAX_STEPS];
  int step_count;
} bd_scenario;

/*
 * Loads a scenario JSON file. On failure returns false and writes a
 * human-readable message (including the step index when relevant) to error.
 */
bool bd_scenario_load(const char *path, bd_scenario *out, char *error,
                      size_t error_size);

#endif /* BRICKDESK_TEST_SCENARIO_H */
