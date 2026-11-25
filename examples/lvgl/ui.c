/*
 * The screen: a status bar, the frequency, tuning step and band buttons.
 * The main dial tunes by the selected step; a band button jumps to the band.
 * Everything here runs in the GUI task.
 */
#include "FreeRTOS.h"
#include "task.h"
#include "lvgl.h"
#include "hw.h"

static lv_obj_t *freq_label, *clock_label, *mode_label;
static uint32_t freq_hz = 14100000;
static uint32_t step_hz = 10;
static int mode;

static const char *const modes[] = { "USB", "LSB", "CW", "AM", "FM" };
static const char *const band_map[] = { "3.5", "7", "10", "14", "18", "21", "24", "28", "50", "" };
static const uint32_t band_hz[] = { 3573000, 7074000, 10136000, 14100000, 18100000,
                                    21074000, 24915000, 28074000, 50313000 };
static const char *const step_map[] = { "10 Hz", "100 Hz", "1 kHz", "" };
static const uint32_t step_values[] = { 10, 100, 1000 };

static void show_freq(void)
{
    lv_label_set_text_fmt(freq_label, "%lu.%03lu.%02lu", (unsigned long)(freq_hz / 1000000),
                          (unsigned long)(freq_hz / 1000 % 1000),
                          (unsigned long)(freq_hz % 1000 / 10));
}

static void on_band(lv_event_t *e)
{
    uint32_t i = lv_buttonmatrix_get_selected_button(lv_event_get_target(e));

    if (i < sizeof(band_hz) / sizeof(band_hz[0])) {
        freq_hz = band_hz[i];
        show_freq();
    }
}

static void on_step(lv_event_t *e)
{
    uint32_t i = lv_buttonmatrix_get_selected_button(lv_event_get_target(e));

    if (i < 3) {
        step_hz = step_values[i];
    }
}

static void on_mode(lv_event_t *e)
{
    mode = (mode + 1) % 5;
    lv_label_set_text(mode_label, modes[mode]);
}

static lv_obj_t *button_row(lv_obj_t *parent, const char *const *map, int y, int h,
                            lv_event_cb_t cb, int checked)
{
    lv_obj_t *m = lv_buttonmatrix_create(parent);

    lv_buttonmatrix_set_map(m, (const char **)map);
    lv_obj_set_size(m, LCD_W - 16, h);
    lv_obj_align(m, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_pad_all(m, 4, 0);
    lv_obj_add_event_cb(m, cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (checked >= 0) {
        lv_buttonmatrix_set_button_ctrl_all(m, LV_BUTTONMATRIX_CTRL_CHECKABLE);
        lv_buttonmatrix_set_one_checked(m, true);
        lv_buttonmatrix_set_button_ctrl(m, checked, LV_BUTTONMATRIX_CTRL_CHECKED);
    }
    return m;
}

void ui_create(void)
{
    lv_obj_t *scr = lv_screen_active(), *bar, *mode_btn, *label;

    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    bar = lv_obj_create(scr);                               /* status bar */
    lv_obj_set_size(bar, LCD_W, 30);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 2, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    mode_btn = lv_button_create(bar);
    lv_obj_set_size(mode_btn, 70, 24);
    lv_obj_align(mode_btn, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_add_event_cb(mode_btn, on_mode, LV_EVENT_CLICKED, NULL);
    mode_label = lv_label_create(mode_btn);
    lv_label_set_text(mode_label, modes[0]);
    lv_obj_center(mode_label);
    label = lv_label_create(bar);
    lv_label_set_text(label, "FreeRTOS + LVGL");
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    clock_label = lv_label_create(bar);
    lv_obj_align(clock_label, LV_ALIGN_RIGHT_MID, -4, 0);

    freq_label = lv_label_create(scr);                      /* frequency */
    lv_obj_set_style_text_font(freq_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(freq_label, lv_color_white(), 0);
    lv_obj_align(freq_label, LV_ALIGN_TOP_MID, -20, 42);
    label = lv_label_create(scr);
    lv_label_set_text(label, "MHz");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_align_to(label, freq_label, LV_ALIGN_OUT_RIGHT_BOTTOM, 8, -6);
    show_freq();
    lv_obj_align_to(label, freq_label, LV_ALIGN_OUT_RIGHT_BOTTOM, 8, -6);

    button_row(scr, step_map, 104, 44, on_step, 0);          /* tuning step */
    label = lv_label_create(scr);
    lv_label_set_text(label, "Main dial tunes  -  hold POWER to switch off");
    lv_obj_set_style_text_color(label, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 154);
    button_row(scr, band_map, 180, 84, on_band, -1);        /* bands, MHz */
}

/* Called from the GUI loop: the main dial and the clock. */
void ui_poll(void)
{
    static uint32_t shown_s = ~0u;
    int counts = dial_read_delta();
    uint32_t s = xTaskGetTickCount() / configTICK_RATE_HZ;

    if (counts) {
        int64_t f = (int64_t)freq_hz + (int64_t)counts * step_hz;

        freq_hz = f < 30000 ? 30000 : f > 470000000 ? 470000000 : (uint32_t)f;
        show_freq();
    }
    if (s != shown_s) {
        shown_s = s;
        lv_label_set_text_fmt(clock_label, LV_SYMBOL_BATTERY_FULL "  %02lu:%02lu",
                              (unsigned long)(s / 60 % 100), (unsigned long)(s % 60));
    }
}

void ui_show_power_off(void)
{
    lv_obj_t *scr = lv_obj_create(NULL), *label = lv_label_create(scr);

    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_label_set_text(label, "POWER OFF...");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_48, 0);
    lv_obj_center(label);
    lv_screen_load(scr);
}
