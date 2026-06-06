#include <pebble.h>

#define COLS_PER_DIGIT 3
#define ROWS_PER_DIGIT 5
#define GRID_COLS 12
#define GRID_ROWS 10
#define NUM_DIGITS 8
#define ANIM_DURATION_MS 4000
#define SETTINGS_KEY 1

typedef struct {
  GColor fg_color;
  GColor bg_color;
} Settings;

// Each column has 2 animations:
// one clears the column to bg color (0s-2s)
// other draws a part of a digit (1s-4s)
typedef struct {
  uint16_t clear_fill_y;
  uint16_t clear_speed;
  uint16_t new_fill_y;
  uint16_t new_speed;
} Column;

// 3x5 digit patterns. Each literal reads top-to-bottom, left-to-right:
// Cell (dc, dr) is at bit position (14 - dr*3 - dc).
static const uint16_t DIGITS[10] = {
  0b111101101101111, // 0
  0b011001001001001, // 1
  0b111001111100111, // 2
  0b111001111001111, // 3
  0b101101111001001, // 4
  0b111100111001111, // 5
  0b111100111101111, // 6
  0b111101001001001, // 7
  0b111101111101111, // 8
  0b111101111001111, // 9
};

static inline bool digit_pixel(uint8_t value, uint8_t dc, uint8_t dr) {
  return (DIGITS[value] >> (14 - dr * 3 - dc)) & 1;
}

static Settings s_settings;

static Window *s_window;
static Layer *s_layer;

static uint16_t s_screen_width;
static uint16_t s_screen_height;
static uint16_t s_num_columns;

static struct tm s_current_time;
static uint8_t s_digits[NUM_DIGITS];

// Precomputed on/off table per grid cell, so the hot render loop avoids the
// bit-shift in digit_pixel for every screen pixel.
static bool s_old_grid[GRID_COLS][GRID_ROWS];
static bool s_new_grid[GRID_COLS][GRID_ROWS];

static AnimationImplementation s_animation_impl;
static Column *s_columns;
static bool s_first_animation;

// Lookup tables, populated in window_load. Sized for the largest supported
// platform, emery (200x228, 100 columns).
// s_gc_for_x is per-pixel because on emery the digit grid boundary falls
// between paired pixels for a few columns (8/33/58/83); the slow path in
// update_proc consults both halves.
static uint8_t s_gc_for_x[200];
static uint8_t s_gr_for_y[228];
static bool s_col_is_bound[100];

static void load_settings(void) {
  s_settings.fg_color = GColorWhite;
  s_settings.bg_color = GColorBlack;
  persist_read_data(SETTINGS_KEY, &s_settings, sizeof(s_settings));
}

static void save_settings(void) {
  persist_write_data(SETTINGS_KEY, &s_settings, sizeof(s_settings));
}

// Fills the digits array with new values
static void compute_digits(const struct tm *t, uint8_t *out) {
  out[0] = t->tm_hour / 10;
  out[1] = t->tm_hour % 10;
  out[2] = t->tm_min / 10;
  out[3] = t->tm_min % 10;
  out[4] = t->tm_mday / 10;
  out[5] = t->tm_mday % 10;
  out[6] = (t->tm_mon + 1) / 10;
  out[7] = (t->tm_mon + 1) % 10;
}

static void fill_grid(const uint8_t *digits, bool grid[GRID_COLS][GRID_ROWS]) {
  for (uint8_t gc = 0; gc < GRID_COLS; gc++) {
    for (uint8_t gr = 0; gr < GRID_ROWS; gr++) {
      // 4 digits per row
      uint8_t slot = (gr / ROWS_PER_DIGIT) * 4 + (gc / COLS_PER_DIGIT);
      uint8_t dc = gc % COLS_PER_DIGIT;
      uint8_t dr = gr % ROWS_PER_DIGIT;
      grid[gc][gr] = digit_pixel(digits[slot], dc, dr);
    }
  }
}

