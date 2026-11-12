#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "brickdesk/input.h"

int main(void) {
  int failures = 0;

  for (int i = 1; i < (int)BD_KEY_COUNT; ++i) {
    const bd_key key = (bd_key)i;
    const char *name = bd_key_name(key);
    if (name == NULL || strcmp(name, "UNKNOWN") == 0) {
      fprintf(stderr, "key_names: key %d has no name\n", i);
      ++failures;
      continue;
    }
    if (bd_key_from_name(name) != key) {
      fprintf(stderr, "key_names: '%s' did not round-trip\n", name);
      ++failures;
    }

    char lower[32];
    size_t length = strlen(name);
    if (length >= sizeof(lower)) {
      fprintf(stderr, "key_names: name too long: %s\n", name);
      ++failures;
      continue;
    }
    for (size_t c = 0; c < length; ++c) {
      lower[c] = (char)tolower((unsigned char)name[c]);
    }
    lower[length] = '\0';
    if (bd_key_from_name(lower) != key) {
      fprintf(stderr, "key_names: lower-case '%s' did not parse\n", lower);
      ++failures;
    }
  }

  static const char *const required[] = {
      "A",     "B",      "X",     "Y",     "L1",         "R1",
      "L2",    "R2",     "SELECT", "START", "MENU",      "HOME",
      "UP",    "DOWN",   "LEFT",  "RIGHT", "POWER",      "VOLUME_UP",
      "VOLUME_DOWN"};
  for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
    if (bd_key_from_name(required[i]) == BD_KEY_UNKNOWN) {
      fprintf(stderr, "key_names: required key '%s' not accepted\n", required[i]);
      ++failures;
    }
  }

  if (bd_key_from_name("NOPE") != BD_KEY_UNKNOWN) {
    fprintf(stderr, "key_names: unknown key was accepted\n");
    ++failures;
  }
  if (bd_key_from_name(NULL) != BD_KEY_UNKNOWN) {
    fprintf(stderr, "key_names: NULL was accepted\n");
    ++failures;
  }
  if (strcmp(bd_key_name(BD_KEY_UNKNOWN), "UNKNOWN") != 0) {
    fprintf(stderr, "key_names: BD_KEY_UNKNOWN name mismatch\n");
    ++failures;
  }
  if (strcmp(bd_key_name((bd_key)999), "UNKNOWN") != 0) {
    fprintf(stderr, "key_names: invalid key name mismatch\n");
    ++failures;
  }

  if (failures != 0) {
    fprintf(stderr, "key_names: %d failure(s)\n", failures);
    return 1;
  }
  printf("key_names: %d logical keys round-trip\n", (int)BD_KEY_COUNT - 1);
  return 0;
}
