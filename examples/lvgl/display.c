/*
 * LCD: VDC5 graphics plane GR0 shows an RGB565 frame buffer of 480x272 at
 * 0x20800000. LVGL renders in partial mode into a 40-line buffer, and the
 * flush callback copies each finished area into the frame buffer.
 */
#include <string.h>
#include "lvgl.h"
#include "hw.h"

#define DRAW_LINES 40

static uint8_t draw_buffer[LCD_W * DRAW_LINES * 2] __attribute__((aligned(4)));

static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *pixels)
{
    int width = lv_area_get_width(area);

    for (int y = area->y1; y <= area->y2; y++) {
        memcpy(&FRAME_BUFFER[y * LCD_W + area->x1], pixels, width * 2);
        pixels += width * 2;
    }
    lv_display_flush_ready(disp);
}

static void plane_on(void)
{
    GR0(0x0C) = (uint32_t)FRAME_BUFFER;         /* FLM2: frame buffer */
    GR0(0x10) = (LCD_W * 2) << 16;              /* FLM3: line stride */
    GR0(0x18) = (LCD_H - 1) << 16;              /* FLM5: lines - 1 */
    GR0(0x1C) = 0u << 28 | (LCD_W - 1) << 16;   /* FLM6: RGB565, width - 1 */
    GR0(0x24) = 0;                              /* AB2: vertical start */
    GR0(0x28) = 0;                              /* AB3: horizontal start */
    GR0(0x20) = 2;                              /* AB1: show the plane */
    GR0(0x04) = 1;                              /* FLM_RD: read the buffer */
    GR0(0x00) = 0x111;                          /* apply at the next vsync */
}

void display_init(void)
{
    lv_display_t *disp;

    memset(FRAME_BUFFER, 0, LCD_W * LCD_H * 2);
    plane_on();
    disp = lv_display_create(LCD_W, LCD_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, draw_buffer, NULL, sizeof(draw_buffer),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush);
}

void display_off(void)
{
    GR0(0x04) = 0;
    GR0(0x20) = 0;
    GR0(0x00) = 0x111;
}
