/* 86Box-specific adapter for tigt and the PC keyboard mapper. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>

#include <86box/86box.h>
#include <86box/keyboard.h>
#include <86box/video.h>
#include <tigt.h>
#include <tigt_keyboard.h>
#include "terminal_renderer.h"


static void *terminal_keyboard;
static bool terminal_initialized;
static bool terminal_suspended;

#define TERMINAL_TEXT_COLUMNS 320
#define TERMINAL_TEXT_ROWS 128
typedef struct {
    tigt_text_cell cells[TERMINAL_TEXT_COLUMNS * TERMINAL_TEXT_ROWS];
    uint32_t cell_frame[TERMINAL_TEXT_COLUMNS * TERMINAL_TEXT_ROWS];
    uint32_t frame;
    uint16_t columns, rows;
    tigt_overscan overscan;
} terminal_video_frame;

static terminal_video_frame terminal_frames[MONITORS_NUM];

static void
terminal_blit(int x, int y, int width, int height, int monitor_index)
{
    (void) x;
    (void) y;
    (void) width;
    (void) height;
    video_blit_complete_monitor(monitor_index);
}

static void
terminal_input(const tigt_input_event *event, void *user)
{
    (void) user;
    if ((event->kind == TIGT_PRESS || event->kind == TIGT_REPEAT) &&
        (event->modifiers & TIGT_MOD_CONTROL) && event->key.kind == TIGT_KEY_CHAR) {
        if (event->key.character == 'c' || event->key.character == 'C') {
            kill(getpid(), SIGINT);
            return;
        }
        if (event->key.character == 'z' || event->key.character == 'Z') {
            kill(getpid(), SIGTSTP);
            return;
        }
    }
    uint8_t bytes[PC_XT_KEYBOARD_V1_EVENT_MAX_BYTES];
    const size_t count = tigt_keyboard_handle(terminal_keyboard, event, bytes, sizeof(bytes));
    if (count == PC_XT_KEYBOARD_V1_ERROR)
        return;
    for (size_t index = 0; index < count; index++)
        keyboard_input_set1(bytes[index]);
}

void
terminal_renderer_init(void)
{
    video_setblit(terminal_blit);
    if (terminal_initialized || !isatty(STDOUT_FILENO))
        return;
    terminal_keyboard = pc_xt_keyboard_v1_create(PC_XT_KEYBOARD_V1_XT_SET1);
    if (terminal_keyboard == NULL)
        fatal("Terminal: could not create keyboard mapper\n");
    const tigt_config config = { TIGT_ABI_VERSION, terminal_input, NULL };
    const int result = tigt_init(&config);
    if (result != TIGT_OK) {
        pc_xt_keyboard_v1_destroy(terminal_keyboard);
        terminal_keyboard = NULL;
        fatal("Terminal: tigt initialization failed (%d)\n", result);
    }
    terminal_initialized = true;
    terminal_suspended = false;
    atexit(terminal_renderer_close);
}

void
terminal_renderer_suspend(void)
{
    if (!terminal_initialized || terminal_suspended)
        return;
    tigt_suspend();
    keyboard_all_up();
    pc_xt_keyboard_v1_destroy(terminal_keyboard);
    terminal_keyboard = NULL;
    terminal_suspended = true;
}

void
terminal_renderer_resume(void)
{
    if (!terminal_initialized || !terminal_suspended)
        return;
    terminal_keyboard = pc_xt_keyboard_v1_create(PC_XT_KEYBOARD_V1_XT_SET1);
    if (terminal_keyboard == NULL)
        fatal("Terminal: could not recreate keyboard mapper\n");
    const int result = tigt_resume();
    if (result != TIGT_OK)
        fatal("Terminal: tigt resume failed (%d)\n", result);
    terminal_suspended = false;
}

void
terminal_renderer_close(void)
{
    if (!terminal_initialized)
        return;
    tigt_shutdown();
    keyboard_all_up();
    pc_xt_keyboard_v1_destroy(terminal_keyboard);
    terminal_keyboard = NULL;
    terminal_initialized = false;
    terminal_suspended = false;
}

void
terminal_video_begin(void)
{
    terminal_video_frame *frame = &terminal_frames[monitor_index_global];
    if (++frame->frame == 0) {
        memset(frame->cell_frame, 0, sizeof(frame->cell_frame));
        frame->frame = 1;
    }
    frame->columns = frame->rows = 0;
    frame->overscan = (tigt_overscan) { 0 };
}

void
terminal_video_text_cell(uint16_t column, uint16_t row, uint8_t character,
                         uint32_t foreground, uint32_t background,
                         int underline, int cursor)
{
    if (column >= TERMINAL_TEXT_COLUMNS || row >= TERMINAL_TEXT_ROWS)
        return;
    terminal_video_frame *frame = &terminal_frames[monitor_index_global];
    const size_t index = (size_t) row * TERMINAL_TEXT_COLUMNS + column;
    tigt_text_cell *cell = &frame->cells[index];
    /* Sample the first rendered scanline, not a second walk through VRAM.
       Decorations may occur on later scanlines, so accumulate those. */
    if (frame->cell_frame[index] != frame->frame) {
        *cell = (tigt_text_cell) {
            tigt_cp437_codepoint(character), foreground & 0xffffff,
            background & 0xffffff, 0
        };
        frame->cell_frame[index] = frame->frame;
    }
    if (underline)
        cell->flags |= TIGT_TEXT_UNDERLINE;
    if (cursor)
        cell->flags |= TIGT_TEXT_CURSOR;
    if (column >= frame->columns)
        frame->columns = column + 1;
    if (row >= frame->rows)
        frame->rows = row + 1;
}

