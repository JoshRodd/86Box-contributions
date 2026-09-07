/* Exercise the actual PCjr rasterizer and terminal adapter with one frame sink.
   No ROM or emulator process is needed: the test font has both ink and paper. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <tigt.h>

static tigt_text_cell submitted[320 * 128];
static unsigned submitted_columns, submitted_rows;
static tigt_overscan submitted_overscan;
static int capture_text(const tigt_text_cell *cells, uint16_t columns, uint16_t rows, uint16_t stride)
{
    submitted_columns = columns;
    submitted_rows = rows;
    for (unsigned row = 0; row < rows; row++)
        memcpy(submitted + row * columns, cells + row * stride, columns * sizeof(*cells));
    return TIGT_OK;
}
static int capture_overscan(const tigt_overscan *overscan)
{
    submitted_overscan = *overscan;
    return TIGT_OK;
}
static unsigned bitmap_submissions;
static int capture_bitmap(const uint32_t *image, uint16_t width, uint16_t height,
                           uint16_t stride, uint8_t pixel_width)
{
    assert(width == 640 && height == 200 && pixel_width == 1);
    for (unsigned y = 0; y < height; y++)
        for (unsigned x = 0; x < width; x++)
            assert(image[y * stride + x] == ((y << 16) | x));
    bitmap_submissions++;
    return TIGT_OK;
}
#define tigt_present_bitmap capture_bitmap
#define tigt_present_text capture_text
#define tigt_set_overscan capture_overscan
#include "../../src/terminal/terminal_renderer.c"
#undef tigt_present_text
#undef tigt_set_overscan
#undef tigt_present_bitmap
#include "../../src/video/vid_pcjr.c"

monitor_t monitors[MONITORS_NUM];
int monitor_index_global;
int enable_overscan;
uint8_t fontdat[2048][8];

void hline(bitmap_t *bitmap, int x1, int y, int x2, uint32_t color)
{
    for (int x = x1; x < x2; x++)
        bitmap->line[y][x] = color;
}
void video_process_8_monitor(int width, int y, int monitor)
{
    for (int x = 0; x < width; x++) {
        uint32_t *pixel = &monitors[monitor].target_buffer->line[y][x];
        *pixel = monitors[monitor].mon_pal_lookup[*pixel];
    }
}
uint32_t *Composite_Process(uint8_t mode, uint8_t border, uint32_t blocks, uint32_t *pixels)
{
    (void) mode; (void) border; (void) blocks; (void) pixels;
    assert(!"RGB fixture must not enter composite conversion");
    return pixels;
}

static uint32_t rgb[256];
static uint32_t pixels[432][1024];
static uint8_t vram[16384];
static bitmap_t bitmap;

static void draw(pcjr_t *pcjr, uint8_t mode, uint16_t address)
{
    pcjr->array[0] = mode;
    terminal_video_begin();
    for (unsigned scanline = 0; scanline < 8; scanline++) {
        pcjr->scanline = scanline;
        pcjr->memaddr = address;
        vid_render(pcjr, scanline + 8, 32, 16);
    }
    terminal_video_overscan(rgb[20], 16, 16, 8, 8);
    terminal_video_blit(0, 0, 64, 24, 0);
    assert(submitted_columns == 2 && submitted_rows == 1);
}

int main(void)
{
    for (unsigned y = 0; y < 432; y++)
        bitmap.line[y] = pixels[y];
    bitmap.w = 1024;
    bitmap.h = 432;
    monitors[0].target_buffer = &bitmap;
    monitors[0].mon_pal_lookup = rgb;
    for (unsigned color = 0; color < 256; color++)
        rgb[color] = 0xff000000u | color * 0x010101u;
    memset(fontdat['A'], 0x80, sizeof(fontdat['A']));
    memset(fontdat['Z'], 0x40, sizeof(fontdat['Z']));
    pcjr_t pcjr = { 0 };
    pcjr.vram = vram;
    pcjr.crtc[1] = 2;
    pcjr.array[1] = 15;
    for (unsigned color = 0; color < 16; color++)
        pcjr.array[16 + color] = color;
    /* Deliberately disagree with the CRTC display start: consume the native
       renderer's latched memory address, not a reconstructed VRAM traversal. */
    pcjr.crtc[12] = 0;
    pcjr.crtc[13] = 0;
    vram[0] = 'Z';
    vram[1] = 0x12;
    vram[0x200] = 'A';
    vram[0x201] = 0x47;
    vram[0x202] = 'Z';
    vram[0x203] = 0x27;
    for (unsigned mode = 0; mode <= 1; mode++) {
        for (unsigned color = 0; color < 16; color++) {
            pcjr.array[23] = color;
            draw(&pcjr, mode, 0x100);
            assert(submitted[0].codepoint == 'A' && submitted[1].codepoint == 'Z');
            assert(submitted[0].foreground == (rgb[16 + color] & 0xffffff));
            assert(submitted[0].background == (rgb[20] & 0xffffff));
            /* Compare both consumers of the native colour/glyph decision. */
            assert(submitted[0].foreground == (rgb[pixels[8][16]] & 0xffffff));
            assert(submitted[0].background == (rgb[pixels[8][mode ? 17 : 18]] & 0xffffff));
        }
        pcjr.array[1] = 3;
        pcjr.array[19] = 12;
        draw(&pcjr, mode, 0x100);
        assert(submitted[0].foreground == (rgb[28] & 0xffffff));
        assert(submitted[0].background == (rgb[16] & 0xffffff));
        pcjr.array[1] = 15;
        pcjr.array[3] = 4;
        pcjr.blink = 16;
        vram[0x201] = 0xc7;
        draw(&pcjr, mode, 0x100);
        assert(submitted[0].foreground == submitted[0].background);
        pcjr.blink = 0;
        draw(&pcjr, mode, 0x100);
        assert(submitted[0].foreground != submitted[0].background);
        pcjr.array[3] = 0;
        vram[0x201] = 0x47;
    }
    pcjr.crtc[14] = 1;
    pcjr.crtc[15] = 0;
    pcjr.cursorvisible = pcjr.cursoron = 1;
    draw(&pcjr, 0, 0x100);
    assert(submitted[0].flags & TIGT_TEXT_CURSOR);
    pcjr.cursoron = 0;
    draw(&pcjr, 0, 0x100);
    assert(!(submitted[0].flags & TIGT_TEXT_CURSOR));
    /* Border metadata comes from the same helper that paints vertical overscan. */
    pcjr.crtc[2] = 45;
    pcjr.firstline = 0;
    pcjr.lastline = 8;
    pcjr.array[2] = 5;
    vid_blit_v_overscan(&pcjr);
    terminal_video_blit(0, 0, 64, 24, 0);
    assert(submitted_overscan.color == (pixels[0][0] & 0xffffff));
    assert(submitted_overscan.left + submitted_overscan.right == vid_get_h_overscan_size(&pcjr));
    assert(submitted_overscan.top == 8 && submitted_overscan.bottom == 8);
    monitors[0].mon_res_x = 640;
    monitors[0].mon_res_y = 200;
    monitors[0].mon_bpp = 1;
    for (enable_overscan = 0; enable_overscan <= 1; enable_overscan++) {
        for (unsigned scale = 1; scale <= 2; scale++) {
            memset(pixels, 0xff, sizeof(pixels));
            for (unsigned y = 0; y < 200; y++)
                for (unsigned x = 0; x < 640; x++)
                    pixels[(y + 8) * scale][x + 16] = (y << 16) | x;
            terminal_video_begin();
            terminal_video_overscan(rgb[20], 16, 16, 8, 8);
            terminal_video_blit(enable_overscan ? 0 : 16, enable_overscan ? 0 : 8 * scale,
                                enable_overscan ? 672 : 640,
                                (enable_overscan ? 216 : 200) * scale, 0);
        }
    }
    assert(bitmap_submissions == 4);
    puts("PASS native PCjr text palette, mask, latched address, blink, cursor and border");
    return 0;
}
