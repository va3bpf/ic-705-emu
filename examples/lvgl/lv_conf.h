/* LVGL 9.2 configuration for the IC-705 demo: 480x272 RGB565, bare-metal libc, FreeRTOS. */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH              16

/* LVGL's own allocator, string and printf: no C library needed */
#define LV_USE_STDLIB_MALLOC        LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING        LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF       LV_STDLIB_BUILTIN
#define LV_MEM_SIZE                 (256 * 1024)

/* All LVGL calls happen in the GUI task, so LVGL needs no locking. */
#define LV_USE_OS                   LV_OS_NONE

#define LV_DEF_REFR_PERIOD          33
#define LV_DPI_DEF                  130
#define LV_DRAW_BUF_ALIGN           4
#define LV_USE_DRAW_SW              1
#define LV_DRAW_SW_COMPLEX          1

#define LV_USE_LOG                  0
#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1

#define LV_FONT_MONTSERRAT_14       1
#define LV_FONT_MONTSERRAT_20       1
#define LV_FONT_MONTSERRAT_48       1
#define LV_FONT_DEFAULT             &lv_font_montserrat_14

#define LV_USE_THEME_DEFAULT        1
#define LV_THEME_DEFAULT_DARK       1

/* features this demo does not use */
#define LV_USE_FLOAT                0
#define LV_USE_MATRIX               0
#define LV_USE_FS_STDIO             0
#define LV_USE_SYSMON               0
#define LV_USE_PROFILER             0
#define LV_BUILD_EXAMPLES           0
#define LV_USE_DEMO_WIDGETS         0

#endif
