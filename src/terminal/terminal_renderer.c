/* 86Box-specific adapter for tigt and the PC keyboard mapper. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <termios.h>
/* POSIX carriage-return delay name conflicts with the CPU register member
   in translation units embedding this adapter alongside a video device. */
#ifdef CR0
#undef CR0
#endif
#include <unistd.h>
#include <string.h>
#include <time.h>

#include <86box/86box.h>
#include <86box/keyboard.h>
#include <86box/machine.h>
#include <86box/video.h>
#include <86box/plat.h>
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

/* Exact raw aperture from the last submitted frame, including attributes and
   off-screen bytes that disappear entirely when hardware output is disabled. */
static struct {
    uint8_t bytes[16384];
    size_t aperture;
    int machine, monitor;
    unsigned kind;
} terminal_video_memory;

/* Presenter input shares the emulation thread; curses owns its own decoder. */
static tigt_input *terminal_stdin_decoder;
static struct termios terminal_saved_input;
static bool terminal_input_saved, terminal_input_tty, terminal_input_active;
static bool terminal_input_fullscreen, terminal_input_protocol, terminal_input_eof;
/* Space for a paced cooked line plus input received during a cursor query. */
static uint8_t terminal_input_bytes[8192];
static size_t terminal_input_offset, terminal_input_length, terminal_input_cooked;
static uint64_t terminal_input_deadline, terminal_input_last_byte;
static unsigned terminal_input_events;
static bool terminal_input_echo_tty, terminal_input_baseline;
enum { TERMINAL_CURSOR_IDLE, TERMINAL_CURSOR_WAITING, TERMINAL_CURSOR_READY };
static unsigned terminal_cursor_query;
static bool terminal_cursor_mode;
static uint64_t terminal_cursor_deadline;
static uint8_t terminal_cursor_reply[32];
static size_t terminal_cursor_length;

static void terminal_stdin_mode(bool fullscreen);

/* One ordered stream shared by CPU observations and coherent video callbacks.
   Disabled by default; no emulated cycles, registers or memory are modified. */
static FILE *boot_trace;
static int boot_trace_checked;
static unsigned long long boot_trace_sequence;

int
terminal_boot_trace_enabled(void)
{
    if (!boot_trace_checked) {
        boot_trace_checked = 1;
        const char *path = getenv("86BOX_BOOT_TRACE");
        if (path && *path) {
            boot_trace = fopen(path, "wx");
            if (!boot_trace)
                fatal("Terminal: cannot create boot trace %s\n", path);
        }
    }
    return boot_trace != NULL;
}

static void
boot_trace_prefix(const char *kind)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    fprintf(boot_trace, "{\"kind\":\"%s\",\"seq\":%llu,\"host_ns\":%llu",
            kind, ++boot_trace_sequence,
            (unsigned long long) now.tv_sec * 1000000000ULL + now.tv_nsec);
}

static void
boot_trace_hex(const uint8_t *bytes, size_t count)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < count; i++) {
        fputc(hex[bytes[i] >> 4], boot_trace);
        fputc(hex[bytes[i] & 15], boot_trace);
    }
}

void
terminal_boot_trace_interrupt(unsigned vector, const uint16_t regs[14],
                              const uint8_t *ram, size_t ram_size)
{
    if (!terminal_boot_trace_enabled())
        return;
    boot_trace_prefix("int");
    fprintf(boot_trace, ",\"vector\":%u,\"regs\":[", vector);
    for (unsigned i = 0; i < 14; i++)
        fprintf(boot_trace, "%s%u", i ? "," : "", regs[i]);
    fputs("],\"bda_hex\":\"", boot_trace);
    if (ram_size >= 0x467)
        boot_trace_hex(ram + 0x449, 0x1e);
    fputs("\",\"buffer_hex\":\"", boot_trace);
    const unsigned ah = regs[0] >> 8;
    unsigned segment = regs[10], offset = regs[3], requested = 0;
    int dollar_terminated = 0, truncated = 0;
    if (vector == 0x21 && ah == 9) {
        requested = 4096;
        dollar_terminated = 1;
    } else if (vector == 0x21 && ah == 0x40)
        requested = regs[2];
    else if (vector == 0x10 && ah == 0x13) {
        segment = regs[11];
        offset = regs[6];
        requested = regs[2] * ((regs[0] & 2) ? 2 : 1);
    }
    unsigned i;
    for (i = 0; i < requested && i < 4096; i++) {
        const size_t address = ((segment << 4) + ((offset + i) & 0xffff)) & 0xfffff;
        if (address >= ram_size) {
            truncated = 1;
            break;
        }
        if (dollar_terminated && ram[address] == '$')
            break;
        boot_trace_hex(ram + address, 1);
    }
    if ((dollar_terminated && i == requested) || (i < requested && i == 4096))
        truncated = 1;
    fprintf(boot_trace, "\",\"truncated\":%s}\n", truncated ? "true" : "false");
    fflush(boot_trace);
}

