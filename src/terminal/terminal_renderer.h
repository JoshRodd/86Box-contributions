#ifndef TERMINAL_RENDERER_H
#define TERMINAL_RENDERER_H

#include <stdint.h>
void terminal_renderer_init(void);
void terminal_renderer_close(void);
void terminal_renderer_suspend(void);
void terminal_renderer_resume(void);
/* Native video renderers publish resolved cells while walking the beam. The
   shared blit boundary submits that frame; no VRAM or registers cross here. */
void terminal_video_begin(void);
void terminal_video_text_cell(uint16_t column, uint16_t row, uint8_t character,
                              uint32_t foreground, uint32_t background,
                              int underline, int cursor);
/* Output-only modes sample coherent text memory and CRTC state at vsync.
   The existing curses renderer still consumes rasterized cells.
   pcjr_array is the 32 gate-array registers, or NULL for CGA/MDA. */
void terminal_video_snapshot(const uint8_t *vram, const uint8_t *crtc,
                              uint8_t mode, int monochrome, const uint8_t *pcjr_array);
/* Border dimensions are native pixels, before host scanline doubling. */
void terminal_video_overscan(uint32_t color, uint16_t left, uint16_t right,
                             uint16_t top, uint16_t bottom);
void terminal_video_blit(int x, int y, int width, int height, int monitor_index);

#endif
