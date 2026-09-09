#ifndef TERMINAL_RENDERER_H
#define TERMINAL_RENDERER_H

#include <stdint.h>
#include <stddef.h>
/* Opt-in, read-only boot evidence. Registers are AX..DX, SI, DI, BP, SP,
   CS, IP, DS, ES, SS, FLAGS; guest memory is never read through device hooks. */
int terminal_boot_trace_enabled(void);
void terminal_boot_trace_interrupt(unsigned vector, const uint16_t regs[14],
                                   const uint8_t *ram, size_t ram_size);
void terminal_renderer_init(void);
void terminal_renderer_close(void);
void terminal_renderer_suspend(void);
void terminal_renderer_resume(void);
/* Nonblocking presenter stdin pump; call on the presentation/emulation thread,
   including paused iterations with no video frames. Curses polls independently. */
void terminal_renderer_poll_input(void);
/* Native video renderers publish resolved cells while walking the beam. The
   shared blit boundary submits that frame; no VRAM or registers cross here. */
void terminal_video_begin(void);
void terminal_video_text_cell(uint16_t column, uint16_t row, uint8_t character,
                              uint32_t foreground, uint32_t background,
                              int underline, int cursor);
/* Presenter modes sample coherent text memory and CRTC state at vsync.
   The existing curses renderer still consumes rasterized cells.
   pcjr_array is the 32 gate-array registers, or NULL for CGA/MDA.
   Mode bit 3 is hardware video enable, including synthesized PCjr mode.
   VRAM must expose the full 4KB MDA or 16KB CGA/PCjr aperture: hidden bytes
   and attributes also cancel the presenter's hardware-disable hold. */
void terminal_video_snapshot(const uint8_t *vram, const uint8_t *crtc,
                              uint8_t mode, int monochrome, const uint8_t *pcjr_array);
/* Border dimensions are native pixels, before host scanline doubling. */
void terminal_video_overscan(uint32_t color, uint16_t left, uint16_t right,
                             uint16_t top, uint16_t bottom);
void terminal_video_blit(int x, int y, int width, int height, int monitor_index);

#endif