static void animation_stopped(Animation *anim, bool stopped, void *ctx) {
  // Ensure the final frame shows new digits fully, even if the last frame()
  // callback didn't reach progress = ANIMATION_NORMALIZED_MAX exactly.
  for (uint16_t c = 0; c < s_num_columns; c++) {
    s_columns[c].clear_fill_y = s_screen_height;
    s_columns[c].new_fill_y = s_screen_height;
  }
  if (s_first_animation) {
    s_first_animation = false;
  }
  if (s_layer) {
    layer_mark_dirty(s_layer);
  }
}

static void start_animation(void) {
  memcpy(s_old_grid, s_new_grid, sizeof(s_old_grid));

  compute_digits(&s_current_time, s_digits);
  fill_grid(s_digits, s_new_grid);

  for (uint16_t c = 0; c < s_num_columns; c++) {
    s_columns[c].clear_fill_y = 0;
    s_columns[c].new_fill_y = 0;
    s_columns[c].clear_speed = 256 + (rand() % 257); // [256, 512]
    s_columns[c].new_speed = 256 + (rand() % 257);   // [256, 512]
  }

  Animation *anim = animation_create();
  animation_set_duration(anim, ANIM_DURATION_MS);
  animation_set_curve(anim, AnimationCurveEaseInOut);
  animation_set_handlers(anim, (AnimationHandlers){
    .stopped = animation_stopped,
  }, NULL);
  animation_set_implementation(anim, &s_animation_impl);
  animation_schedule(anim);
}

static void frame(Animation *anim, AnimationProgress progress) {
  // The two waterfalls are phases of one 4 s animation:
  //   clear: ramps over [0, 2/4] of progress (0..2000 ms)
  //   new:   ramps over [1/4, 1] of progress (1000..4000 ms)
  // Both ramps are normalized to a 0..256 fixed-point "base", then scaled
  // by each column's individual speed multiplier.
  int32_t p = (int32_t)progress;
  int32_t norm = (int32_t)ANIMATION_NORMALIZED_MAX;

  int32_t clear_p = p * 4 / 2;
  if (clear_p > norm) clear_p = norm;
  int32_t new_p = (p * 4 - norm) / 3;
  if (s_first_animation) {
    new_p = p;
  }
  if (new_p < 0) new_p = 0;
  if (new_p > norm) new_p = norm;

  uint32_t clear_base = (uint32_t)clear_p * 256 / (uint32_t)norm;
  uint32_t new_base = (uint32_t)new_p * 256 / (uint32_t)norm;

  for (uint16_t c = 0; c < s_num_columns; c++) {
    uint32_t cf = clear_base * s_columns[c].clear_speed * s_screen_height / (256 * 256);
    if (cf > s_screen_height) cf = s_screen_height;
    uint32_t nf = new_base * s_columns[c].new_speed * s_screen_height / (256 * 256);
    if (nf > s_screen_height) nf = s_screen_height;
    // The new front must never overtake the clear front in any column, or
    // new digits would briefly show over old with no black gap between them.
    if (nf > cf) nf = cf;
    s_columns[c].clear_fill_y = (uint16_t)cf;
    s_columns[c].new_fill_y = (uint16_t)nf;
  }
  if (s_layer) {
    layer_mark_dirty(s_layer);
  }
}