static void
boot_trace_frame(const uint8_t *vram, const uint8_t *crtc,
                 uint8_t mode, int monochrome)
{
    if (!terminal_boot_trace_enabled())
        return;
    const unsigned aperture = monochrome ? 4096 : 16384;
    const unsigned start = (crtc[12] << 8) | crtc[13];
    const unsigned cursor = (((crtc[14] << 8) | crtc[15]) - start) & (aperture / 2 - 1);
    boot_trace_prefix("frame");
    fprintf(boot_trace, ",\"mono\":%d,\"mode\":%u,\"columns\":%u,\"rows\":%u,"
            "\"cursor\":%u,\"crtc_hex\":\"", !!monochrome, mode, crtc[1], crtc[6], cursor);
    boot_trace_hex(crtc, 18);
    fputs("\",\"vram_hex\":\"", boot_trace);
    boot_trace_hex(vram, aperture);
    fputs("\"}\n", boot_trace);
    fflush(boot_trace);
}

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
    if (!terminal_presenter &&
        (event->kind == TIGT_PRESS || event->kind == TIGT_REPEAT) &&
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
    if (terminal_presenter && terminal_input_fullscreen && terminal_input_tty &&
        event->kind == TIGT_PRESS &&
        (event->modifiers & TIGT_MOD_CONTROL) &&
        event->key.kind == TIGT_KEY_CHAR && event->key.character == ']') {
        kill(getpid(), SIGINT);
        return;
    }
    if (terminal_presenter && event->kind != TIGT_RELEASE)
        terminal_input_events++;
    pc_xt_keyboard_v1_key_event keys[PC_XT_KEYBOARD_V1_EVENT_MAX_KEYS];
    const size_t count = tigt_keyboard_handle(terminal_keyboard, event, keys,
                                             PC_XT_KEYBOARD_V1_EVENT_MAX_KEYS);
    if (count == PC_XT_KEYBOARD_V1_ERROR)
        return;
    for (size_t index = 0; index < count; index++)
        terminal_key(keys[index].down, keys[index].key);
}

static uint64_t
terminal_input_clock(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t) now.tv_sec * 1000000000ULL + now.tv_nsec;
}

static void
terminal_input_sequence(const char *sequence)
{
    size_t remaining = strlen(sequence);
    while (remaining) {
        const ssize_t count = write(STDOUT_FILENO, sequence, remaining);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        sequence += count;
        remaining -= (size_t) count;
    }
}

static void
terminal_stdin_restore(void)
{
    terminal_cursor_query = TERMINAL_CURSOR_IDLE;
    terminal_cursor_mode = false;
    terminal_cursor_length = 0;
    if (terminal_input_protocol) {
        terminal_input_sequence("\033[<u");
        terminal_input_protocol = false;
    }
    if (terminal_input_saved && terminal_input_tty)
        while (tcsetattr(STDIN_FILENO, TCSANOW, &terminal_saved_input) < 0 &&
               errno == EINTR) {}
    terminal_input_active = false;
}

