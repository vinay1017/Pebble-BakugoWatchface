#include <pebble.h>
#include "special-draw.h"
#include "special-draw-rotation.h"

static Window *s_main_window;
static BitmapLayer *s_background_layer;
static GBitmap *s_background_bitmap;
static Layer *s_time_layer;
static Layer *s_overlay_layer; // battery dots - separate from s_time_layer
                                 // so redrawing it never triggers the
                                 // expensive special-draw rotation session
static GFont s_time_font;
static GFont s_day_font;
static char s_time_buffer[8];
static char s_day_buffer[10]; // longest weekday name + null terminator

static int s_battery_dots = 0; // 1-4, how many gauntlet lights are lit
static GColor s_battery_color; // color for the lit dots, changes with charge tier

// Settings, toggled via the Clay config page and persisted across restarts
static bool s_show_battery = true;
static bool s_show_weekday = true;
static bool s_rotated_text = true; // when false, skip special-draw entirely

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

// Custom draw callback for the time layer - TextLayer can't rotate on its
// own, so we draw the text manually and wrap it in a special-draw rotation
// session instead.
//
// IMPORTANT: special-draw expects to draw across the *whole screen*, not a
// small sub-layer - it rotates everything around a fixed full-screen center
// point, and Pebble clips each layer's drawing to its own small frame. So
// this layer covers the entire window, and we position text at real
// on-screen coordinates explicitly, rather than relying on the layer's own
// (small) bounds.
static void time_layer_update_proc(Layer *layer, GContext *ctx) {
  // Rotated mode: position tuned for the rotated composition.
  GRect text_bounds_rotated = GRect(32, 20, 160, 90);
  GRect day_bounds_rotated = GRect(10, 95, 200, 60);

  // Straight (non-rotated) mode: separate position, shifted down and left
  // per feedback - estimate, needs tuning once you see it rendered.
  GRect text_bounds_straight = GRect(15, 30, 160, 90);
  GRect day_bounds_straight = GRect(-50, 105, 200, 60);

  if (s_rotated_text) {
    GRect text_bounds = text_bounds_rotated;
    GRect day_bounds = day_bounds_rotated;

    // Begin the special-draw session - everything drawn between begin/end
    // gets rotated TOGETHER as one unit when the session ends. Drawing the
    // day name in here too (rather than a second session) means it shares
    // the same single rotation pass and costs almost nothing extra on top
    // of what the time text already costs.
    GSpecialSession *session = graphics_context_begin_special_draw(ctx);

    // thickness of 2 = a noticeably bolder outline than the original 1px
    draw_outlined_text(ctx, s_time_buffer, s_time_font, text_bounds, GColorRed, 2);

    if (s_show_weekday) {
      // thinner outline for the smaller day text, proportionally similar weight
      draw_outlined_text(ctx, s_day_buffer, s_day_font, day_bounds, GColorRed, 1);
    }

    // Negative angle flips rotation direction (Pebble rotates clockwise for
    // positive values) - flipped per feedback that it was tilting the wrong way.
    // NOTE: this rotates around the fixed screen center (100, 114), not
    // around the text's own position, so both text blocks swing together
    // around that point - may need bounds repositioned once you see the
    // actual result, to compensate for that arc.
    graphics_context_special_session_add_modifier(session,
        graphics_special_draw_create_rotation_modifier(-(TRIG_MAX_ANGLE / 12)));

    graphics_context_end_special_draw(session); // actually draws the rotated result and cleans up
  } else {
    // Rotated Text OFF - plain upright drawing, no special-draw session at
    // all. This completely skips the expensive rotation operation, not
    // just a lighter version of it. Uses its own separate position.
    draw_outlined_text(ctx, s_time_buffer, s_time_font, text_bounds_straight, GColorRed, 2);
    if (s_show_weekday) {
      draw_outlined_text(ctx, s_day_buffer, s_day_font, day_bounds_straight, GColorRed, 1);
    }
  }
}

