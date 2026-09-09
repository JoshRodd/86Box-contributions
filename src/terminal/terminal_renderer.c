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
#include <86box/machine.h>
#include <86box/video.h>
#include <tigt.h>
#include <tigt_keyboard.h>
#include <tigt_presenter.h>
#include <tigt_video.h>
#include "terminal_renderer.h"


static void *terminal_keyboard;
static uint16_t terminal_print_screen_key, terminal_pause_key;
static bool terminal_initialized;
static bool terminal_suspended;
static tigt_presenter *terminal_presenter;
static tigt_video *terminal_text_decoders[MONITORS_NUM][2];

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

static void *
terminal_keyboard_create(void)
{
    terminal_print_screen_key = terminal_pause_key = 0;
    const bool at = keyboard_type != KEYBOARD_TYPE_PC_XT &&
                    (keyboard_type != KEYBOARD_TYPE_INTERNAL ||
                     machine_has_bus(machine, MACHINE_BUS_AT_KBD));
    return pc_xt_keyboard_v1_create(at ? PC_XT_KEYBOARD_V1_AT_SET1 :
                                        PC_XT_KEYBOARD_V1_XT_SET1);
}

static void
terminal_key(int down, uint16_t key)
{
    /* Keep each special key's press-time identity until release: the mapper
       may release a synthesized modifier before releasing the key itself. */
    if (key == 0x137) {
        if (down && !terminal_print_screen_key)
            terminal_print_screen_key =
                (keyboard_recv_ui(0x38) || keyboard_recv_ui(0x138)) ? 0x54 : 0x137;
        key = terminal_print_screen_key;
        if (!key)
            return;
        if (key == 0x137 && down)
            keyboard_input(1, 0x12a);
        keyboard_input(down, key);
        if (!down) {
            if (key == 0x137)
                keyboard_input(0, 0x12a);
            terminal_print_screen_key = 0;
        }
    } else if (key == 0x145) {
        if (down && !terminal_pause_key)
            terminal_pause_key =
                (keyboard_recv_ui(0x1d) || keyboard_recv_ui(0x11d)) ? 0x146 : 0x45;
        key = terminal_pause_key;
        if (!key)
            return;
        if (key == 0x45)
            keyboard_input(down, 0xe11d);
        keyboard_input(down, key);
        if (!down)
            terminal_pause_key = 0;
    } else
        keyboard_input(down, key);
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
    pc_xt_keyboard_v1_key_event keys[PC_XT_KEYBOARD_V1_EVENT_MAX_KEYS];
    const size_t count = tigt_keyboard_handle(terminal_keyboard, event, keys,
                                             PC_XT_KEYBOARD_V1_EVENT_MAX_KEYS);
    if (count == PC_XT_KEYBOARD_V1_ERROR)
        return;
    for (size_t index = 0; index < count; index++)
        terminal_key(keys[index].down, keys[index].key);
}

