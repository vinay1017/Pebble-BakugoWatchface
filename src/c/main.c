#include <pebble.h>
#include "special-draw.h"
#include "special-draw-rotation.h"

static Window *s_main_window;
static BitmapLayer *s_background_layer;
static GBitmap *s_background_bitmap;
static Layer *s_time_layer;
static GFont s_time_font;
static char s_time_buffer[8];

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

  s_time_font = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_BADA_75));

  // Time layer goes on top of the background image
  layer_add_child(window_layer, s_time_layer);
}

static void main_window_unload(Window *window) {
  // Destroy time layer and font
  layer_destroy(s_time_layer);
  fonts_unload_custom_font(s_time_font);

  // Destroy background bitmap and layer
  gbitmap_destroy(s_background_bitmap);
  bitmap_layer_destroy(s_background_layer);
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

  // Register with TickTimerService
  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
}

static void deinit() {
  window_destroy(s_main_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}