#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "brickdesk/keyboard.h"

static int failures;

#define CHECK(condition, ...)                     \
  do {                                            \
    if (!(condition)) {                           \
      fprintf(stderr, "keyboard_test: FAIL: ");   \
      fprintf(stderr, __VA_ARGS__);               \
      fputc('\n', stderr);                        \
      ++failures;                                 \
    }                                             \
  } while (0)

typedef struct result {
  bool called;
  bool accepted;
  char text[BD_KEYBOARD_TEXT_MAX + 1];
} result;

static void on_done(void *user, bool accepted, const char *text) {
  result *out = (result *)user;
  out->called = true;
  out->accepted = accepted;
  snprintf(out->text, sizeof(out->text), "%s", text != NULL ? text : "");
}

static void press(bd_keyboard *keyboard, bd_key key) {
  CHECK(bd_keyboard_input(keyboard, key, true), "key %d not consumed", key);
  bd_keyboard_input(keyboard, key, false);
}

int main(void) {
  bd_keyboard *keyboard = bd_keyboard_create();
  CHECK(keyboard != NULL, "create");
  if (keyboard == NULL) {
    return 1;
  }
  result out;
  memset(&out, 0, sizeof(out));
  CHECK(bd_keyboard_open(keyboard, "密码", "abc", 16, true, on_done, &out),
        "open");
  CHECK(strcmp(bd_keyboard_text(keyboard), "abc") == 0, "initial text");

  press(keyboard, BD_KEY_A); /* row 0, col 0 = 1 */
  CHECK(strcmp(bd_keyboard_text(keyboard), "abc1") == 0, "type 1: %s",
        bd_keyboard_text(keyboard));
  press(keyboard, BD_KEY_B);
  CHECK(strcmp(bd_keyboard_text(keyboard), "abc") == 0, "backspace");

  press(keyboard, BD_KEY_DOWN);
  press(keyboard, BD_KEY_A); /* row 1, col 0 = q */
  CHECK(strcmp(bd_keyboard_text(keyboard), "abcq") == 0, "type q: %s",
        bd_keyboard_text(keyboard));

  press(keyboard, BD_KEY_X); /* symbols, row 1 col 0 = - */
  press(keyboard, BD_KEY_A);
  CHECK(strcmp(bd_keyboard_text(keyboard), "abcq-") == 0, "symbol: %s",
        bd_keyboard_text(keyboard));

  press(keyboard, BD_KEY_Y);
  CHECK(out.called && out.accepted, "done accepted");
  CHECK(strcmp(out.text, "abcq-") == 0, "done text %s", out.text);
  CHECK(!bd_keyboard_is_open(keyboard), "keyboard closed after done");

  memset(&out, 0, sizeof(out));
  CHECK(bd_keyboard_open(keyboard, "取消", "", 16, false, on_done, &out),
        "reopen");
  for (int i = 0; i < 4; ++i) {
    press(keyboard, BD_KEY_DOWN);
  }
  press(keyboard, BD_KEY_A); /* 取消 */
  CHECK(out.called && !out.accepted, "cancel reports accepted=false");
  CHECK(out.text[0] == '\0', "cancel carries no text");
  CHECK(!bd_keyboard_is_open(keyboard), "keyboard closed after cancel");

  memset(&out, 0, sizeof(out));
  CHECK(bd_keyboard_open(keyboard, "上限", "12345", 5, false, on_done, &out),
        "open with limit");
  press(keyboard, BD_KEY_A); /* limit reached, ignored */
  CHECK(strcmp(bd_keyboard_text(keyboard), "12345") == 0,
        "max length enforced: %s", bd_keyboard_text(keyboard));
  press(keyboard, BD_KEY_Y);
  CHECK(out.called && strcmp(out.text, "12345") == 0, "limit text %s",
        out.text);

  bd_keyboard_destroy(keyboard);
  if (failures != 0) {
    fprintf(stderr, "keyboard_test: %d failure(s)\n", failures);
    return 1;
  }
  printf("keyboard_test: ok\n");
  return 0;
}