void
terminal_renderer_init(void)
{
    video_setblit(terminal_blit);
    if (terminal_initialized)
        return;
    const char *presentation = getenv("TIGT_PRESENTATION");
    if (presentation && *presentation) {
        const bool glass = !strcmp(presentation, "glass");
        const bool reversible = !strcmp(presentation, "adaptive-reversible");
        if (!glass && !reversible && strcmp(presentation, "adaptive"))
            fatal("Terminal: invalid TIGT_PRESENTATION\n");
        const tigt_presenter_config output = {
            TIGT_PRESENTER_ABI_VERSION, STDOUT_FILENO,
            glass ? TIGT_PRESENT_GLASS : TIGT_PRESENT_ADAPTIVE,
            TIGT_ENCODING_LOCALE, reversible
        };
        const int result = tigt_presenter_create(&output, &terminal_presenter);
        if (result != TIGT_OK)
            fatal("Terminal: presentation initialization failed (%d)\n", result);
        terminal_initialized = true;
        terminal_suspended = false;
        atexit(terminal_renderer_close);
        return;
    }
    if (!isatty(STDOUT_FILENO))
        return;
    terminal_keyboard = terminal_keyboard_create();
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
    if (terminal_presenter) {
        terminal_suspended = true;
        return;
    }
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
    if (terminal_presenter) {
        terminal_suspended = false;
        return;
    }
    terminal_keyboard = terminal_keyboard_create();
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
    if (terminal_presenter) {
        tigt_presenter_destroy(terminal_presenter);
        for (unsigned m = 0; m < MONITORS_NUM; m++)
            for (unsigned kind = 0; kind < 2; kind++) {
                tigt_video_destroy(terminal_text_decoders[m][kind]);
                terminal_text_decoders[m][kind] = NULL;
            }
        terminal_presenter = NULL;
        terminal_initialized = terminal_suspended = false;
        return;
    }
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
terminal_video_snapshot(const uint8_t *vram, const uint8_t *crtc,
                         uint8_t mode, int monochrome, const uint8_t *pcjr_array)
{
    if (!terminal_presenter || terminal_suspended || !crtc[1] || !crtc[6])
        return;
    if (!monochrome && (mode & 2))
        return; /* This output-only integration handles text modes. */
    const unsigned kind = monochrome ? 1 : 0;
    tigt_video **decoder = &terminal_text_decoders[monitor_index_global][kind];
    if (!*decoder) {
        *decoder = tigt_video_create(monochrome ? TIGT_VIDEO_MDA : TIGT_VIDEO_CGA);
        if (!*decoder)
            fatal("Terminal: could not create text decoder\n");
    }
    const uint16_t port = monochrome ? 0x3b4 : 0x3d4;
    for (unsigned reg = 12; reg <= 13; reg++) {
        tigt_video_write(*decoder, port, reg);
        tigt_video_write(*decoder, port + 1, crtc[reg]);
    }
    tigt_video_write(*decoder, port + 4, mode);
    const size_t aperture = monochrome ? 4096 : 16384;
    tigt_video_frame decoded;
    int result = tigt_video_decode_text(*decoder, vram, aperture, crtc[1], crtc[6], 1, &decoded);
    if (result != TIGT_OK)
        fatal("Terminal: coherent text decode failed (%d)\n", result);
    terminal_video_frame *storage = &terminal_frames[monitor_index_global];
    const unsigned start = (crtc[12] << 8) | crtc[13];
    const unsigned cursor = (((crtc[14] << 8) | crtc[15]) - start) & (aperture / 2 - 1);
    const size_t count = (size_t) decoded.width * decoded.height;
    memcpy(storage->cells, decoded.cells, count * sizeof(*storage->cells));
    if (mode & 8)
        for (size_t i = 0; i < count; i++) {
            const uint8_t attr = vram[((start + i) * 2 + 1) & (aperture - 1)];
            if (pcjr_array) {
                const unsigned mask = pcjr_array[1] & 15;
                const uint32_t *palette = monitors[monitor_index_global].mon_pal_lookup;
                storage->cells[i].foreground =
                    palette[16 + pcjr_array[16 + ((attr & 15) & mask)]] & 0xffffff;
                storage->cells[i].background =
                    palette[16 + pcjr_array[16 + (((attr >> 4) & ((mode & 0x20) ? 7 : 15)) & mask)]] & 0xffffff;
            }
            if (attr & 8)
                storage->cells[i].flags |= TIGT_PRESENT_BOLD;
            if ((attr & 0x70) == 0x70) {
                /* Glass output uses reverse as emphasis; ANSI applies SGR 7.
                   Normalize resolved colors so that inversion happens once. */
                const uint32_t foreground = storage->cells[i].foreground;
                storage->cells[i].foreground = storage->cells[i].background;
                storage->cells[i].background = foreground;
                storage->cells[i].flags |= TIGT_PRESENT_REVERSE;
            }
        }
    /* An off-screen hardware cursor has no visible glass-TTY position. Keep
       the visible image; a later visible cursor supplies its position again. */
    const unsigned position = cursor < count ? cursor : 0;
    const tigt_presenter_frame output = {
        storage->cells, decoded.width, decoded.height, decoded.width,
        position % decoded.width, position / decoded.width, monochrome ? 50 : 60, 0
    };
    result = tigt_presenter_present(terminal_presenter, &output);
    if (result < 0)
        fatal("Terminal: output presentation aborted (%d)\n", result);
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
    const monitor_t *monitor = &monitors[monitor_index];
    if (terminal_suspended || terminal_presenter)
        return;
    (void) tigt_set_display_technology(
        monitor->mon_vid_type == VIDEO_FLAG_TYPE_MDA ?
            TIGT_DISPLAY_MDA : TIGT_DISPLAY_GENERIC);
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
