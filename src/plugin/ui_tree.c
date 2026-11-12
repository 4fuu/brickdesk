#define _POSIX_C_SOURCE 200809L

#include "brickdesk/plugin.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "json.h"

typedef struct ui_parser {
  const bd_plugin_json *json;
  bd_plugin_ui *ui;
  char *error;
  size_t error_size;
} ui_parser;

static bool ui_error(ui_parser *parser, const char *format, ...) {
  if (parser->error != NULL && parser->error_size > 0) {
    va_list args;
    va_start(args, format);
    vsnprintf(parser->error, parser->error_size, format, args);
    va_end(args);
  }
  return false;
}

static const char *ui_type_name(const bd_plugin_ui_node *node) {
  switch (node->type) {
    case BD_PLUGIN_UI_COLUMN:
      return "column";
    case BD_PLUGIN_UI_ROW:
      return "row";
    case BD_PLUGIN_UI_TEXT:
      return "text";
    case BD_PLUGIN_UI_ICON:
      return "icon";
    case BD_PLUGIN_UI_PROGRESS:
      return "progress";
    case BD_PLUGIN_UI_TOGGLE:
      return "toggle";
    case BD_PLUGIN_UI_SLIDER:
      return "slider";
    case BD_PLUGIN_UI_SELECT:
      return "select";
    case BD_PLUGIN_UI_BUTTON:
      return "button";
    case BD_PLUGIN_UI_SPACER:
      return "spacer";
    default:
      return "unknown";
  }
}

static bool ui_node_type(const char *name, bd_plugin_ui_node_type *out) {
  if (strcmp(name, "column") == 0) {
    *out = BD_PLUGIN_UI_COLUMN;
  } else if (strcmp(name, "row") == 0) {
    *out = BD_PLUGIN_UI_ROW;
  } else if (strcmp(name, "text") == 0) {
    *out = BD_PLUGIN_UI_TEXT;
  } else if (strcmp(name, "icon") == 0) {
    *out = BD_PLUGIN_UI_ICON;
  } else if (strcmp(name, "progress") == 0) {
    *out = BD_PLUGIN_UI_PROGRESS;
  } else if (strcmp(name, "toggle") == 0) {
    *out = BD_PLUGIN_UI_TOGGLE;
  } else if (strcmp(name, "slider") == 0) {
    *out = BD_PLUGIN_UI_SLIDER;
  } else if (strcmp(name, "select") == 0) {
    *out = BD_PLUGIN_UI_SELECT;
  } else if (strcmp(name, "button") == 0) {
    *out = BD_PLUGIN_UI_BUTTON;
  } else if (strcmp(name, "spacer") == 0) {
    *out = BD_PLUGIN_UI_SPACER;
  } else {
    return false;
  }
  return true;
}

static bool ui_member_string(ui_parser *parser, const jsmntok_t *object,
                             const char *field, char *out, size_t out_size,
                             bool required) {
  const jsmntok_t *token = bd_plugin_json_member(parser->json, object, field);
  if (token == NULL) {
    if (required) {
      return ui_error(parser, "node is missing '%s'", field);
    }
    return true;
  }
  if (!bd_plugin_json_string(parser->json, token, out, out_size)) {
    return ui_error(parser, "node field '%s' is not a string or is too long",
                    field);
  }
  return true;
}

static bool ui_member_float(ui_parser *parser, const jsmntok_t *object,
                            const char *field, float *out, bool required) {
  const jsmntok_t *token = bd_plugin_json_member(parser->json, object, field);
  if (token == NULL) {
    if (required) {
      return ui_error(parser, "node is missing '%s'", field);
    }
    return true;
  }
  if (!bd_plugin_json_float(parser->json, token, out)) {
    return ui_error(parser, "node field '%s' must be a finite number", field);
  }
  return true;
}

static bool ui_member_int(ui_parser *parser, const jsmntok_t *object,
                          const char *field, int *out) {
  const jsmntok_t *token = bd_plugin_json_member(parser->json, object, field);
  if (token == NULL) {
    return true;
  }
  if (!bd_plugin_json_int(parser->json, token, out)) {
    return ui_error(parser, "node field '%s' must be an integer", field);
  }
  return true;
}

static bool ui_member_bool(ui_parser *parser, const jsmntok_t *object,
                           const char *field, bool *out, bool required) {
  const jsmntok_t *token = bd_plugin_json_member(parser->json, object, field);
  if (token == NULL) {
    if (required) {
      return ui_error(parser, "node is missing '%s'", field);
    }
    return true;
  }
  if (!bd_plugin_json_bool(parser->json, token, out)) {
    return ui_error(parser, "node field '%s' must be a boolean", field);
  }
  return true;
}

