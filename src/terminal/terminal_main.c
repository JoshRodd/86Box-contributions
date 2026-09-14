#include <stdbool.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <86box/86box.h>
#include <86box/plat.h>
#include <86box/video.h>
#include <tigt_terminal.h>
#include "terminal_renderer.h"

static volatile sig_atomic_t terminal_running = 1;

static void
terminal_set_default_logfile(int argc, char **argv)
{
    const char *vm_path = NULL;

    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "--logfile") || !strcmp(argv[i], "-L")) &&
            i + 1 < argc)
            return;
        if ((!strcmp(argv[i], "--vmpath") || !strcmp(argv[i], "-P")) &&
            i + 1 < argc)
            vm_path = argv[++i];
    }

    if (vm_path != NULL)
        snprintf(log_path, sizeof(log_path), "%s/terminal.log", vm_path);
    else
        strcpy(log_path, "terminal.log");
}

static void
terminal_exit_requested(int signal)
{
    (void) signal;
    terminal_running = 0;
}

static void
terminal_install_exit_handlers(void)
{
    struct sigaction action = {
        .sa_handler = terminal_exit_requested
    };

    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    sigaction(SIGQUIT, &action, NULL);
}


void
startblit(void)
{
}

void
endblit(void)
{
}

void
do_start(void)
{
    terminal_running = 1;
}

void
do_stop(void)
{
    terminal_running = 0;
}

void
terminal_power_off(void)
{
    terminal_running = 0;
}

int
main(int argc, char **argv)
{
    uint64_t old_ns;
    uint64_t debt_ns = 0;
    const uint64_t quantum_ns = 1000000ULL;
    const uint64_t max_debt_ns = 50000000ULL;

    terminal_set_default_logfile(argc, argv);
    /* These are application exit decisions, not terminal cleanup handlers.
       TIGT wraps them and owns release, job control, and crash handling. */
    terminal_install_exit_handlers();
    if (tigt_terminal_install_signal_handlers() != 0) {
        fprintf(stderr, "Terminal: could not install lifecycle handlers.\n");
        return 1;
    }

    if (!pc_init(argc, argv)) {
        tigt_terminal_uninstall_signal_handlers();
        return 0;
    }
    if (!pc_init_roms()) {
        fprintf(stderr, "No ROMs found.\n");
        tigt_terminal_uninstall_signal_handlers();
        return 6;
    }

    pc_init_modules();
    timer_freq = 1000000000ULL;
    terminal_renderer_init();
    pc_reset_hard_init();
    plat_pause(0);
    is_cpu_thread = 1;
    old_ns = plat_timer_read();
    int terminal_error = 0;

    while (terminal_running && cpu_thread_run) {
        const uint64_t now_ns = plat_timer_read();
        debt_ns += now_ns - old_ns;
        old_ns = now_ns;
        if (debt_ns > max_debt_ns)
            debt_ns = max_debt_ns;

        terminal_error = terminal_renderer_poll_input();
        if (terminal_error < 0 || !terminal_running)
            break;
        if (debt_ns >= quantum_ns && !dopause) {
            pc_run();
            debt_ns -= quantum_ns;
        } else {
            plat_delay_ms(1);
        }
    }

    terminal_renderer_close();
    pc_close(NULL);
    tigt_terminal_uninstall_signal_handlers();
    if (terminal_error < 0) {
        fprintf(stderr, "Terminal: lifecycle failed (%d).\n", terminal_error);
        return 1;
    }
    return 0;
}
