/* Exercise canonical/raw input and restoration using the real renderer and PTY. */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include "../../src/terminal/terminal_renderer.c"

monitor_t monitors[MONITORS_NUM];
int monitor_index_global;
int machine;
int dopause;
int keyboard_type = KEYBOARD_TYPE_PC_XT;
static uint8_t held[65536];
static uint16_t presses[64];
static size_t press_count;
static int expect_cursor_timeout;

void fatal(const char *fmt, ...)
{
    if (expect_cursor_timeout)
        _exit(75);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    abort();
}
int machine_has_bus(int m, uintptr_t flags) { (void) m; (void) flags; return 0; }
void video_setblit(void (*blit)(int, int, int, int, int)) { (void) blit; }
void video_blit_complete_monitor(int m) { (void) m; }
void keyboard_input(int down, uint16_t scan)
{
    held[scan] = !!down;
    if (down) {
        assert(press_count < sizeof(presses) / sizeof(*presses));
        presses[press_count++] = scan;
    }
}
int keyboard_recv_ui(uint16_t scan) { return held[scan]; }
void keyboard_all_up(void) { memset(held, 0, sizeof(held)); }

static int master, slave, saved_stdin, saved_stdout, original_flags;
static struct termios original;
static uint8_t vram[16384], crtc[18];
static char output_bytes[131072];
static size_t output_length;
static unsigned cursor_requests;
static int answer_cursor = 1;

static void no_keys_held(void)
{
    for (size_t i = 0; i < sizeof(held); i++)
        assert(!held[i]);
}

static void drain_output(void)
{
    char bytes[4096];
    ssize_t count;
    while ((count = read(master, bytes, sizeof(bytes))) > 0) {
        assert(output_length + (size_t) count < sizeof(output_bytes));
        const size_t start = output_length > 3 ? output_length - 3 : 0;
        memcpy(output_bytes + output_length, bytes, (size_t) count);
        output_length += (size_t) count;
        output_bytes[output_length] = 0;
        const char *query = output_bytes + start;
        while ((query = strstr(query, "\033[6n"))) {
            cursor_requests++;
            if (answer_cursor)
                assert(write(master, "\033[3;2R", 6) == 6);
            query += 4;
        }
    }
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
}