static bool ui_valid_style(const char *style) {
  return strcmp(style, "title") == 0 || strcmp(style, "body") == 0 ||
         strcmp(style, "caption") == 0 || strcmp(style, "mono") == 0;
}

static bool ui_parse_options(ui_parser *parser, const jsmntok_t *object,
                             bd_plugin_ui_node *node) {
  const jsmntok_t *array =
      bd_plugin_json_member(parser->json, object, "options");
  if (!bd_plugin_json_is_array(array) || array->size < 1 ||
      array->size > 12) {
    return ui_error(parser, "select node needs 1 to 12 options");
  }
  node->first_option = parser->ui->option_count;
  node->option_count = array->size;
  if (parser->ui->option_count + array->size > BD_PLUGIN_UI_MAX_OPTIONS) {
    return ui_error(parser, "too many select options");
  }
  int index = (int)(array - parser->json->tokens) + 1;
  for (int i = 0; i < array->size; ++i) {
    const jsmntok_t *token = &parser->json->tokens[index];
    char *option = parser->ui->options[parser->ui->option_count];
    if (!bd_plugin_json_string(parser->json, token, option, 32)) {
      return ui_error(parser, "select options must be strings shorter than 32 bytes");
    }
    ++parser->ui->option_count;
    const jsmntok_t *next = bd_plugin_json_skip(parser->json, token);
    if (next == NULL) {
      return ui_error(parser, "truncated select options");
    }
    index = (int)(next - parser->json->tokens);
  }
  return true;
}

