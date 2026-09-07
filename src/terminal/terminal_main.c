#include <stdbool.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <86box/86box.h>
#include <86box/plat.h>
#include <86box/video.h>
#include "terminal_renderer.h"

static volatile sig_atomic_t terminal_running = 1;
static volatile sig_atomic_t terminal_suspend_requested;

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
terminal_signal_handler(int signal)
{
    if (signal == SIGTSTP)
        terminal_suspend_requested = 1;
    else
        terminal_running = 0;
}

static void
terminal_install_signal_handlers(void)
{
    struct sigaction action = {
        .sa_handler = terminal_signal_handler
    };

    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGTSTP, &action, NULL);
}

static void
terminal_suspend(void)
{
    struct sigaction action = {
        .sa_handler = SIG_DFL
    };

    terminal_renderer_suspend();
    sigemptyset(&action.sa_mask);
    sigaction(SIGTSTP, &action, NULL);
    raise(SIGTSTP);
    terminal_install_signal_handlers();
    terminal_renderer_resume();
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
    terminal_install_signal_handlers();

    if (!pc_init(argc, argv))
        return 0;
    if (!pc_init_roms()) {
        fprintf(stderr, "No ROMs found.\n");
        return 6;
    }

    pc_init_modules();
    timer_freq = 1000000000ULL;
    terminal_renderer_init();
    pc_reset_hard_init();
    plat_pause(0);
    is_cpu_thread = 1;
    old_ns = plat_timer_read();

    while (terminal_running && cpu_thread_run) {
        const uint64_t now_ns = plat_timer_read();
        debt_ns += now_ns - old_ns;
        old_ns = now_ns;
        if (debt_ns > max_debt_ns)
            debt_ns = max_debt_ns;

        if (terminal_suspend_requested) {
            terminal_suspend_requested = 0;
            terminal_suspend();
        }
        if (debt_ns >= quantum_ns && !dopause) {
            pc_run();
            debt_ns -= quantum_ns;
        } else {
            plat_delay_ms(1);
        }
    }

    terminal_renderer_close();
    pc_close(NULL);
    return 0;
}