// Overlay layer: battery dots, entirely separate from the time layer above
// - redrawing this never touches special-draw/rotation.
static void overlay_update_proc(Layer *layer, GContext *ctx) {
  if (s_show_battery) {
    draw_battery_dots(ctx);
  }
}

static void update_time() {
  // Get a tm structure
  time_t temp = time(NULL);
  struct tm *tick_time = localtime(&temp);

  // Follow the watch's system 24h/12h clock setting
  strftime(s_time_buffer, sizeof(s_time_buffer), clock_is_24h_style() ?
                                                    "%H:%M" : "%I:%M", tick_time);

  // Full weekday name, e.g. "Wednesday"
  strftime(s_day_buffer, sizeof(s_day_buffer), "%A", tick_time);

  // Trigger a redraw of the custom time layer
  layer_mark_dirty(s_time_layer);
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  update_time();
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

// Receives settings from the Clay config page and saves them to persistent
// storage so they survive an app restart. Buffer sizes are set explicitly
// small in app_message_open() below (256 bytes each) rather than the
// platform maximum - a real advantage of native C over the Alloy project's
// Message class, which couldn't do this.
static void inbox_received_handler(DictionaryIterator *iterator, void *context) {
  Tuple *battery_tuple = dict_find(iterator, MESSAGE_KEY_ShowBattery);
  if (battery_tuple) {
    s_show_battery = battery_tuple->value->int32 == 1;
    persist_write_bool(MESSAGE_KEY_ShowBattery, s_show_battery);
  }

  Tuple *weekday_tuple = dict_find(iterator, MESSAGE_KEY_ShowWeekday);
  if (weekday_tuple) {
    s_show_weekday = weekday_tuple->value->int32 == 1;
    persist_write_bool(MESSAGE_KEY_ShowWeekday, s_show_weekday);
  }

  Tuple *rotated_tuple = dict_find(iterator, MESSAGE_KEY_RotatedText);
  if (rotated_tuple) {
    s_rotated_text = rotated_tuple->value->int32 == 1;
    persist_write_bool(MESSAGE_KEY_RotatedText, s_rotated_text);
  }

  layer_mark_dirty(s_time_layer);
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
  s_day_font = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_BADA_30));

  // Time layer goes on top of the background image
  layer_add_child(window_layer, s_time_layer);

  // Overlay layer (battery dots) goes on top of everything else
  s_overlay_layer = layer_create(bounds);
  layer_set_update_proc(s_overlay_layer, overlay_update_proc);
  layer_add_child(window_layer, s_overlay_layer);

  // Battery gauntlet lights
  update_battery_dots(battery_state_service_peek());
  battery_state_service_subscribe(update_battery_dots);
}

static void main_window_unload(Window *window) {
  // Destroy time layer and fonts
  layer_destroy(s_time_layer);
  fonts_unload_custom_font(s_time_font);
  fonts_unload_custom_font(s_day_font);

  layer_destroy(s_overlay_layer);

  // Destroy background bitmap and layer
  gbitmap_destroy(s_background_bitmap);
  bitmap_layer_destroy(s_background_layer);

  battery_state_service_unsubscribe();
}

static void init() {
  // Load saved settings, if any exist from a previous run
  if (persist_exists(MESSAGE_KEY_ShowBattery)) {
    s_show_battery = persist_read_bool(MESSAGE_KEY_ShowBattery);
  }
  if (persist_exists(MESSAGE_KEY_ShowWeekday)) {
    s_show_weekday = persist_read_bool(MESSAGE_KEY_ShowWeekday);
  }
  if (persist_exists(MESSAGE_KEY_RotatedText)) {
    s_rotated_text = persist_read_bool(MESSAGE_KEY_RotatedText);
  }

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

  // Settings - small explicit buffer sizes, not the platform maximum
  app_message_register_inbox_received(inbox_received_handler);
  app_message_open(256, 256);
}

static void deinit() {
  window_destroy(s_main_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}