/* Exercise MDA video enable through the actual rasterizer and terminal adapter.
   Synthetic glyphs need no ROM and expose ink, paper, underline and cursor. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <tigt.h>

static tigt_text_cell submitted[4];
static unsigned text_submissions;
static int capture_text(const tigt_text_cell *cells, uint16_t columns, uint16_t rows, uint16_t stride)
{
    assert(columns == 4 && rows == 1 && stride >= columns);
    memcpy(submitted, cells, sizeof(submitted));
    text_submissions++;
    return TIGT_OK;
}
static int capture_overscan(const tigt_overscan *overscan)
{
    (void) overscan;
    return TIGT_OK;
}
static int capture_bitmap(const uint32_t *image, uint16_t width, uint16_t height,
                          uint16_t stride, uint8_t pixel_width)
{
    (void) image; (void) width; (void) height; (void) stride; (void) pixel_width;
    assert(!"MDA text must not fall back to bitmap submission");
    return TIGT_OK;
}
#define tigt_present_text capture_text
#define tigt_set_overscan capture_overscan
#define tigt_present_bitmap capture_bitmap
#include "../../src/terminal/terminal_renderer.c"
#undef tigt_present_text
#undef tigt_set_overscan
#undef tigt_present_bitmap
#include "../../src/video/vid_mda.c"

monitor_t monitors[MONITORS_NUM];
int monitor_index_global;
int enable_overscan;
int frames;
double cpuclock = 4772728.0;
uint64_t MDACONST = 1ULL << 32;
uint8_t fontdatm[2048][16];

void fatal(const char *fmt, ...)
{
    fputs(fmt, stderr);
    abort();
}

/* Raster-only fixtures must never activate stdin. */
int machine, keyboard_type;
int machine_has_bus(int m, uintptr_t flags) { (void) m; (void) flags; abort(); }
void keyboard_input(int down, uint16_t scan) { (void) down; (void) scan; abort(); }
int keyboard_recv_ui(uint16_t scan) { (void) scan; abort(); }
void keyboard_all_up(void) { abort(); }

/* Host services are inert; palette conversion and MDA scanout remain real. */
void timer_enable(pc_timer_t *timer) { (void) timer; }
void timer_add(pc_timer_t *timer, void (*callback)(void *), void *priv, int start_timer)
{
    (void) timer; (void) callback; (void) priv; (void) start_timer;
}
int device_get_config_int(const char *name)
{
    assert(strcmp(name, "rgb_type") == 0);
    return MDA_MONITOR_TYPE_DEFAULT;
}
void cgapal_rebuild_monitor(int monitor) { (void) monitor; }
void video_wait_for_buffer_monitor(int monitor) { (void) monitor; }
void video_lightpen_hsync(void) {}
void video_lightpen_vsync(void) {}
void video_lightpen_check_trigger_strobe(int x_offset, int y, int x_offset_from_hsync,
                                         int firstline, double pix_clock, int monitor_used)
{
    (void) x_offset; (void) y; (void) x_offset_from_hsync;
    (void) firstline; (void) pix_clock; (void) monitor_used;
}
uint8_t video_force_resize_get_monitor(int monitor) { (void) monitor; return 0; }
void video_force_resize_set_monitor(uint8_t res, int monitor) { (void) res; (void) monitor; }
void set_screen_size(int x, int y) { (void) x; (void) y; }
void video_blit_memtoscreen_monitor(int x, int y, int w, int h, int monitor)
{
    terminal_video_blit(x, y, w, h, monitor);
}
void video_process_8_monitor(int width, int y, int monitor)
{
    for (int x = 0; x < width; x++) {
        uint32_t *pixel = &monitors[monitor].target_buffer->line[y][x];
        *pixel = monitors[monitor].mon_pal_lookup[*pixel];
    }
}

static uint32_t rgb[256];
static uint32_t pixels[14][36];
static uint8_t vram[MDA_VRAM];
static bitmap_t bitmap;

static void draw(mda_t *mda, uint8_t mode)
{
    uint8_t saved_vram[MDA_VRAM], saved_crtc[MDA_CRTC_NUM_REGISTERS];
    memcpy(saved_vram, vram, sizeof(saved_vram));
    memcpy(saved_crtc, mda->crtc, sizeof(saved_crtc));
    const uint64_t previous_time = mda->timer.ts_integer;
    const unsigned previous_submissions = text_submissions;
    mda_out(MDA_REGISTER_MODE_CONTROL, mode, mda);
    /* Start one complete character row, then let both half-line phases run.
       Video enable must not stop the beam, address counter or cursor timing. */
    mda->memaddr = mda->memaddr_backup = 0x100;
    mda->vc = mda->scanline = mda->linepos = mda->displine = 0;
    mda->firstline = 1000;
    mda->lastline = 0;
    mda->dispon = mda->cursoron = 1;
    mda->cursorvisible = 0;
    memset(pixels, 0xff, sizeof(pixels));
    for (unsigned half_line = 0; half_line < 28; half_line++)
        mda_poll(mda);
    terminal_video_blit(0, 0, 36, 14, 0);
    assert(text_submissions == previous_submissions + 1);
    assert(mda->timer.ts_integer == previous_time + 84);
    assert(mda->memaddr == 0x104 && mda->memaddr_backup == 0x104);
    assert(mda->vc == 1 && mda->scanline == 0 && mda->displine == 14);
    assert(mda->linepos == 0 && !mda->dispon && !mda->cursorvisible);
    assert(mda->mode == mode);
    assert(memcmp(vram, saved_vram, sizeof(vram)) == 0);
    assert(memcmp(mda->crtc, saved_crtc, sizeof(saved_crtc)) == 0);
}