static void update_proc(Layer *layer, GContext *ctx) {
  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (!fb) return;

  uint8_t fg = s_settings.fg_color.argb;
  uint8_t bg = s_settings.bg_color.argb;
  uint16_t y_mid = s_screen_height >> 1;
  uint16_t num_cols = s_num_columns;

  for (uint16_t y = 0; y < s_screen_height; y++) {
    GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, y);
    uint8_t *p = row.data;

    // The middle row always has bg color
    if (y == y_mid) {
      memset(p + row.min_x, bg, row.max_x - row.min_x + 1);
      continue;
    }

    uint8_t gr = s_gr_for_y[y];

    for (uint16_t col = 0; col < num_cols; col++) {
      uint16_t x_base = col << 1;
      uint16_t nfy = s_columns[col].new_fill_y;
      uint16_t cfy = s_columns[col].clear_fill_y;
      uint8_t gc_l = s_gc_for_x[x_base];
      uint8_t gc_r = s_gc_for_x[x_base + 1];

      if (gc_l == gc_r) {
        // Fast path: both pixels share gc
        bool on;
        if (y < nfy) {
          on = s_new_grid[gc_l][gr];
        } else if (y < cfy) {
          on = false;
        } else {
          on = s_old_grid[gc_l][gr];
        }
        uint8_t color = on ? fg : bg;
        if (s_col_is_bound[col]) {
          p[x_base] = bg;          // grid line: left pixel is bg
          p[x_base + 1] = color;
        } else {
          // Paired-pixel store: emery rows are byte-aligned and rectangular.
          uint16_t pair = (uint16_t)color | ((uint16_t)color << 8);
          *(uint16_t *)(p + x_base) = pair;
        }
      } else {
        // Slow path: grid boundary falls between the two pixels of this
        // column (cols 8/33/58/83 on emery;
        bool on_l, on_r;
        if (y < nfy) {
          on_l = s_new_grid[gc_l][gr];
          on_r = s_new_grid[gc_r][gr];
        } else if (y < cfy) {
          on_l = false;
          on_r = false;
        } else {
          on_l = s_old_grid[gc_l][gr];
          on_r = s_old_grid[gc_r][gr];
        }
        // None of cols 8/33/58/83 overlap with bound cols 0/25/50/75, so
        // the bound check is not needed on the slow path.
        p[x_base] = on_l ? fg : bg;
        p[x_base + 1] = on_r ? fg : bg;
      }
    }
  }

  graphics_release_frame_buffer(ctx, fb);
}

static void tick_handler(struct tm *t, TimeUnits tu) {
  #ifdef FAKE_TIME
    s_current_time.tm_min++;
  #else
  s_current_time = *t;
  #endif
  start_animation();
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_screen_width = bounds.size.w;
  s_screen_height = bounds.size.h;
  s_num_columns = s_screen_width >> 1;
  s_first_animation = true;

  for (uint16_t x = 0; x < s_screen_width; x++) {
    s_gc_for_x[x] = ((uint32_t)x * GRID_COLS) / s_screen_width;
  }
  // xbound x's fall on {0, screen_width/4, screen_width/2, 3*screen_width/4}.
  // All are even, so each lands on a column's left pixel (x_base = col * 2).
  uint16_t xbound_step = s_screen_width >> 2;
  for (uint16_t col = 0; col < s_num_columns; col++) {
    uint16_t x_base = col << 1;
    s_col_is_bound[col] = (x_base % xbound_step) == 0;
  }
  for (uint16_t y = 0; y < s_screen_height; y++) {
    s_gr_for_y[y] = ((uint32_t)y * GRID_ROWS) / s_screen_height;
  }

  window_set_background_color(window, s_settings.bg_color);

  s_layer = layer_create(bounds);
  layer_set_update_proc(s_layer, update_proc);
  layer_add_child(root, s_layer);

  s_columns = calloc(s_num_columns, sizeof(Column));
  if (!s_columns) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Could not allocate column arrays");
    window_stack_pop(false);
    return;
  }

  memset(s_new_grid, 0, sizeof(s_new_grid));

  #ifdef FAKE_TIME
    s_current_time.tm_hour = 18;
    s_current_time.tm_min = 55;
    s_current_time.tm_mday = 23;
    s_current_time.tm_mon = 3;
  #endif

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
}

static void window_unload(Window *window) {
  tick_timer_service_unsubscribe();
  layer_destroy(s_layer);
  s_layer = NULL;
  free(s_columns);
  s_columns = NULL;
}

static void inbox_received(DictionaryIterator *iter, void *ctx) {
  Tuple *fg = dict_find(iter, MESSAGE_KEY_FOREGROUND_COLOR);
  Tuple *bg = dict_find(iter, MESSAGE_KEY_BACKGROUND_COLOR);
  if (fg) {
    s_settings.fg_color = GColorFromHEX(fg->value->int32);
  }
  if (bg) {
    s_settings.bg_color = GColorFromHEX(bg->value->int32);
  }
  if (fg || bg) {
    save_settings();
    window_set_background_color(s_window, s_settings.bg_color);
    start_animation();
  }
}

static void init(void) {
  srand(time(NULL));

  s_animation_impl.update = frame;
  load_settings();

  app_message_register_inbox_received(inbox_received);
  app_message_open(128, 128);

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);
}

static void deinit(void) {
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