static bool ui_parse_node(ui_parser *parser, int token_index, int depth,
                          int node_index) {
  if (depth > BD_PLUGIN_UI_MAX_DEPTH) {
    return ui_error(parser, "UI tree is deeper than %d levels",
                    BD_PLUGIN_UI_MAX_DEPTH);
  }
  if (node_index >= BD_PLUGIN_UI_MAX_NODES) {
    return ui_error(parser, "UI tree has more than %d nodes",
                    BD_PLUGIN_UI_MAX_NODES);
  }
  if (token_index < 0 || token_index >= parser->json->count) {
    return ui_error(parser, "truncated UI tree");
  }
  const jsmntok_t *object = &parser->json->tokens[token_index];
  if (!bd_plugin_json_is_object(object)) {
    return ui_error(parser, "UI node must be an object");
  }
  bd_plugin_ui_node *node = &parser->ui->nodes[node_index];
  memset(node, 0, sizeof(*node));
  node->first_option = 0;

  char type[24];
  if (!ui_member_string(parser, object, "type", type, sizeof(type), true)) {
    return false;
  }
  if (!ui_node_type(type, &node->type)) {
    return ui_error(parser, "unknown UI node type '%s'", type);
  }
  ++parser->ui->node_count;

  if (!ui_member_string(parser, object, "id", node->id, sizeof(node->id),
                        false) ||
      !ui_member_int(parser, object, "padding", &node->padding) ||
      !ui_member_int(parser, object, "gap", &node->gap)) {
    return false;
  }
  if (node->padding < 0 || node->padding > 64 || node->gap < 0 ||
      node->gap > 64) {
    return ui_error(parser, "padding and gap must be 0..64");
  }

  switch (node->type) {
    case BD_PLUGIN_UI_COLUMN:
    case BD_PLUGIN_UI_ROW: {
      const jsmntok_t *children =
          bd_plugin_json_member(parser->json, object, "children");
      if (children != NULL && !bd_plugin_json_is_array(children)) {
        return ui_error(parser, "children must be an array");
      }
      if (children != NULL && children->size > BD_PLUGIN_UI_MAX_CHILDREN) {
        return ui_error(parser, "a container has more than %d children",
                        BD_PLUGIN_UI_MAX_CHILDREN);
      }
      if (children != NULL && children->size > 0) {
        int child_index = (int)(children - parser->json->tokens) + 1;
        for (int i = 0; i < children->size; ++i) {
          int child_node = parser->ui->node_count;
          if (child_node >= BD_PLUGIN_UI_MAX_NODES) {
            return ui_error(parser, "UI tree has more than %d nodes",
                            BD_PLUGIN_UI_MAX_NODES);
          }
          node->children[node->child_count] = child_node;
          ++node->child_count;
          if (!ui_parse_node(parser, child_index, depth + 1, child_node)) {
            return false;
          }
          const jsmntok_t *next =
              bd_plugin_json_skip(parser->json, &parser->json->tokens[child_index]);
          if (next == NULL) {
            return ui_error(parser, "truncated children array");
          }
          child_index = (int)(next - parser->json->tokens);
        }
      }
      break;
    }
    case BD_PLUGIN_UI_TEXT:
      if (!ui_member_string(parser, object, "text", node->text,
                            sizeof(node->text), true)) {
        return false;
      }
      if (node->text[0] == '\0') {
        return ui_error(parser, "text node needs a non-empty text");
      }
      snprintf(node->style, sizeof(node->style), "%s", "body");
      if (!ui_member_string(parser, object, "style", node->style,
                            sizeof(node->style), false)) {
        return false;
      }
      if (!ui_valid_style(node->style)) {
        return ui_error(parser, "unknown text style '%s'", node->style);
      }
      break;
    case BD_PLUGIN_UI_ICON:
      if (!ui_member_string(parser, object, "name", node->icon,
                            sizeof(node->icon), true)) {
        return false;
      }
      break;
    case BD_PLUGIN_UI_PROGRESS:
      if (!ui_member_float(parser, object, "value", &node->value, true) ||
          !ui_member_float(parser, object, "max", &node->max_value, true)) {
        return false;
      }
      node->min_value = 0.0f;
      if (node->max_value <= 0.0f || node->max_value > 1000000.0f ||
          node->value < 0.0f || node->value > node->max_value) {
        return ui_error(parser, "progress needs 0 <= value <= max");
      }
      break;
    case BD_PLUGIN_UI_TOGGLE:
      if (node->id[0] == '\0') {
        return ui_error(parser, "toggle node needs an id");
      }
      if (!ui_member_string(parser, object, "label", node->text,
                            sizeof(node->text), true) ||
          !ui_member_bool(parser, object, "value", &node->boolean, true)) {
        return false;
      }
      break;
    case BD_PLUGIN_UI_SLIDER:
      if (node->id[0] == '\0') {
        return ui_error(parser, "slider node needs an id");
      }
      node->step = 1.0f;
      if (!ui_member_string(parser, object, "label", node->text,
                            sizeof(node->text), true) ||
          !ui_member_float(parser, object, "value", &node->value, true) ||
          !ui_member_float(parser, object, "min", &node->min_value, true) ||
          !ui_member_float(parser, object, "max", &node->max_value, true) ||
          !ui_member_float(parser, object, "step", &node->step, false)) {
        return false;
      }
      if (node->max_value <= node->min_value || node->step <= 0.0f ||
          node->value < node->min_value || node->value > node->max_value) {
        return ui_error(parser, "slider needs min < value <= max and step > 0");
      }
      break;
    case BD_PLUGIN_UI_SELECT:
      if (node->id[0] == '\0') {
        return ui_error(parser, "select node needs an id");
      }
      if (!ui_member_string(parser, object, "label", node->text,
                            sizeof(node->text), true) ||
          !ui_member_string(parser, object, "value", node->value_text,
                            sizeof(node->value_text), true) ||
          !ui_parse_options(parser, object, node)) {
        return false;
      }
      {
        bool match = false;
        for (int i = 0; i < node->option_count; ++i) {
          if (strcmp(parser->ui->options[node->first_option + i],
                     node->value_text) == 0) {
            match = true;
            break;
          }
        }
        if (!match) {
          return ui_error(parser, "select value '%s' is not in options",
                          node->value_text);
        }
      }
      break;
    case BD_PLUGIN_UI_BUTTON:
      if (node->id[0] == '\0') {
        return ui_error(parser, "button node needs an id");
      }
      if (!ui_member_string(parser, object, "label", node->text,
                            sizeof(node->text), true) ||
          !ui_member_string(parser, object, "action", node->action,
                            sizeof(node->action), true)) {
        return false;
      }
      break;
    case BD_PLUGIN_UI_SPACER:
      break;
    default:
      return ui_error(parser, "unknown UI node type '%s'", ui_type_name(node));
  }
  return true;
}

bool bd_plugin_ui_parse(const char *json_text, size_t length,
                        bd_plugin_ui *out, char *error, size_t error_size) {
  if (out == NULL) {
    return false;
  }
  memset(out, 0, sizeof(*out));
  out->root = -1;
  if (json_text == NULL || length == 0) {
    if (error != NULL && error_size > 0) {
      snprintf(error, error_size, "empty UI tree");
    }
    return false;
  }
  if (length > BD_PLUGIN_UI_JSON_MAX) {
    if (error != NULL && error_size > 0) {
      snprintf(error, error_size, "UI tree is larger than %d bytes",
               BD_PLUGIN_UI_JSON_MAX);
    }
    return false;
  }
  bd_plugin_json json;
  if (!bd_plugin_json_parse(json_text, length, &json, BD_PLUGIN_JSON_MAX_TOKENS,
                            error, error_size)) {
    return false;
  }
  ui_parser parser;
  parser.json = &json;
  parser.ui = out;
  parser.error = error;
  parser.error_size = error_size;
  bool ok = ui_parse_node(&parser, 0, 1, 0);
  if (ok) {
    out->root = 0;
  } else {
    memset(out, 0, sizeof(*out));
    out->root = -1;
  }
  bd_plugin_json_destroy(&json);
  return ok;
}