static void assert_blank(void)
{
    for (unsigned cell = 0; cell < 4; cell++) {
        /* The codepoint is immaterial when both colours are black. */
        assert(submitted[cell].foreground == 0 && submitted[cell].background == 0);
        assert(!(submitted[cell].flags & (TIGT_TEXT_CURSOR | TIGT_TEXT_UNDERLINE)));
    }
    for (unsigned y = 0; y < 14; y++)
        for (unsigned x = 0; x < 36; x++)
            assert((pixels[y][x] & 0xffffff) == 0);
}

static void assert_enabled(void)
{
    const char glyphs[] = "AUCR";
    const uint32_t foreground[] = { 0xffffff, 0xaaaaaa, 0xaaaaaa, 0 };
    const uint32_t background[] = { 0, 0, 0, 0xffffff };
    for (unsigned cell = 0; cell < 4; cell++) {
        assert(submitted[cell].codepoint == (uint32_t) glyphs[cell]);
        assert(submitted[cell].foreground == foreground[cell]);
        assert(submitted[cell].background == background[cell]);
        assert(!!(submitted[cell].flags & TIGT_TEXT_UNDERLINE) == (cell == 1));
        assert(!!(submitted[cell].flags & TIGT_TEXT_CURSOR) == (cell == 2));
        for (unsigned y = 0; y < 14; y++) {
            for (unsigned x = 0; x < 9; x++) {
                int ink = x < 8 && (fontdatm[(unsigned) glyphs[cell]][y] & (0x80 >> x));
                if (cell == 1 && y == 12)
                    ink = 1;
                if (cell == 2 && y >= 11)
                    ink = !ink;
                const uint32_t expected = ink ? submitted[cell].foreground : submitted[cell].background;
                assert((pixels[y][cell * 9 + x] & 0xffffff) == expected);
            }
        }
    }
}

int main(void)
{
    int palette_selection = 0;
    for (unsigned y = 0; y < 14; y++)
        bitmap.line[y] = pixels[y];
    bitmap.w = 36;
    bitmap.h = 14;
    monitors[0].target_buffer = &bitmap;
    monitors[0].mon_pal_lookup = rgb;
    monitors[0].mon_cga_palette = &palette_selection;
    monitors[0].mon_vid_type = VIDEO_FLAG_TYPE_MDA;
    /* Include low palette aliases used by the native cursor XOR. */
    rgb[7] = rgb[23] = 0xaaaaaa;
    rgb[15] = rgb[31] = 0xffffff;
    memset(fontdatm['A'], 0x80, sizeof(fontdatm['A']));
    memset(fontdatm['U'], 0x40, sizeof(fontdatm['U']));
    memset(fontdatm['C'], 0x20, sizeof(fontdatm['C']));
    memset(fontdatm['R'], 0x10, sizeof(fontdatm['R']));
    mda_t mda = { 0 };
    mda.vram = vram;
    mda_init(&mda);
    mda.crtc[MDA_CRTC_HTOTAL] = 5;
    mda.crtc[MDA_CRTC_HDISP] = 4;
    mda.crtc[MDA_CRTC_VTOTAL] = 3;
    mda.crtc[MDA_CRTC_VDISP] = 1;
    mda.crtc[MDA_CRTC_VSYNC] = 2;
    mda.crtc[MDA_CRTC_MAX_SCANLINE_ADDR] = 13;
    mda.crtc[MDA_CRTC_START_ADDR_HIGH] = 1;
    mda.crtc[MDA_CRTC_CURSOR_ADDR_HIGH] = 1;
    mda.crtc[MDA_CRTC_CURSOR_ADDR_LOW] = 2;
    mda.crtc[MDA_CRTC_CURSOR_START] = 11;
    mda.crtc[MDA_CRTC_CURSOR_END] = 13;
    mda_recalctimings(&mda);
    /* Bright glyph, underline, cursor, reverse video: none may leak while off. */
    const uint8_t text[] = { 'A', 0x0f, 'U', 0x01, 'C', 0x07, 'R', 0x70 };
    memcpy(vram + 0x200, text, sizeof(text));
    draw(&mda, MDA_MODE_HIGHRES);
    assert_blank();
    draw(&mda, MDA_MODE_HIGHRES | MDA_MODE_VIDEO_ENABLE);
    assert_enabled();
    draw(&mda, MDA_MODE_HIGHRES);
    assert_blank();
    puts("PASS native/terminal MDA video off/on/off, colors, cursor, underline and preserved scanout state");
    return 0;
}