static void
terminal_stdin_release(void)
{
    if (terminal_keyboard) {
        keyboard_all_up();
        pc_xt_keyboard_v1_destroy(terminal_keyboard);
    }
    terminal_keyboard = NULL;
    tigt_input_destroy(terminal_stdin_decoder);
    terminal_stdin_decoder = NULL;
}

static void
terminal_stdin_create(void)
{
    terminal_keyboard = terminal_keyboard_create();
    terminal_stdin_decoder = tigt_input_create(terminal_input, NULL);
    if (!terminal_keyboard || !terminal_stdin_decoder)
        fatal("Terminal: could not create stdin keyboard decoder\n");
}

static void
terminal_stdin_mode(bool fullscreen)
{
    if (!terminal_input_saved || terminal_input_eof)
        return;
    if (terminal_input_active && terminal_input_fullscreen == fullscreen && !terminal_cursor_mode)
        return;
    if (terminal_input_active) {
        /* Explicit protocol releases need not arrive across a mode boundary. */
        terminal_stdin_release();
        terminal_stdin_create();
    }
    if (terminal_input_protocol) {
        terminal_input_sequence("\033[<u");
        terminal_input_protocol = false;
    }
    if (terminal_input_tty) {
        struct termios mode = terminal_saved_input;
        if (fullscreen) {
            mode.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
            mode.c_cflag = (mode.c_cflag & ~(CSIZE | PARENB)) | CS8;
            mode.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
            mode.c_cc[VMIN] = 1;
            mode.c_cc[VTIME] = 0;
        } else {
            mode.c_iflag |= ICRNL;
            mode.c_iflag &= ~(IGNCR | INLCR);
            mode.c_lflag |= ICANON | ECHO | ECHOE | ECHOK | ISIG | IEXTEN;
        }
        /* Preserve output processing: stdin and stdout often share this TTY,
           and the presenter writes LF expecting the shell's ONLCR behavior. */
        int result;
        do {
            result = tcsetattr(STDIN_FILENO, TCSANOW, &mode);
        } while (result < 0 && errno == EINTR);
        if (result < 0)
            fatal("Terminal: could not change stdin mode: %s\n", strerror(errno));
    }
    /* Never change file-status flags: stdin/stdout can be dup()s of one open
       description, so O_NONBLOCK here would also make presenter writes fail
       under output backpressure. This thread is the sole reader; poll before
       each bounded read, with VMIN=1/VTIME=0 in raw mode, avoids waiting. */
    terminal_input_fullscreen = fullscreen;
    terminal_input_active = true;
    terminal_cursor_mode = false;
    terminal_cursor_query = TERMINAL_CURSOR_IDLE;
    if (!fullscreen)
        tigt_presenter_forget_cursor(terminal_presenter);
    terminal_input_deadline = terminal_input_last_byte = 0;
    if (fullscreen && terminal_input_tty && isatty(STDOUT_FILENO)) {
        terminal_input_sequence("\033[>u\033[=11;1u");
        terminal_input_protocol = true;
    }
}

/* The kernel has already edited and echoed these bytes. Register the complete
   read before any of its keys can reach DOS, not one paced key at a time. A
   forced raw cutover also finalizes the previously unread canonical prefix. */
static void
terminal_stdin_local_echo(const uint8_t *bytes, size_t length)
{
    if (!terminal_input_echo_tty || !terminal_input_baseline || !length)
        return;
    uint32_t text[TIGT_PRESENTER_LOCAL_ECHO_MAX];
    size_t used = 0;
    for (size_t i = 0; i < length;) {
        uint32_t scalar = bytes[i++];
        unsigned continuation = 0;
        uint32_t minimum = 0;
        if (scalar >= 0xc2 && scalar <= 0xdf) {
            scalar &= 0x1f; continuation = 1; minimum = 0x80;
        } else if (scalar >= 0xe0 && scalar <= 0xef) {
            scalar &= 0x0f; continuation = 2; minimum = 0x800;
        } else if (scalar >= 0xf0 && scalar <= 0xf4) {
            scalar &= 7; continuation = 3; minimum = 0x10000;
        } else if (scalar >= 0x80)
            return;
        if (continuation > length - i)
            return;
        while (continuation--) {
            if ((bytes[i] & 0xc0) != 0x80)
                return;
            scalar = (scalar << 6) | (bytes[i++] & 0x3f);
        }
        if (scalar < minimum || scalar > 0x10ffff ||
            (scalar >= 0xd800 && scalar <= 0xdfff))
            return;
        if (used == TIGT_PRESENTER_LOCAL_ECHO_MAX)
            fatal("Terminal: cooked input exceeds local echo capacity\n");
        text[used++] = scalar;
    }
    const int result = tigt_presenter_local_echo(terminal_presenter, text, used);
    /* Wide/control characters do not describe a single-cell echo trajectory.
       Keep their original input bytes rather than inventing display scalars. */
    if (result != TIGT_OK && result != TIGT_ERROR_ARGUMENT &&
        result != TIGT_ERROR_UNREPRESENTABLE)
        fatal("Terminal: could not account for cooked echo (%d)\n", result);
}