void
terminal_video_overscan(uint32_t color, uint16_t left, uint16_t right,
                        uint16_t top, uint16_t bottom)
{
    terminal_frames[monitor_index_global].overscan =
        (tigt_overscan) { color & 0xffffff, left, right, top, bottom };
}

void
terminal_video_blit(int x, int y, int width, int height, int monitor_index)
{
    terminal_video_frame *frame = &terminal_frames[monitor_index];
    (void) tigt_set_overscan(&frame->overscan);
    if (frame->columns && frame->rows) {
        for (uint16_t row = 0; row < frame->rows; row++) {
            for (uint16_t column = 0; column < frame->columns; column++) {
                const size_t index = (size_t) row * TERMINAL_TEXT_COLUMNS + column;
                if (frame->cell_frame[index] != frame->frame)
                    frame->cells[index] = (tigt_text_cell) { ' ', 0, 0, 0 };
            }
        }
        (void) tigt_present_text(frame->cells, frame->columns, frame->rows,
                                 TERMINAL_TEXT_COLUMNS);
        return;
    }

    const monitor_t *monitor = &monitors[monitor_index];
    const bitmap_t *buffer = monitor->target_buffer;
    const int logical_width = (int) monitor->mon_res_x;
    if (monitor->mon_bpp <= 0 || (logical_width != 320 && logical_width != 640) ||
        monitor->mon_res_y != 200 || buffer == NULL)
        return;
    const uint8_t pixel_width = logical_width == 320 ? 2 : 1;
    const int border_height = enable_overscan ?
        frame->overscan.top + frame->overscan.bottom : 0;
    const int native_height = 200 + border_height;
    const int line_scale = height == native_height ? 1 :
                           height == native_height * 2 ? 2 : 0;
    if (!line_scale)
        return;
    if (enable_overscan) {
        x += frame->overscan.left;
        y += frame->overscan.top * line_scale;
        width -= frame->overscan.left + frame->overscan.right;
    }
    if (width != logical_width * pixel_width || x < 0 || y < 0 ||
        x > buffer->w - width || y > buffer->h - 200 * line_scale ||
        buffer->w > UINT16_MAX / line_scale)
        return;
    (void) tigt_present_bitmap(buffer->line[y] + x, width, 200,
                               buffer->w * line_scale, pixel_width);
}
