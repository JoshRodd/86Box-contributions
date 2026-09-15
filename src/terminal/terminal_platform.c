#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <86box/mem.h>
#include <86box/config.h>

#include <86box/86box.h>
#include <86box/hdc_ide.h>
#include <86box/hdd.h>
#include <86box/nvr.h>
#include <86box/path.h>
#include <86box/plat.h>
#include <86box/rom.h>
#include <86box/gameport.h>
#include <86box/version.h>

extern void terminal_power_off(void);
extern int nvr_save(void);

int hide_status_bar;
int hide_tool_bar;
volatile int cpu_thread_run = 1;
int fast_forward;
int fixed_size_x = 640;
int fixed_size_y = 480;
int kbd_req_capture;
int mouse_capture;
int rctrl_is_lalt;
int update_icons;
int joysticks_present;
joystick_state_t joystick_state[GAMEPORT_MAX][MAX_JOYSTICKS];
plat_joystick_state_t plat_joystick_state[MAX_PLAT_JOYSTICKS];

void joystick_init(void) {}
void joystick_close(void) {}
void joystick_process(uint8_t gp) { (void) gp; }

FILE *plat_fopen(const char *path, const char *mode) { return fopen(path, mode); }
FILE *plat_fopen64(const char *path, const char *mode) { return fopen(path, mode); }
void plat_remove(char *path) { remove(path); }
int plat_getcwd(char *buf, int max) { return getcwd(buf, max) != NULL; }
int plat_chdir(char *path) { return chdir(path); }
int plat_dir_check(char *path) { struct stat st; return !stat(path, &st) && S_ISDIR(st.st_mode); }
int plat_file_check(const char *path) { struct stat st; return !stat(path, &st) && !S_ISDIR(st.st_mode); }
int plat_dir_create(char *path) { return mkdir(path, S_IRWXU); }

void plat_tempfile(char *buf, char *prefix, char *suffix)
{
    snprintf(buf, 1024, "%s-%ld%s", prefix ? prefix : "86box", (long) time(NULL), suffix ? suffix : "");
}

void path_normalize(char *path) { (void) path; }
int path_abs(char *path) { return path[0] == '/'; }
void path_slash(char *path) { size_t len = strlen(path); if (len && path[len - 1] != '/') strcat(path, "/"); }
const char *path_get_slash(char *path) { return path[strlen(path) - 1] == '/' ? "" : "/"; }
char *path_get_basename(const char *path) { return (char *) basename((char *) path); }
char *path_get_filename(char *path) { char *slash = strrchr(path, '/'); return slash ? slash + 1 : path; }
char *path_get_extension(char *path) { char *dot = strrchr(path, '.'); return dot ? dot + 1 : path + strlen(path); }
void path_append_filename(char *dest, const char *s1, const char *s2) { strcpy(dest, s1); path_slash(dest); strcat(dest, s2); }
void path_get_dirname(char *dest, const char *path) { strcpy(dest, path); char *slash = strrchr(dest, '/'); if (slash) *slash = '\0'; else dest[0] = '\0'; }

void plat_get_exe_name(char *path, int size)
{
    ssize_t len = readlink("/proc/self/exe", path, size - 1);
    if (len > 0) { path[len] = '\0'; return; }
    if (!getcwd(path, size)) path[0] = '\0';
    path_slash(path); strncat(path, EMU_NAME, size - strlen(path) - 1);
}

void plat_get_global_data_dir(char *out, size_t len)
{
    const char *home = getenv("HOME");
    snprintf(out, len, "%s/.local/share/" EMU_NAME "/", home ? home : ".");
}
void plat_get_global_config_dir(char *out, size_t len) { plat_get_global_data_dir(out, len); }
void plat_get_temp_dir(char *out, uint8_t len) { const char *tmp = getenv("TMPDIR"); snprintf(out, len, "%s", tmp ? tmp : "/tmp/"); path_slash(out); }
void plat_get_vmm_dir(char *out, size_t len) { if (len) out[0] = '\0'; }
void plat_init_rom_paths(void) {}
void plat_init_asset_paths(void) {}

uint64_t plat_timer_read(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t) t.tv_sec * 1000000000ULL + t.tv_nsec; }
uint32_t plat_get_ticks(void) { return (uint32_t) (plat_timer_read() / 1000000ULL); }
void plat_delay_ms(uint32_t count) { struct timespec t = { count / 1000, (long) (count % 1000) * 1000000L }; nanosleep(&t, NULL); }

void *plat_mmap(size_t size, uint8_t executable, uint8_t *large)
{
    int flags = MAP_ANON | MAP_PRIVATE;

    if (large) *large = 0;
#if defined(__APPLE__) && defined(MAP_JIT)
    if (executable) flags |= MAP_JIT;
#endif
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE | (executable ? PROT_EXEC : 0), flags, -1, 0);
    return ptr == MAP_FAILED ? NULL : ptr;
}
void plat_munmap(void *ptr, size_t size) { munmap(ptr, size); }
void plat_set_thread_name(void *thread, const char *name) { (void) thread; pthread_setname_np(name); }
void plat_break(void) {}
void plat_clean_up(void) {}
int plat_run_command(const char *cmd, const char **env, const char *title) { (void) env; (void) title; return system(cmd) == 0; }

void plat_power_off(void)
{
    confirm_exit_cmdl = 0;
    ide_wait_for_async_reads();
    hdd_image_sync_all();
    nvr_save();
    config_save();
    terminal_power_off();
}
void plat_pause(int paused) { do_pause(paused); }
void plat_mouse_capture(int on) { (void) on; }
int plat_vidapi(const char *name) { (void) name; return 0; }
char *plat_vidapi_name(int api) { (void) api; return "terminal"; }
void plat_resize(int x, int y, int monitor) { (void) x; (void) y; (void) monitor; }
void plat_resize_request(int x, int y, int monitor) { (void) x; (void) y; (void) monitor; }
int plat_language_code(char *lang) { (void) lang; return 0; }
void plat_language_code_r(int id, char *out, int len) { (void) id; if (len) out[0] = '\0'; }
void plat_get_cpu_string(char *out, uint8_t len) { snprintf(out, len, "Unknown"); }
int stricmp(const char *a, const char *b) { return strcasecmp(a, b); }
int strnicmp(const char *a, const char *b, size_t n) { return strncasecmp(a, b, n); }

char *plat_get_string(int id)
{
    static char unknown[] = "Platform error";
    (void) id;
    return unknown;
}

int plat_is_block_device(const char *path) { (void) path; return 0; }
int64_t plat_get_block_device_size(const char *path) { (void) path; return -1; }
plat_device_vol_locked_t *plat_lock_volumes(FILE *file) { (void) file; return NULL; }
void plat_unlock_volumes(plat_device_vol_locked_t *vol) { (void) vol; }
