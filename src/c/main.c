#include <pebble.h>
#include "special-draw.h"
#include "special-draw-rotation.h"

static Window *s_main_window;
static BitmapLayer *s_background_layer;
static GBitmap *s_background_bitmap;
static Layer *s_time_layer;
static Layer *s_overlay_layer; // battery dots + burst - separate from
                                 // s_time_layer so redrawing them never
                                 // triggers the expensive special-draw
                                 // rotation session
static GFont s_time_font;
static char s_time_buffer[8];

static int s_battery_dots = 0; // 1-4, how many gauntlet lights are lit
static GColor s_battery_color; // color for the lit dots, changes with charge tier

// Explosion burst animation state - plays on launch and every hour.
// Simple, cheap: filled star shapes with a black outline, no special-draw
// involved.
#define BURST_FRAMES 10
#define BURST_FRAME_MS 40
#define BURST_MAX_RADIUS 30
#define STAR_POINTS 5
static AppTimer *s_burst_timer;
static bool s_burst_active = false;
static int s_burst_frame = 0;
static GPoint s_star_pts[STAR_POINTS * 2];
static GPath s_star_path = { .num_points = STAR_POINTS * 2, .points = s_star_pts };

// Draws text with a black outline of the given pixel thickness, by drawing
// offset black copies underneath the real colored text in every direction,
// then the real text on top.
static void draw_outlined_text(GContext *ctx, const char *text, GFont font,
                                GRect box, GColor fill_color, int thickness) {
  graphics_context_set_text_color(ctx, GColorBlack);
  for (int dx = -thickness; dx <= thickness; dx++) {
    for (int dy = -thickness; dy <= thickness; dy++) {
      if (dx == 0 && dy == 0) continue; // skip center - that's the fill pass below
      graphics_draw_text(ctx, text, font,
          GRect(box.origin.x + dx, box.origin.y + dy, box.size.w, box.size.h),
          GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    }
  }

  graphics_context_set_text_color(ctx, fill_color);
  graphics_draw_text(ctx, text, font, box,
      GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

// Battery gauntlet lights: draws all 4 sockets as solid black (visible,
// "off" state) with lit ones filled with the current tier color on top.
// Drawn as plain drawing, NOT inside the special-draw session - these
// shouldn't rotate with the time text, they're fixed decoration on the
// gauntlet art.
static void draw_battery_dots(GContext *ctx) {
  GPoint dot_positions[4] = {
    {160, 180},
    {171, 185},
    {155, 195},
    {166, 202}
  };
  int radius = 5;

  for (int i = 0; i < 4; i++) {
    // socket base - always drawn as solid black so unlit dots stay clearly
    // visible instead of blending into the gauntlet art
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_circle(ctx, dot_positions[i], radius);
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_draw_circle(ctx, dot_positions[i], radius);

    if (i < s_battery_dots) {
      // lit - color reflects the current charge tier, slightly inset for
      // a glowing-LED look
      graphics_context_set_fill_color(ctx, s_battery_color);
      graphics_fill_circle(ctx, dot_positions[i], radius - 1);
    }
  }
}

// Computes the STAR_POINTS*2 vertices of a star (alternating outer/inner
// radius) centered on the given point, into s_star_pts.
static void compute_star(GPoint center, int outer_r, int inner_r) {
  for (int i = 0; i < STAR_POINTS * 2; i++) {
    int32_t angle = (TRIG_MAX_ANGLE * i) / (STAR_POINTS * 2);
    int r = (i % 2 == 0) ? outer_r : inner_r;
    s_star_pts[i].x = center.x + (sin_lookup(angle) * r) / TRIG_MAX_RATIO;
    s_star_pts[i].y = center.y - (cos_lookup(angle) * r) / TRIG_MAX_RATIO;
  }
}

// Explosion burst: two expanding, color-shifting star bursts centered on
// Bakugo's gauntlets. Positions are a first estimate - nudge once you see
// it rendered against the real art, same as every other visual feature in
// this project. Color shifts yellow -> orange -> red across the burst to
// suggest a fading hot flash, since Pebble doesn't do true alpha fading
// easily on this kind of shape.
static void draw_burst(GContext *ctx) {
  if (!s_burst_active) return;

  GPoint fist_positions[2] = {
    {50, 190},   // left gauntlet - estimate, needs tuning
    {150, 195}   // right gauntlet - estimate, needs tuning
  };

  int outer_r = ((s_burst_frame + 1) * BURST_MAX_RADIUS) / BURST_FRAMES;
  int inner_r = (outer_r * 4) / 10;

  GColor color;
  if (s_burst_frame < BURST_FRAMES / 3) {
    color = GColorYellow;
  } else if (s_burst_frame < (2 * BURST_FRAMES) / 3) {
    color = GColorOrange;
  } else {
    color = GColorRed;
  }

  graphics_context_set_fill_color(ctx, color);
  graphics_context_set_stroke_color(ctx, GColorBlack);
  for (int i = 0; i < 2; i++) {
    compute_star(fist_positions[i], outer_r, inner_r);
    gpath_draw_filled(ctx, &s_star_path);
    gpath_draw_outline(ctx, &s_star_path);
  }
}

static void burst_step(void *data) {
  s_burst_timer = NULL; // this timer just fired - its handle is no longer valid
  s_burst_frame++;
  layer_mark_dirty(s_overlay_layer);
  if (s_burst_frame < BURST_FRAMES) {
    s_burst_timer = app_timer_register(BURST_FRAME_MS, burst_step, NULL);
  } else {
    s_burst_active = false;
  }
}

static void start_burst(void) {
  s_burst_active = true;
  s_burst_frame = 0;
  layer_mark_dirty(s_overlay_layer);
  s_burst_timer = app_timer_register(BURST_FRAME_MS, burst_step, NULL);
}

// Custom draw callback for the time layer - TextLayer can't rotate on its
// own, so we draw the text manually and wrap it in a special-draw rotation
// session instead.
//
// IMPORTANT: special-draw expects to draw across the *whole screen*, not a
// small sub-layer - it rotates everything around a fixed full-screen center
// point, and Pebble clips each layer's drawing to its own small frame. So
// this layer covers the entire window, and we position the text at its
// real on-screen coordinates (the bubble's location) explicitly, rather
// than relying on the layer's own (small) bounds.
static void time_layer_update_proc(Layer *layer, GContext *ctx) {
  // Position of the text within the full-screen canvas - box enlarged for
  // the bigger 72px font, may need further tuning once rendered.
  GRect text_bounds = GRect(32, 20, 160, 90);

  // Begin the special-draw session - everything drawn between begin/end
  // gets rotated together as one unit when the session ends.
  GSpecialSession *session = graphics_context_begin_special_draw(ctx);

  // thickness of 2 = a noticeably bolder outline than the original 1px
  draw_outlined_text(ctx, s_time_buffer, s_time_font, text_bounds, GColorRed, 2);

  // Negative angle flips rotation direction (Pebble rotates clockwise for
  // positive values) - flipped per feedback that it was tilting the wrong way.
  // NOTE: this rotates around the fixed screen center (100, 114), not
  // around the text's own position - since our text sits well above that
  // (around y=45-95), it may swing/shift visibly rather than tilting
  // cleanly in place. May need text_bounds repositioned once you see the
  // actual result, to compensate for that arc.
  graphics_context_special_session_add_modifier(session,
      graphics_special_draw_create_rotation_modifier(-(TRIG_MAX_ANGLE / 12)));

  graphics_context_end_special_draw(session); // actually draws the rotated result and cleans up
}

// Overlay layer: battery dots and burst effect, entirely separate from the
// time layer above - redrawing this never touches special-draw/rotation.
static void overlay_update_proc(Layer *layer, GContext *ctx) {
  draw_battery_dots(ctx);
  draw_burst(ctx);
}

static void update_time() {
  // Get a tm structure
  time_t temp = time(NULL);
  struct tm *tick_time = localtime(&temp);

  // Follow the watch's system 24h/12h clock setting
  strftime(s_time_buffer, sizeof(s_time_buffer), clock_is_24h_style() ?
                                                    "%H:%M" : "%I:%M", tick_time);

  // Trigger a redraw of the custom time layer
  layer_mark_dirty(s_time_layer);
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  update_time();
  if (units_changed & HOUR_UNIT) {
    start_burst();
  }
}

// Boundaries: <25% = 1 dot (red), <50% = 2 (yellow), <75% = 3 (green),
// else = 4 (teal)
static void update_battery_dots(BatteryChargeState state) {
  if (state.charge_percent < 25) {
    s_battery_dots = 1;
    s_battery_color = GColorRed;
  } else if (state.charge_percent < 50) {
    s_battery_dots = 2;
    s_battery_color = GColorYellow;
  } else if (state.charge_percent < 75) {
    s_battery_dots = 3;
    s_battery_color = GColorGreen;
  } else {
    s_battery_dots = 4;
    s_battery_color = GColorTiffanyBlue; // closest built-in to teal/blue-green
  }
  layer_mark_dirty(s_overlay_layer);
}

static void main_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  // Background image - Bakugo art with the empty speech bubble.
  // Image is 200x228, matching Emery's screen exactly, so it fills the
  // screen with no scaling/cropping needed.
  s_background_bitmap = gbitmap_create_with_resource(RESOURCE_ID_BOOM);
  s_background_layer = bitmap_layer_create(bounds);
  bitmap_layer_set_bitmap(s_background_layer, s_background_bitmap);
  layer_add_child(window_layer, bitmap_layer_get_layer(s_background_layer));

  // Time layer covers the whole screen (required by special-draw - see
  // comment on time_layer_update_proc). The actual text position is
  // handled inside the draw callback, not by this layer's frame.
  s_time_layer = layer_create(bounds);
  layer_set_update_proc(s_time_layer, time_layer_update_proc);

  s_time_font = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_BADA_72));

  // Time layer goes on top of the background image
  layer_add_child(window_layer, s_time_layer);

  // Overlay layer (battery dots, burst) goes on top of everything else
  s_overlay_layer = layer_create(bounds);
  layer_set_update_proc(s_overlay_layer, overlay_update_proc);
  layer_add_child(window_layer, s_overlay_layer);

  // Battery gauntlet lights
  update_battery_dots(battery_state_service_peek());
  battery_state_service_subscribe(update_battery_dots);
}

static void main_window_unload(Window *window) {
  // Destroy time layer and font
  layer_destroy(s_time_layer);
  fonts_unload_custom_font(s_time_font);

  layer_destroy(s_overlay_layer);

  // Destroy background bitmap and layer
  gbitmap_destroy(s_background_bitmap);
  bitmap_layer_destroy(s_background_layer);

  battery_state_service_unsubscribe();

  if (s_burst_timer) {
    app_timer_cancel(s_burst_timer);
  }
}

static void init() {
  s_main_window = window_create();

  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload
  });

  window_stack_push(s_main_window, true);

  // Make sure the time is displayed from the start
  update_time();

  // Register with TickTimerService for both per-minute clock updates and
  // hourly burst triggers
  tick_timer_service_subscribe(MINUTE_UNIT | HOUR_UNIT, tick_handler);

  // Launch burst
  start_burst();
}

static void deinit() {
  window_destroy(s_main_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}