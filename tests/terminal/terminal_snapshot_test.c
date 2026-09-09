/* Decode terminal output from the real adaptive presenter, not a mock sink. */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "../../src/terminal/terminal_renderer.c"

monitor_t monitors[MONITORS_NUM];
int monitor_index_global;
void fatal(const char *fmt, ...) { fputs(fmt, stderr); abort(); }

/* This fixture supplies only an output fd; it must never activate stdin. */
int machine, keyboard_type;
int machine_has_bus(int m, uintptr_t flags) { (void) m; (void) flags; abort(); }
void keyboard_input(int down, uint16_t scan) { (void) down; (void) scan; abort(); }
int keyboard_recv_ui(uint16_t scan) { (void) scan; abort(); }
void keyboard_all_up(void) { abort(); }

/* Track SGR colors to check what the host terminal displays for our marker. */
static void check_marker(const char *p, uint32_t expected_fg, uint32_t expected_bg)
{
    uint32_t fg = 0xaaaaaa, bg = 0;
    int reverse = 0, found = 0;
    unsigned row = 1, column = 1;
    while (*p) {
        if (p[0] == '\033' && p[1] == '[') {
            p += 2;
            unsigned params[32], n = 0;
            while (*p && !(*p >= '@' && *p <= '~')) {
                if (*p >= '0' && *p <= '9') {
                    char *end;
                    unsigned value = strtoul(p, &end, 10);
                    assert(n < 32);
                    params[n++] = value;
                    p = end;
                } else p++;
            }
            if (*p == 'm') {
                for (unsigned i = 0; i < n; i++) {
                    if (!params[i]) { fg = 0xaaaaaa; bg = 0; reverse = 0; }
                    else if (params[i] == 7) reverse = 1;
                    else if (params[i] == 27) reverse = 0;
                    else if ((params[i] == 38 || params[i] == 48) && i + 4 < n && params[i + 1] == 2) {
                        uint32_t rgb = (params[i + 2] << 16) | (params[i + 3] << 8) | params[i + 4];
                        if (params[i] == 38) fg = rgb; else bg = rgb;
                        i += 4;
                    }
                }
            }
            if (*p == 'H' || *p == 'f') {
                row = n && params[0] ? params[0] : 1;
                column = n > 1 && params[1] ? params[1] : 1;
            } else if (*p == 'G')
                column = n && params[0] ? params[0] : 1;
            else if (*p == 'd')
                row = n && params[0] ? params[0] : 1;
            if (*p) p++;
        } else {
            const char byte = *p++;
            if (byte == 'X') {
                assert(row == 4 && column == 1);
                assert((reverse ? bg : fg) == expected_fg);
                assert((reverse ? fg : bg) == expected_bg);
                found = 1;
            }
            if (byte == '\r') column = 1;
            else if (byte == '\n') row++;
            else if (byte == '\b' && column > 1) column--;
            else if ((unsigned char) byte >= 0x20) column++;
        }
    }
    assert(found);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const int mono = !strcmp(argv[1], "mda");
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    assert(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0);
    int slave = open(ptsname(master), O_RDWR | O_NOCTTY);
    assert(slave >= 0);
    struct winsize size = { .ws_col = 10, .ws_row = 6 };
    assert(ioctl(slave, TIOCSWINSZ, &size) == 0);
    const tigt_presenter_config config = {
        TIGT_PRESENTER_ABI_VERSION, slave, TIGT_PRESENT_ADAPTIVE, TIGT_ENCODING_ASCII, 0
    };
    assert(tigt_presenter_create(&config, &terminal_presenter) == TIGT_OK);
    static uint8_t vram[16384];
    uint8_t crtc[18] = { 0 }, array[32] = { 0 };
    static uint32_t palette[256];
    monitors[0].mon_pal_lookup = palette;
    for (unsigned i = 0; i < 16; i++) array[16 + i] = i;
    palette[20] = 0xaa0000;
    palette[18] = 0x00aa00;
    array[1] = 15;
    array[23] = 4; /* Guest foreground 7 is red, not CGA gray. */
    crtc[1] = 4; crtc[6] = 2; crtc[15] = 5;
    for (unsigned i = 0; i < 8; i++) { vram[2 * i] = ' '; vram[2 * i + 1] = mono ? 0x70 : 0x07; }
    vram[0] = 'A'; vram[8] = 'B';
    terminal_video_snapshot(vram, crtc, 8, mono, mono ? NULL : array);
    /* This output-only fixture supplies the current host position itself;
       production obtains it asynchronously from the shared terminal's CPR. */
    assert(tigt_presenter_observe_cursor(terminal_presenter, 2, 3) == TIGT_OK);
    /* Rewriting an earlier line eventually requires adaptive fullscreen. */
    vram[0] = 'X';
    for (unsigned i = 0; i < 10; i++)
        terminal_video_snapshot(vram, crtc, 8, mono, mono ? NULL : array);
    char output[16384];
    ssize_t amount = read(master, output, sizeof(output) - 1);
    assert(amount > 0); output[amount] = 0;
    check_marker(output, mono ? 0 : 0xaa0000, mono ? 0xffffff : 0);
    if (!mono) {
        /* A palette-only update, including the mask, must redraw text colors. */
        array[1] = 3; array[19] = 2;
        terminal_video_snapshot(vram, crtc, 8, 0, array);
        amount = read(master, output, sizeof(output) - 1);
        assert(amount > 0); output[amount] = 0;
        check_marker(output, 0x00aa00, 0);
    }
    tigt_presenter_destroy(terminal_presenter);
    terminal_presenter = NULL;
    for (unsigned i = 0; i < 2; i++) tigt_video_destroy(terminal_text_decoders[0][i]);
    close(slave); close(master);
    puts("PASS adaptive terminal colors match hardware attributes/palette");
    return 0;
}