static bool
terminal_stdin_readable(void)
{
    struct pollfd input = { .fd = STDIN_FILENO, .events = POLLIN };
    return poll(&input, 1, 0) > 0 &&
           (input.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL));
}

static void
terminal_stdin_read(bool cooked)
{
    size_t capacity = sizeof(terminal_input_bytes) - terminal_input_length;
    if (capacity > TIGT_PRESENTER_LOCAL_ECHO_MAX)
        capacity = TIGT_PRESENTER_LOCAL_ECHO_MAX;
    if (!capacity || !terminal_stdin_readable())
        return;
    uint8_t *bytes = terminal_input_bytes + terminal_input_length;
    const ssize_t count = read(STDIN_FILENO, bytes, capacity);
    if (count > 0) {
        if (cooked)
            terminal_stdin_local_echo(bytes, (size_t) count);
        terminal_input_length += (size_t) count;
        if (cooked)
            terminal_input_cooked = terminal_input_length;
    } else if (count == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK))
        terminal_input_eof = true;
}

static void
terminal_cursor_request(void)
{
    if (terminal_cursor_query != TERMINAL_CURSOR_IDLE)
        return;
    if (!terminal_input_active || terminal_input_eof || !terminal_input_echo_tty)
        fatal("Terminal: adaptive output needs a cursor reply on the output TTY\n");
    /* Make room without discarding a paced line. Never change O_NONBLOCK:
       stdout may share stdin's open file description. */
    if (terminal_input_offset) {
        memmove(terminal_input_bytes, terminal_input_bytes + terminal_input_offset,
                terminal_input_length - terminal_input_offset);
        terminal_input_cooked = terminal_input_cooked > terminal_input_offset ?
                                terminal_input_cooked - terminal_input_offset : 0;
        terminal_input_length -= terminal_input_offset;
        terminal_input_offset = 0;
    }
    /* Drain complete canonical lines first, reserving one kernel line plus
       room for the CPR and keys typed while it is in flight. */
    while (!terminal_input_fullscreen && !terminal_input_eof && terminal_stdin_readable()) {
        if (sizeof(terminal_input_bytes) - terminal_input_length <
            TIGT_PRESENTER_LOCAL_ECHO_MAX + 256)
            return;
        terminal_stdin_read(true);
    }
    if (terminal_input_eof) {
        terminal_stdin_restore();
        return;
    }
    if (sizeof(terminal_input_bytes) - terminal_input_length <
        TIGT_PRESENTER_LOCAL_ECHO_MAX + 256)
        return; /* Retry at a later vsync after the guest drains the line. */
    struct termios mode;
    if (tcgetattr(STDIN_FILENO, &mode) < 0)
        fatal("Terminal: could not prepare cursor query: %s\n", strerror(errno));
    /* ISIG and the shell's output processing remain in effect during DSR.
       TCSANOW, never TCSAFLUSH: changing ICANON exposes its unfinished line. */
    mode.c_lflag &= ~(ICANON | ECHO | ECHONL);
    mode.c_cc[VMIN] = 1;
    mode.c_cc[VTIME] = 0;
    int result;
    do {
        result = tcsetattr(STDIN_FILENO, TCSANOW, &mode);
    } while (result < 0 && errno == EINTR);
    if (result < 0)
        fatal("Terminal: could not query cursor: %s\n", strerror(errno));
    terminal_cursor_mode = true;
    /* Everything queued before the query belongs to the previous input mode,
       including a host-edited partial line that canonical poll could not see. */
    while (!terminal_input_eof && terminal_stdin_readable()) {
        if (sizeof(terminal_input_bytes) - terminal_input_length < 256) {
            terminal_stdin_restore();
            fatal("Terminal: pending input exceeds cursor query capacity\n");
            return;
        }
        terminal_stdin_read(!terminal_input_fullscreen);
    }
    if (terminal_input_eof) {
        terminal_stdin_restore();
        return;
    }
    terminal_cursor_length = 0;
    terminal_cursor_query = TERMINAL_CURSOR_WAITING;
    terminal_cursor_deadline = terminal_input_clock() + 1000000000ULL;
    terminal_input_sequence("\033[6n");
}

