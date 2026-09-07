#include <stdio.h>

#include <86box/ui.h>

int
ui_msgbox(int flags, char *message)
{
    return ui_msgbox_header(flags, NULL, message);
}

int
ui_msgbox_header(int flags, char *header, char *message)
{
    (void) flags;
    if (header)
        fprintf(stderr, "%s: ", header);
    fprintf(stderr, "%s\n", message);
    return 1;
}

void ui_emu_status(int speed_percent) { (void) speed_percent; }
void ui_hard_reset_completed(void) {}
void ui_init_monitor(int monitor_index) { (void) monitor_index; }
void ui_deinit_monitor(int monitor_index) { (void) monitor_index; }
void ui_sb_set_ready(int ready) { (void) ready; }
void ui_sb_update_panes(void) {}
void ui_sb_update_text(void) {}
void ui_sb_update_tip(int meaning) { (void) meaning; }
void ui_sb_update_icon(int tag, int active) { (void) tag; (void) active; }
void ui_sb_update_icon_write(int tag, int write) { (void) tag; (void) write; }
void ui_sb_update_icon_state(int tag, int state) { (void) tag; (void) state; }
void ui_sb_update_icon_wp(int tag, int state) { (void) tag; (void) state; }
void ui_sb_set_text(char *str) { (void) str; }
void ui_sb_bugui(char *str) { (void) str; }
void ui_sb_mt32lcd(char *str) { (void) str; }
void ui_update_force_interpreter(void) {}
