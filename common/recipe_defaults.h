#ifndef RECIPE_DEFAULTS_H
#define RECIPE_DEFAULTS_H

#include <stddef.h>

/* One per-recipe form field for the dashboard: `value` pre-fills the input when
 * the user selects `recipe`; `shown` (1/0) is whether that field is visible for
 * that recipe (0 = managed but hidden -- value still applied on Apply). Every
 * recipe lists every recipe-managed key. `recipe` matches an opt_recipe enum
 * value; `key` is a PIVID_* config key; `value` is its string form. */
typedef struct {
    const char *recipe;
    const char *key;
    const char *value;
    int         shown;
} RecipeDefault;

extern const RecipeDefault recipe_defaults[];
extern const size_t        recipe_defaults_len;

/* Serialise the table to a JSON object keyed by recipe, each field carrying its
 * value and visibility flag:
 *   {"droppable":{"BITRATE":{"v":"650000","show":true},...},...}
 * Bounds-checked: writes only within `cap`; returns the byte count written
 * (excluding the NUL) or -1 on truncation. */
int recipe_defaults_json_build(char *buf, size_t cap);

#endif