/* A possible CPR is held outside the keyboard decoder until its final byte.
   Non-CPR sequences are returned unchanged, in order, to the normal decoder. */
static void
terminal_cursor_byte(uint8_t byte)
{
    if (!terminal_cursor_length && byte != '\033') {
        terminal_input_bytes[terminal_input_length++] = byte;
        return;
    }
    terminal_cursor_reply[terminal_cursor_length++] = byte;
    const uint8_t *reply = terminal_cursor_reply;
    size_t i = 1;
    unsigned row = 0, column = 0;
    if (i == terminal_cursor_length)
        return;
    if (reply[i++] != '[')
        goto keyboard;
    const size_t row_start = i;
    while (i < terminal_cursor_length && reply[i] >= '0' && reply[i] <= '9') {
        row = row * 10 + reply[i++] - '0';
        if (row > 65535)
            goto keyboard;
    }
    if (i == terminal_cursor_length) {
        if (i < sizeof(terminal_cursor_reply))
            return;
        goto keyboard;
    }
    if (i == row_start || !row || reply[i++] != ';')
        goto keyboard;
    const size_t column_start = i;
    while (i < terminal_cursor_length && reply[i] >= '0' && reply[i] <= '9') {
        column = column * 10 + reply[i++] - '0';
        if (column > 65535)
            goto keyboard;
    }
    if (i == terminal_cursor_length) {
        if (i < sizeof(terminal_cursor_reply))
            return;
        goto keyboard;
    }
    if (i == column_start || !column || reply[i] != 'R' ||
        i + 1 != terminal_cursor_length)
        goto keyboard;
    if (tigt_presenter_observe_cursor(terminal_presenter, column, row) != TIGT_OK)
        fatal("Terminal: invalid host cursor observation\n");
    terminal_cursor_length = 0;
    terminal_cursor_query = TERMINAL_CURSOR_READY;
    return;

keyboard:
    ;
    /* A new ESC can start the response immediately after an ordinary Escape
       key or an incomplete keyboard sequence. Do not swallow either one. */
    const bool restart = byte == '\033';
    const size_t count = terminal_cursor_length - (restart ? 1 : 0);
    memcpy(terminal_input_bytes + terminal_input_length, reply, count);
    terminal_input_length += count;
    terminal_cursor_length = restart ? 1 : 0;
    if (restart)
        terminal_cursor_reply[0] = '\033';
}

static void
terminal_cursor_poll(void)
{
    if (terminal_input_clock() >= terminal_cursor_deadline) {
        terminal_stdin_restore();
        fatal("Terminal: host cursor query timed out\n");
        return;
    }
    /* Reserve room to replay an incomplete CPR as genuine keyboard input.
       This also bounds input while paused without making the shared fd raw. */
    size_t capacity = sizeof(terminal_input_bytes) - terminal_input_length -
                      terminal_cursor_length;
    if (!capacity || !terminal_stdin_readable())
        return;
    uint8_t bytes[256];
    if (capacity > sizeof(bytes))
        capacity = sizeof(bytes);
    const ssize_t count = read(STDIN_FILENO, bytes, capacity);
    if (count > 0) {
        for (ssize_t i = 0; i < count; i++) {
            if (terminal_cursor_query == TERMINAL_CURSOR_WAITING)
                terminal_cursor_byte(bytes[i]);
            else
                terminal_input_bytes[terminal_input_length++] = bytes[i];
        }
    } else if (count == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
        terminal_stdin_restore();
        fatal("Terminal: input ended while awaiting host cursor\n");
    }
}