static void pump(unsigned milliseconds)
{
    struct timespec now, end, pause = { .tv_nsec = 1000000 };
    assert(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
    end.tv_nsec += (milliseconds % 1000) * 1000000L;
    end.tv_sec += milliseconds / 1000 + end.tv_nsec / 1000000000L;
    end.tv_nsec %= 1000000000L;
    do {
        assert(terminal_renderer_poll_input() == TIGT_OK);
        drain_output();
        nanosleep(&pause, NULL);
        assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    } while (now.tv_sec < end.tv_sec ||
             (now.tv_sec == end.tv_sec && now.tv_nsec < end.tv_nsec));
}

static void await_presses(size_t count)
{
    for (unsigned i = 0; i < 200 && press_count < count; i++)
        pump(5);
    assert(press_count == count);
}

static void send_bytes(const char *bytes, size_t count)
{
    assert(write(master, bytes, count) == (ssize_t) count);
}

static void check_fallback_marker(const char *p)
{
    unsigned row = 4, column = 3; /* The position returned in the CPR. */
    int found = 0;
    while (*p) {
        if (p[0] == '\033' && p[1] == '[') {
            p += 2;
            unsigned params[32], count = 0;
            while (*p && !(*p >= '@' && *p <= '~')) {
                if (*p >= '0' && *p <= '9') {
                    char *end;
                    assert(count < 32);
                    params[count++] = strtoul(p, &end, 10);
                    p = end;
                } else p++;
            }
            if (*p == 'H' || *p == 'f') {
                row = count && params[0] ? params[0] : 1;
                column = count > 1 && params[1] ? params[1] : 1;
            } else if (*p == 'G')
                column = count && params[0] ? params[0] : 1;
            else if (*p == 'd')
                row = count && params[0] ? params[0] : 1;
            if (*p) p++;
        } else {
            const char byte = *p++;
            if (byte == 'X') {
                assert(row == 5 && column == 1);
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

static void check_mode(int canonical)
{
    struct termios actual;
    assert(tcgetattr(slave, &actual) == 0);
    const tcflag_t mask = ICANON | ECHO | ISIG;
    assert((actual.c_lflag & mask) == (canonical ? mask : 0));
}

static void check_restored(void)
{
    struct termios actual;
    assert(tcgetattr(slave, &actual) == 0);
    assert(actual.c_iflag == original.c_iflag);
    assert(actual.c_oflag == original.c_oflag);
    assert(actual.c_cflag == original.c_cflag);
    /* BSD sets PENDIN when canonical input is restored. It is kernel state
       requesting reprocessing of pending input, not a shell mode setting. */
    tcflag_t lflag_mask = (tcflag_t) -1;
#ifdef PENDIN
    lflag_mask &= ~PENDIN;
#endif
    assert((actual.c_lflag & lflag_mask) == (original.c_lflag & lflag_mask));
    assert(memcmp(actual.c_cc, original.c_cc, sizeof(original.c_cc)) == 0);
    assert(cfgetispeed(&actual) == cfgetispeed(&original));
    assert(cfgetospeed(&actual) == cfgetospeed(&original));
    /* F_GETFL may also report kernel history (Darwin FWASWRITTEN after the
       presenter writes through the shared description), not mutable modes. */
    const int flags = fcntl(STDIN_FILENO, F_GETFL);
    int flag_mask = O_ACCMODE | O_NONBLOCK | O_APPEND | O_SYNC;
#ifdef O_ASYNC
    flag_mask |= O_ASYNC;
#endif
#ifdef O_DSYNC
    flag_mask |= O_DSYNC;
#endif
    assert(flags >= 0 && (flags & flag_mask) == (original_flags & flag_mask));
}

static void setup(void)
{
    master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    assert(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0);
    slave = open(ptsname(master), O_RDWR | O_NOCTTY);
    assert(slave >= 0);
    assert(ioctl(slave, TIOCSCTTY, 0) == 0);
    assert(tcsetpgrp(slave, getpgrp()) == 0);
    struct winsize size = { .ws_col = 10, .ws_row = 6 };
    assert(ioctl(slave, TIOCSWINSZ, &size) == 0);
    assert(tcgetattr(slave, &original) == 0);
    original.c_lflag |= ICANON | ECHO | ECHOE | ISIG;
    original.c_iflag |= ICRNL | IXON;
    original.c_lflag |= IEXTEN;
    original.c_cc[VINTR] = 0x03;
    original.c_cc[VQUIT] = 0x1c;
    original.c_cc[VSUSP] = 0x1a;
    original.c_cc[VLNEXT] = 0x16;
    original.c_cc[VERASE] = 0x7f;
    original.c_cc[VEOF] = 0x04;
    assert(tcsetattr(slave, TCSANOW, &original) == 0);
    assert(tcgetattr(slave, &original) == 0);
    saved_stdin = dup(STDIN_FILENO);
    saved_stdout = dup(STDOUT_FILENO);
    assert(saved_stdin >= 0 && saved_stdout >= 0);
    assert(dup2(slave, STDIN_FILENO) == STDIN_FILENO);
    assert(dup2(slave, STDOUT_FILENO) == STDOUT_FILENO);
    original_flags = fcntl(STDIN_FILENO, F_GETFL);
    assert(original_flags >= 0);
    assert(setenv("TERM", "xterm-256color", 1) == 0);
    assert(unsetenv("86BOX_BOOT_TRACE") == 0);
}

static void teardown(void)
{
    /* Closing the last controlling PTY is teardown, not a lifecycle scenario. */
    assert(signal(SIGHUP, SIG_IGN) != SIG_ERR);
    assert(dup2(saved_stdin, STDIN_FILENO) == STDIN_FILENO);
    assert(dup2(saved_stdout, STDOUT_FILENO) == STDOUT_FILENO);
    close(saved_stdin); close(saved_stdout); close(slave); close(master);
}

static void snapshot(void)
{
    terminal_video_snapshot(vram, crtc, 8, 0, NULL);
    drain_output();
    assert(terminal_renderer_poll_input() == TIGT_OK);
}

static void blank_frame(void)
{
    memset(vram, 0, sizeof(vram));
    for (unsigned i = 0; i < sizeof(vram) / 2; i++) {
        vram[i * 2] = ' ';
        vram[i * 2 + 1] = 7;
    }
    memset(crtc, 0, sizeof(crtc));
    crtc[1] = 4;
    crtc[6] = 2;
}

static void enter_fullscreen(void)
{
    blank_frame();
    snapshot();
    vram[0] = 'A'; vram[8] = 'B'; crtc[15] = 5;
    snapshot();
    check_mode(1);
    vram[0] = 'X';
    snapshot();
    check_mode(1); /* An unconfirmed frame must not switch input to raw. */
    for (unsigned i = 0; i < 10; i++)
        snapshot();
    check_mode(0);
}

static void disabled_snapshot(unsigned mode, int mono, const uint8_t *array)
{
    terminal_video_snapshot(vram, crtc, mode, mono, array);
    drain_output();
}

static void video_disable(int mono, int pcjr)
{
    uint8_t array[32] = { 0 };
    uint32_t palette[256] = { 0 };
    array[1] = 15;
    for (unsigned i = 0; i < 16; i++) {
        array[16 + i] = i;
        palette[16 + i] = i * 0x111111;
    }
    monitors[0].mon_pal_lookup = palette;
    const uint8_t *registers = pcjr ? array : NULL;
    const unsigned frames = mono ? 10 : 12;
    const unsigned aperture = mono ? 4096 : 16384;
    assert(setenv("TIGT_PRESENTATION", "glass", 1) == 0);
    terminal_renderer_init();
    blank_frame();
    vram[0] = 'A'; vram[8] = 'B'; crtc[15] = 5;
    disabled_snapshot(8, mono, registers);
    size_t before = output_length;
    for (unsigned i = 1; i < frames; i++) {
        disabled_snapshot(0, mono, registers);
        assert(output_length == before);
        check_mode(1);
    }
    disabled_snapshot(0, mono, registers);
    assert(strchr(output_bytes + before, '\f'));
    check_mode(1);

    /* Non-scroll changes cancel independently: visible character/attribute
       and hidden character/attribute bytes must all produce a real blank. */
    const unsigned changes[] = { 0, 1, aperture - 2, aperture - 1 };
    for (unsigned i = 0; i < sizeof(changes) / sizeof(*changes); i++) {
        disabled_snapshot(8, mono, registers);
        before = output_length;
        if (i != 0) {
            disabled_snapshot(0, mono, registers);
            assert(output_length == before);
        }
        vram[changes[i]] ^= 1;
        disabled_snapshot(0, mono, registers);
        assert(strchr(output_bytes + before, '\f'));
        before = output_length;
        disabled_snapshot(0, mono, registers);
        assert(output_length == before);
    }

    /* A skipped snapshot during DSR cannot consume a hidden-memory change.
       Model the wait here without replacing the presenter or output sink. */
    disabled_snapshot(8, mono, registers);
    before = output_length;
    disabled_snapshot(0, mono, registers);
    terminal_cursor_query = TERMINAL_CURSOR_WAITING;
    vram[aperture - 1] ^= 1;
    disabled_snapshot(0, mono, registers);
    assert(output_length == before);
    terminal_cursor_query = TERMINAL_CURSOR_IDLE;
    disabled_snapshot(0, mono, registers);
    assert(strchr(output_bytes + before, '\f'));

    /* Reenable cancels the partial interval without clearing or replaying the
       retained image. The next disable receives a fresh full interval. */
    disabled_snapshot(8, mono, registers);
    before = output_length;
    disabled_snapshot(0, mono, registers);
    disabled_snapshot(8, mono, registers);
    assert(output_length == before);
    for (unsigned i = 1; i < frames; i++)
        disabled_snapshot(0, mono, registers);
    assert(output_length == before);
    disabled_snapshot(0, mono, registers);
    assert(strchr(output_bytes + before, '\f'));

    /* Switching aperture geometry with an identical visible image is still a
       changed snapshot, not another frame of the previous disable interval. */
    disabled_snapshot(8, mono, registers);
    before = output_length;
    disabled_snapshot(0, mono, registers);
    assert(output_length == before);
    disabled_snapshot(0, !mono, NULL);
    assert(strchr(output_bytes + before, '\f'));

    /* A real CLS is enabled video, not a hardware-disabled interval. */
    disabled_snapshot(8, mono, registers);
    before = output_length;
    blank_frame();
    disabled_snapshot(8, mono, registers);
    assert(strchr(output_bytes + before, '\f'));

    /* Preserve real underlying cells while video is disabled: a slow VRAM
       scroll spans callbacks and must not paint the intermediate duplicate. */
    assert(tigt_presenter_reset(terminal_presenter) == TIGT_OK);
    blank_frame();
    crtc[6] = 4; crtc[15] = 13;
    vram[0] = 'A'; vram[8] = 'B'; vram[16] = 'C'; vram[24] = 'D';
    before = output_length;
    disabled_snapshot(8, mono, registers);
    assert(strcmp(output_bytes + before, "A\r\nB\r\nC\r\nD") == 0);
    before = output_length;
    vram[0] = 'B'; vram[8] = 'C';
    for (unsigned i = 0; i < 8; i++) {
        disabled_snapshot(0, mono, registers);
        assert(output_length == before);
        check_mode(1);
    }
    vram[16] = 'D'; vram[24] = 'E';
    disabled_snapshot(0, mono, registers);
    assert(output_length == before); /* Coherent raw copy is not enabled output. */
    disabled_snapshot(8, mono, registers);
    assert(strcmp(output_bytes + before, "\r\nE") == 0);
    before = output_length;
    vram[0] = 'Z';
    disabled_snapshot(0, mono, registers);
    assert(strcmp(output_bytes + before, "\f") == 0);
    terminal_renderer_close();
    check_restored();

    /* Reinitialization must not carry a previous raw snapshot into a new
       session: the first disabled frame establishes a usable echo baseline. */
    terminal_renderer_init();
    disabled_snapshot(0, mono, registers);
    const uint32_t echo[] = { 'q' };
    assert(tigt_presenter_local_echo(terminal_presenter, echo, 1) == TIGT_OK);
    terminal_renderer_close();
    check_restored();
}

static void hold_raw_key(void)
{
    const size_t before = press_count;
    const char key[] = "\033[97;1:1u"; /* Kitty press, deliberately no release/newline. */
    send_bytes(key, sizeof(key) - 1);
    await_presses(before + 1);
    assert(presses[before] == 0x1e && held[0x1e]);
}

static void video_disable_raw(void)
{
    terminal_renderer_init();
    enter_fullscreen();
    hold_raw_key();
    const size_t before = output_length;
    for (unsigned i = 1; i < 12; i++) {
        disabled_snapshot(0, 0, NULL);
        assert(output_length == before);
        check_mode(0);
        assert(held[0x1e]);
    }
    disabled_snapshot(0, 0, NULL);
    assert(strchr(output_bytes + before, ' ')); /* The old region was erased. */
    check_mode(0);
    assert(held[0x1e]);
    terminal_renderer_close();
    check_restored();
    no_keys_held();
}

static void cooked_line(void)
{
    const size_t before = press_count;
    blank_frame();
    crtc[1] = 8; crtc[6] = 4;
    snapshot();
    vram[0] = '>'; crtc[15] = 1;
    snapshot();
    output_length = 0;
    output_bytes[0] = 0;
    const char editing[] = { 'a', 'b', 0x7f, 'c' };
    send_bytes(editing, sizeof(editing));
    pump(40);
    assert(press_count == before);
    send_bytes("\n", 1);
    await_presses(before + 3);
    assert(presses[before] == 0x1e);
    assert(presses[before + 1] == 0x2e);
    assert(presses[before + 2] == 0x1c);
    no_keys_held();
    assert(strchr(output_bytes, 'a') && strchr(output_bytes, 'c'));
    assert(strchr(output_bytes, '\n')); /* Real host ECHO, not an input mock. */
    const size_t echoed = output_length;
    /* DOS echoes incrementally, after the host has already displayed the
       entire edited line. Neither glyphs nor the final LF may be replayed. */
    vram[2] = 'a'; crtc[15] = 2;
    snapshot();
    vram[4] = 'c'; crtc[15] = 3;
    snapshot();
    crtc[15] = 8;
    snapshot();
    assert(!strchr(output_bytes + echoed, 'a'));
    assert(!strchr(output_bytes + echoed, 'c'));
    assert(!strchr(output_bytes + echoed, '\n'));
}

static void cursor_handshake(void)
{
    terminal_renderer_init();
    blank_frame();
    vram[0] = 'A'; vram[8] = 'B'; crtc[15] = 5;
    snapshot();
    check_mode(1);
    const size_t before = press_count;
    /* The quoted Ctrl-C must retain its kernel-processed provenance when the
       unfinished canonical line is exposed by DSR and later decoded raw. */
    const char editing[] = { 'a', 'b', 0x7f, 'c', 0x16, 0x03 };
    send_bytes(editing, sizeof(editing)); /* No LF: invisible to canonical poll. */
    pump(40);
    assert(press_count == before);
    const unsigned queries = cursor_requests;
    answer_cursor = 0;
    vram[0] = 'X';
    for (unsigned i = 0; i < 10; i++)
        snapshot();
    assert(cursor_requests == queries + 1);
    assert(press_count == before);
    struct termios querying;
    assert(tcgetattr(slave, &querying) == 0);
    assert(!(querying.c_lflag & (ICANON | ECHO)));
    assert(querying.c_lflag & ISIG); /* DSR isn't full raw keyboard mode yet. */
    assert(!(fcntl(STDOUT_FILENO, F_GETFL) & O_NONBLOCK));
    send_bytes("x\033[", 3);
    pump(10);
    send_bytes("4;", 2);
    pump(10);
    assert(press_count == before);
    send_bytes("3R\033[Ay", 6);
    pump(10);
    assert(press_count == before); /* Wait for the pending frame, not just CPR. */
    const size_t fallback = output_length;
    snapshot();
    check_mode(0);
    check_fallback_marker(output_bytes + fallback);
    await_presses(before + 7);
    assert(presses[before] == 0x1e && presses[before + 1] == 0x2e);
    assert(presses[before + 2] == 0x1d && presses[before + 3] == 0x2e);
    assert(presses[before + 4] == 0x2d);
    assert(presses[before + 5] == 0x48 && presses[before + 6] == 0x15);
    assert(held[0x48]);
    send_bytes("\033[1;1:3A", 8);
    pump(10);
    for (unsigned i = 0; i < 5; i++)
        snapshot();
    assert(cursor_requests == queries + 1);
    no_keys_held();
    answer_cursor = 1;
    terminal_renderer_close();
    check_restored();
}

static void cursor_timeout(void)
{
    pid_t child = fork();
    assert(child >= 0);
    const uint64_t started = terminal_input_clock();
    if (!child) {
        terminal_renderer_init();
        blank_frame();
        vram[0] = 'A'; vram[8] = 'B'; crtc[15] = 5;
        snapshot();
        answer_cursor = 0;
        const unsigned queries = cursor_requests;
        vram[0] = 'X';
        for (unsigned i = 0; i < 10; i++)
            snapshot();
        assert(cursor_requests == queries + 1);
        send_bytes("\033[4;", 4);
        pump(10);
        expect_cursor_timeout = 1;
        pump(1200);
        _exit(76);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 75);
    assert(terminal_input_clock() - started >= 900000000ULL);
    check_restored();
    drain_output();
}

static void prebaseline_line(void)
{
    send_bytes("ac\n", 3);
    const size_t before = press_count;
    pump(40);
    assert(press_count == before);
    blank_frame();
    crtc[1] = 8; crtc[6] = 4;
    vram[0] = '>'; crtc[15] = 1;
    snapshot();
    await_presses(before + 3);
    const size_t echoed = output_length;
    vram[2] = 'a'; vram[4] = 'c'; crtc[15] = 8;
    snapshot();
    assert(!strchr(output_bytes + echoed, 'a'));
    assert(!strchr(output_bytes + echoed, 'c'));
    assert(!strchr(output_bytes + echoed, '\n'));
}

static void transitions(void)
{
    terminal_renderer_init();
    check_mode(1);
    cooked_line();
    enter_fullscreen();
    hold_raw_key();
    tigt_terminal_release();
    assert(terminal_renderer_poll_input() == TIGT_OK);
    check_restored();
    no_keys_held();
    assert(tigt_terminal_restore() == TIGT_OK);
    assert(terminal_renderer_poll_input() == TIGT_OK);
    check_mode(0);
    hold_raw_key();
    const unsigned queries = cursor_requests;
    snapshot(); /* True release invalidates the old region's cursor provenance. */
    assert(cursor_requests == queries + 1);
    snapshot(); /* Commit the retained fullscreen image at the fresh CPR. */
    check_mode(0);
    const size_t recovery_output = output_length;
    /* DOS resumes sequential output by scrolling, not by clearing the screen. */
    memcpy(vram, vram + 8, 8);
    for (unsigned i = 8; i < 16; i += 2)
        vram[i] = ' ';
    crtc[15] = 4;
    snapshot();
    check_mode(0); /* A newline alone must not release raw input. */
    vram[8] = 'R'; crtc[15] = 5;
    snapshot();
    for (unsigned i = 0; i < 6; i++) {
        check_mode(0);
        snapshot();
    }
    assert(!strstr(output_bytes + recovery_output, "\033[2K"));
    assert(!strchr(output_bytes + recovery_output, '\f'));
    check_mode(1);
    no_keys_held();
    cooked_line();
    terminal_renderer_close();
    check_restored();
    no_keys_held();
}

static volatile sig_atomic_t received_signal, received_count;

static void record_signal(int signo)
{
    received_signal = signo;
    received_count++;
}

static void host_controls(void)
{
    const int signals[] = { SIGINT, SIGQUIT,
#ifdef SIGINFO
                            SIGINFO,
#endif
    };
    struct sigaction previous[sizeof(signals) / sizeof(*signals)];
    struct sigaction action = { .sa_handler = record_signal };
    sigemptyset(&action.sa_mask);
    for (size_t i = 0; i < sizeof(signals) / sizeof(*signals); i++)
        assert(sigaction(signals[i], &action, &previous[i]) == 0);
    assert(tigt_terminal_install_signal_handlers() == TIGT_OK);
    terminal_renderer_init();
    enter_fullscreen();
    press_count = 0;
    received_count = 0;

    /* Ctrl-V quotes the entire extended-protocol gesture, not only its press.
       Repeat and release must not become host signals or leave Ctrl held. */
    const char quoted_press[] = "\033[118;5:1u\033[118;5:3u\033[99;5:1u";
    send_bytes(quoted_press, sizeof(quoted_press) - 1);
    await_presses(2);
    assert(held[0x1d] && held[0x2e] && received_count == 0);
    const char repeat[] = "\033[99;5:2u";
    send_bytes(repeat, sizeof(repeat) - 1);
    pump(40); /* Guest keyboard hardware, not host repeats, owns typematic. */
    assert(press_count == 2 && held[0x1d] && held[0x2e] && received_count == 0);
    const char release[] = "\033[99;5:3u";
    send_bytes(release, sizeof(release) - 1);
    pump(40);
    no_keys_held();
    assert(received_count == 0);

    const size_t doubled = press_count;
    send_bytes("\026\026", 2);
    await_presses(doubled + 2);
    assert(presses[doubled] == 0x1d && presses[doubled + 1] == 0x2f);
    no_keys_held();
    const char quoted[] = { 0x16, 0x1c, 0x16, 0x1a, 0x16, 0x14 };
    const size_t controls = press_count;
    send_bytes(quoted, sizeof(quoted));
    await_presses(controls + 6);
    assert(presses[controls + 1] == 0x2b);
    assert(presses[controls + 3] == 0x2c);
    assert(presses[controls + 5] == 0x14);
    assert(received_count == 0);
    no_keys_held();

    const char control_bytes[] = { 0x03, 0x1c,
#ifdef SIGINFO
                                    0x14,
#endif
    };
    for (size_t i = 0; i < sizeof(signals) / sizeof(*signals); i++) {
        const size_t before = press_count;
        send_bytes(control_bytes + i, 1);
        pump(40);
        assert(received_count == (sig_atomic_t) i + 1);
        assert(received_signal == signals[i]);
        assert(press_count == before);
        no_keys_held();
#ifdef SIGINFO
        if (signals[i] == SIGINFO) {
            check_mode(0); /* Informational, never terminal teardown. */
            continue;
        }
#endif
        check_restored();
        assert(tigt_terminal_restore() == TIGT_OK);
        pump(10);
        check_mode(0);
    }
    terminal_renderer_close();
    check_restored();
    tigt_terminal_uninstall_signal_handlers();
    for (size_t i = 0; i < sizeof(signals) / sizeof(*signals); i++)
        assert(sigaction(signals[i], &previous[i], NULL) == 0);
}

static void cooked_quote(void)
{
    terminal_renderer_init();
    blank_frame();
    crtc[1] = 8; crtc[6] = 4;
    snapshot();
    const size_t before = press_count;
    /* Kernel VLNEXT removes the prefix. Re-filtering the returned Ctrl-C
       would raise SIGINT instead of passing it to DOS. */
    send_bytes("\026\003\n", 3);
    await_presses(before + 3);
    assert(presses[before] == 0x1d && presses[before + 1] == 0x2e);
    assert(presses[before + 2] == 0x1c);
    no_keys_held();
    terminal_renderer_close();
    check_restored();
}

static void set_foreground(pid_t group)
{
    sigset_t mask, previous;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTTOU);
    assert(sigprocmask(SIG_BLOCK, &mask, &previous) == 0);
    assert(tcsetpgrp(slave, group) == 0);
    assert(sigprocmask(SIG_SETMASK, &previous, NULL) == 0);
}

static char read_notification(int fd)
{
    char byte;
    ssize_t count;
    do {
        count = read(fd, &byte, 1);
    } while (count < 0 && errno == EINTR);
    assert(count == 1);
    return byte;
}

static void job_control(void)
{
    int ready[2], command[2];
    assert(pipe(ready) == 0 && pipe(command) == 0);
    const pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(ready[0]); close(command[1]);
        assert(setpgid(0, 0) == 0);
        assert(read_notification(command[0]) == 'f');
        assert(signal(SIGTSTP, SIG_DFL) != SIG_ERR);
        assert(tigt_terminal_install_signal_handlers() == TIGT_OK);
        terminal_renderer_init();
        enter_fullscreen();
        hold_raw_key();
        const size_t before = press_count;
        send_bytes("\032", 1); /* Host Ctrl-Z from adaptive raw input. */
        pump(40);
        /* The supervisor continues us in the background first. */
        no_keys_held();
        check_restored();
        assert(!tigt_terminal_is_foreground());
        send_bytes("b\n", 2);
        pump(40);
        assert(press_count == before); /* No background input reads. */
        assert(write(ready[1], "b", 1) == 1);
        assert(read_notification(command[0]) == 'f');
        await_presses(before + 2);
        assert(presses[before] == 0x30 && presses[before + 1] == 0x1c);
        check_mode(0);
        no_keys_held();
        terminal_renderer_close();
        check_restored();
        tigt_terminal_uninstall_signal_handlers();
        _exit(0);
    }
    close(ready[1]); close(command[0]);
    assert(setpgid(child, child) == 0);
    set_foreground(child);
    assert(write(command[1], "f", 1) == 1);
    int status;
    assert(waitpid(child, &status, WUNTRACED) == child);
    assert(WIFSTOPPED(status) && WSTOPSIG(status) == SIGTSTP);
    check_restored();
    set_foreground(getpgrp());
    assert(kill(child, SIGCONT) == 0);
    assert(read_notification(ready[0]) == 'b');
    set_foreground(child);
    assert(kill(child, SIGCONT) == 0);
    assert(write(command[1], "f", 1) == 1);
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    set_foreground(getpgrp());
    close(ready[0]); close(command[1]);
    check_restored();
    drain_output();
}

static void tty_eof(void)
{
    terminal_renderer_init();
    blank_frame();
    crtc[1] = 8; crtc[6] = 4;
    vram[0] = '>'; crtc[15] = 1;
    snapshot();
    const char eof = original.c_cc[VEOF];
    send_bytes(&eof, 1);
    pump(40);
    check_restored();
    send_bytes("b\n", 2);
    pump(40);
    assert(press_count == 0);
    no_keys_held();
    const size_t before_output = output_length;
    vram[2] = 'Z'; crtc[15] = 2;
    snapshot(); /* Input EOF must not release a still-live glass output owner. */
    assert(strchr(output_bytes + before_output, 'Z'));
    check_restored();
    terminal_renderer_close();
    check_restored();
}

static void pipe_eof(void)
{
    int descriptors[2];
    assert(pipe(descriptors) == 0);
    assert(dup2(descriptors[0], STDIN_FILENO) == STDIN_FILENO);
    close(descriptors[0]);
    original_flags = fcntl(STDIN_FILENO, F_GETFL);
    terminal_renderer_init();
    blank_frame();
    crtc[1] = 8; crtc[6] = 4;
    vram[0] = '>'; crtc[15] = 1;
    snapshot();
    dopause = 1;
    assert(write(descriptors[1], "ac\n", 3) == 3);
    close(descriptors[1]);
    pump(60);
    assert(press_count == 0);
    dopause = 0;
    await_presses(3);
    pump(40);
    assert(presses[0] == 0x1e && presses[1] == 0x2e && presses[2] == 0x1c);
    no_keys_held();
    check_restored();
    const size_t echoed = output_length;
    vram[2] = 'a'; vram[4] = 'c'; crtc[15] = 8;
    snapshot();
    assert(strstr(output_bytes + echoed, "ac"));
    assert(strchr(output_bytes + echoed, '\n'));
    terminal_renderer_close();
    check_restored();
}

static void redirected_echo(void)
{
    int output[2];
    assert(pipe(output) == 0);
    assert(dup2(output[1], STDOUT_FILENO) == STDOUT_FILENO);
    close(output[1]);
    terminal_renderer_init();
    blank_frame();
    crtc[1] = 8; crtc[6] = 4;
    vram[0] = '>'; crtc[15] = 1;
    snapshot();
    const size_t before = press_count;
    const size_t host_echo = output_length;
    send_bytes("ac\n", 3);
    await_presses(before + 3);
    assert(strstr(output_bytes + host_echo, "ac"));
    vram[2] = 'a'; vram[4] = 'c'; crtc[15] = 8;
    snapshot();
    terminal_renderer_close();
    check_restored();
    assert(dup2(slave, STDOUT_FILENO) == STDOUT_FILENO);
    char bytes[256];
    const ssize_t count = read(output[0], bytes, sizeof(bytes) - 1);
    assert(count > 0);
    bytes[count] = 0;
    /* Input echoed on the PTY, but the independent output stream must still
       receive the guest's text; matching "isatty(stdin)" alone loses it. */
    assert(strstr(bytes, ">ac\n"));
    close(output[0]);
}

static void output_backpressure(void)
{
    terminal_renderer_init();
    enter_fullscreen();
    /* A separate open description fills the queue without changing the
       blocking mode of the stdin/stdout description borrowed by the renderer. */
    int filler = open(ptsname(master), O_WRONLY | O_NOCTTY | O_NONBLOCK);
    assert(filler >= 0);
    char spaces[4096];
    memset(spaces, ' ', sizeof(spaces));
    size_t total = 0;
    ssize_t amount;
    while ((amount = write(filler, spaces, sizeof(spaces))) > 0) {
        total += (size_t) amount;
        assert(total < 16 * 1024 * 1024);
    }
    assert(amount < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    while ((amount = write(filler, spaces, 1)) > 0)
        total++;
    assert(amount < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    close(filler);

    pid_t reader = fork();
    assert(reader >= 0);
    if (!reader) {
        alarm(3);
        struct timespec delay = { .tv_nsec = 100000000 };
        nanosleep(&delay, NULL);
        const char expected[] = "WXYZ";
        size_t matched = 0;
        for (;;) {
            struct pollfd output = { .fd = master, .events = POLLIN };
            assert(poll(&output, 1, 100) >= 0);
            char bytes[4096];
            amount = read(master, bytes, sizeof(bytes));
            if (amount < 0) {
                assert(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
                continue;
            }
            assert(amount > 0);
            for (ssize_t i = 0; i < amount; i++) {
                matched = bytes[i] == expected[matched] ? matched + 1 : 0;
                if (matched == sizeof(expected) - 1)
                    _exit(0); /* Do not run the parent's renderer exit hook. */
            }
        }
    }
    for (unsigned i = 0; i < 4; i++)
        vram[2 * i] = "WXYZ"[i];
    /* This must wait for output capacity, not abort on EAGAIN or truncate. */
    terminal_video_snapshot(vram, crtc, 8, 0, NULL);
    int status;
    assert(waitpid(reader, &status, 0) == reader);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    terminal_renderer_close();
    check_restored();
}

static void process_exit(void)
{
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        terminal_renderer_init();
        enter_fullscreen();
        hold_raw_key();
        exit(0); /* Exercise registered exit restoration, not explicit close. */
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    check_restored();
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    /* Each case gets a real controlling terminal and foreground process group;
       an unattached PTY cannot exercise TIGT's job-control checks. */
    const pid_t child = fork();
    assert(child >= 0);
    if (child) {
        int status;
        assert(waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status));
        return WEXITSTATUS(status);
    }
    assert(setsid() >= 0);
    setup();
    assert(setenv("TIGT_PRESENTATION", "adaptive-reversible", 1) == 0);
    if (!strcmp(argv[1], "disable-mda") || !strcmp(argv[1], "disable-cga") ||
        !strcmp(argv[1], "disable-pcjr"))
        video_disable(!strcmp(argv[1], "disable-mda"), !strcmp(argv[1], "disable-pcjr"));
    else if (!strcmp(argv[1], "disable-raw"))
        video_disable_raw();
    else if (!strcmp(argv[1], "transitions")) {
        cursor_handshake();
        cursor_timeout();
        transitions();
        host_controls();
        job_control();
    }
    else if (!strcmp(argv[1], "exit"))
        process_exit();
    else if (!strcmp(argv[1], "backpressure"))
        output_backpressure();
    else if (!strcmp(argv[1], "raw-close")) {
        original.c_lflag &= ~(ECHO | ECHONL);
        assert(tcsetattr(slave, TCSANOW, &original) == 0);
        terminal_renderer_init();
        enter_fullscreen();
        hold_raw_key();
        terminal_renderer_close();
        check_restored();
        no_keys_held();
    }
    else {
        assert(setenv("TIGT_PRESENTATION", "glass", 1) == 0);
        if (!strcmp(argv[1], "tty-eof"))
            tty_eof();
        else if (!strcmp(argv[1], "pipe-eof"))
            pipe_eof();
        else {
            const int nonblocking = !strcmp(argv[1], "nonblocking");
            assert(nonblocking || !strcmp(argv[1], "glass"));
            if (nonblocking) {
                original_flags |= O_NONBLOCK;
                assert(fcntl(STDIN_FILENO, F_SETFL, original_flags) == 0);
            }
            terminal_renderer_init();
            if (nonblocking)
                assert(fcntl(STDIN_FILENO, F_GETFL) & O_NONBLOCK);
            check_mode(1);
            prebaseline_line();
            cooked_line();
            tigt_terminal_release();
            assert(terminal_renderer_poll_input() == TIGT_OK);
            check_restored();
            assert(tigt_terminal_restore() == TIGT_OK);
            assert(terminal_renderer_poll_input() == TIGT_OK);
            if (nonblocking)
                assert(fcntl(STDIN_FILENO, F_GETFL) & O_NONBLOCK);
            check_mode(1);
            cooked_line();
            terminal_renderer_close();
            check_restored();
            no_keys_held();
            redirected_echo();
            cooked_quote();
        }
    }
    teardown();
    puts("PASS real PTY input delivery and terminal restoration");
    return 0;
}
