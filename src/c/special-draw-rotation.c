#include "special-draw.h"

// PATCHED: removed the original "#include special-draw-opacity.h" line -
// nothing in this file actually uses it, and skipping it means we don't
// need to vendor the opacity feature at all since it's not needed here.

// PATCHED: the original library only defines centers for 144x168 (classic
// Pebble) and 180x180 (old round Chalk) - neither matches Emery's real
// 200x228 (center: 100, 114). Defining this first means the library's own
// fallback (wrong) value is skipped.
#ifndef SPECIAL_DRAW_SCREEN_CENTER
#define SPECIAL_DRAW_SCREEN_CENTER (GPoint(100, 114))
#endif

#if defined(PBL_RECT) && !defined(SPECIAL_DRAW_SCREEN_CENTER)
#define SPECIAL_DRAW_SCREEN_CENTER (GPoint(72, 84))
#elif defined(PBL_ROUND) && !defined(SPECIAL_DRAW_SCREEN_CENTER)
#define SPECIAL_DRAW_SCREEN_CENTER (GPoint(90, 90))
#endif

static void prv_draw_modifier(GContext *ctx, GSpecialSessionModifier * modifier,
        GBitmap * session_bitmap) {
    graphics_draw_rotated_bitmap(ctx, session_bitmap,
        SPECIAL_DRAW_SCREEN_CENTER, *(int32_t *)modifier->context,
        SPECIAL_DRAW_SCREEN_CENTER);
}

static void prv_destroy_modifier(GSpecialSessionModifier * modifier) {
    free(modifier->context);
    free(modifier);
}

GSpecialSessionModifier * graphics_special_draw_create_rotation_modifier(
        int32_t angle) {
    GSpecialSessionModifier * mod = malloc(sizeof(GSpecialSessionModifier));
    mod->overrides_draw = true;
    mod->action.modifier_draw = prv_draw_modifier;
    mod->destroy = prv_destroy_modifier;
    mod->context = malloc(sizeof(int32_t));
    *((int32_t *) mod->context) = angle;
    return mod;
}