void
terminal_renderer_poll_input(void)
{
    if (!terminal_presenter || terminal_suspended || !terminal_input_active)
        return;
    if (terminal_cursor_query == TERMINAL_CURSOR_WAITING)
        terminal_cursor_poll();
    if (terminal_cursor_query != TERMINAL_CURSOR_IDLE)
        return; /* Resubmit the video frame before delivering collected keys. */
    if (terminal_input_offset == terminal_input_length && !terminal_input_eof) {
        terminal_input_offset = terminal_input_length = terminal_input_cooked = 0;
        terminal_stdin_read(!terminal_input_fullscreen && terminal_input_tty);
    }
    /* Reading remains live while paused, but never fill the guest BIOS queue
       while the guest cannot drain it. The fixed buffer backpressures pipes. */
    if (dopause || (!terminal_input_baseline && terminal_input_echo_tty &&
                    terminal_input_offset < terminal_input_cooked))
        return;
    const uint64_t now = terminal_input_clock();
    if (now < terminal_input_deadline)
        return;
    const unsigned before = terminal_input_events;
    while (terminal_input_offset < terminal_input_length) {
        const bool paced = terminal_input_offset < terminal_input_cooked ||
                           !terminal_input_fullscreen || !terminal_input_tty;
        tigt_input_feed(terminal_stdin_decoder,
                        terminal_input_bytes + terminal_input_offset++, 1);
        terminal_input_last_byte = now;
        if (paced && terminal_input_events != before) {
            /* Fifty keys/second leaves XT IRQ1 and the DOS line reader time
               to consume each key instead of overflowing their small queues. */
            terminal_input_deadline = now + 20000000ULL;
            return;
        }
    }
    if (terminal_input_eof ||
        (terminal_input_last_byte && now - terminal_input_last_byte >= 30000000ULL)) {
        tigt_input_flush(terminal_stdin_decoder);
        terminal_input_last_byte = 0;
    }
    if (terminal_input_eof) {
        terminal_stdin_restore();
        terminal_stdin_release();
    }
}

void
terminal_renderer_init(void)
{
    video_setblit(terminal_blit);
    if (terminal_initialized)
        return;
    terminal_video_memory.aperture = 0;
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
        terminal_input_tty = isatty(STDIN_FILENO);
        struct stat input_stat, output_stat;
        terminal_input_echo_tty = terminal_input_tty && isatty(STDOUT_FILENO) &&
                                  fstat(STDIN_FILENO, &input_stat) == 0 &&
                                  fstat(STDOUT_FILENO, &output_stat) == 0 &&
                                  input_stat.st_rdev == output_stat.st_rdev;
        terminal_input_saved = fcntl(STDIN_FILENO, F_GETFL) >= 0;
        terminal_input_eof = false;
        terminal_input_baseline = false;
        terminal_input_offset = terminal_input_length = terminal_input_cooked = 0;
        terminal_cursor_query = TERMINAL_CURSOR_IDLE;
        terminal_cursor_mode = false;
        if (terminal_input_saved && terminal_input_tty &&
            tcgetattr(STDIN_FILENO, &terminal_saved_input) < 0) {
            terminal_input_saved = false;
            fatal("Terminal: could not save stdin mode: %s\n", strerror(errno));
        }
        if (terminal_input_saved) {
            terminal_stdin_create();
            terminal_stdin_mode(false);
        }
        return;
    }
    if (!isatty(STDOUT_FILENO))
        return;
    terminal_keyboard = terminal_keyboard_create();
    if (terminal_keyboard == NULL)
        fatal("Terminal: could not create keyboard mapper\n");
    uint32_t graphics_mode = TIGT_GRAPHICS_AUTO;
    const char *graphics = getenv("TIGT_GRAPHICS");
    if (graphics && *graphics) {
        if (!strcmp(graphics, "ascii"))
            graphics_mode = TIGT_GRAPHICS_ASCII;
        else if (!strcmp(graphics, "sixel"))
            graphics_mode = TIGT_GRAPHICS_SIXEL;
        else if (!strcmp(graphics, "iterm2"))
            graphics_mode = TIGT_GRAPHICS_ITERM2;
        else if (!strcmp(graphics, "blocks"))
            graphics_mode = TIGT_GRAPHICS_BLOCKS;
        else if (strcmp(graphics, "auto"))
            fatal("Terminal: invalid TIGT_GRAPHICS\n");
    }
    const tigt_config config = { TIGT_ABI_VERSION, terminal_input, NULL, graphics_mode };
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
        terminal_stdin_restore();
        terminal_stdin_release();
        tigt_presenter_forget_cursor(terminal_presenter);
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
        if (terminal_input_saved && !terminal_input_eof) {
            terminal_stdin_create();
            terminal_stdin_mode(terminal_input_fullscreen);
        }
        terminal_suspended = false;
        tigt_presenter_forget_cursor(terminal_presenter);
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
        terminal_stdin_restore();
        terminal_stdin_release();
        terminal_input_saved = false;
        tigt_presenter_destroy(terminal_presenter);
        terminal_video_memory.aperture = 0;
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
    boot_trace_frame(vram, crtc, mode, monochrome);
    if (!terminal_presenter || terminal_suspended || !crtc[1] || !crtc[6])
        return;
    if (!monochrome && (mode & 2))
        return; /* This output-only integration handles text modes. */
    if (terminal_cursor_query == TERMINAL_CURSOR_WAITING)
        return; /* No submission: retain the last raw-memory comparison baseline. */
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
    /* Preserve underlying text for scroll-copy recognition even when hardware
       output is disabled. The presenter applies the explicit disable hint. */
    tigt_video_write(*decoder, port + 4, mode | 8);
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
    const unsigned memory_kind = monochrome ? 1 : (pcjr_array ? 2 : 0);
    const bool memory_changed =
        terminal_video_memory.aperture != aperture ||
        terminal_video_memory.machine != machine ||
        terminal_video_memory.monitor != monitor_index_global ||
        terminal_video_memory.kind != memory_kind ||
        memcmp(terminal_video_memory.bytes, vram, aperture) != 0;
    const uint32_t hints = ((mode & 8) ? 0 : TIGT_PRESENT_VIDEO_DISABLED) |
                          (memory_changed ? TIGT_PRESENT_VIDEO_MEMORY_CHANGED : 0);
    const tigt_presenter_frame output = {
        storage->cells, decoded.width, decoded.height, decoded.width,
        position % decoded.width, position / decoded.width, monochrome ? 50 : 60, hints
    };
    result = tigt_presenter_present(terminal_presenter, &output);
    if (result < 0)
        fatal("Terminal: output presentation aborted (%d)\n", result);
    if (memory_changed) {
        memcpy(terminal_video_memory.bytes, vram, aperture);
        terminal_video_memory.aperture = aperture;
        terminal_video_memory.machine = machine;
        terminal_video_memory.monitor = monitor_index_global;
        terminal_video_memory.kind = memory_kind;
    }
    if (result == TIGT_PRESENTER_NEEDS_CURSOR) {
        terminal_cursor_request();
        return;
    }
    if (result == TIGT_OK || result == TIGT_PRESENTER_FULLSCREEN) {
        if (!terminal_input_baseline) {
            terminal_input_baseline = true;
            if (result == TIGT_OK && terminal_input_cooked > terminal_input_offset)
                terminal_stdin_local_echo(terminal_input_bytes + terminal_input_offset,
                                          terminal_input_cooked - terminal_input_offset);
        }
        terminal_stdin_mode(result == TIGT_PRESENTER_FULLSCREEN);
    }
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
