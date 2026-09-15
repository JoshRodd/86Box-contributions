/* Exercise the real mapper and terminal translation through guest key state. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../src/terminal/terminal_renderer.c"

static uint8_t held[65536];
void keyboard_input(int down, uint16_t scan) { held[scan] = !!down; }
int keyboard_recv_ui(uint16_t scan) { return held[scan]; }

static void send_event(void *mapper, pc_xt_keyboard_v1_input_event *input)
{
    pc_xt_keyboard_v1_key_event keys[PC_XT_KEYBOARD_V1_EVENT_MAX_KEYS];
    size_t count = pc_xt_keyboard_v1_handle_events(mapper, input, keys,
                                                 PC_XT_KEYBOARD_V1_EVENT_MAX_KEYS);
    assert(count != PC_XT_KEYBOARD_V1_ERROR);
    for (size_t i = 0; i < count; i++)
        terminal_key(keys[i].down, keys[i].key);
}

static void exercise(uint32_t kind, uint8_t modifiers, uint16_t expected)
{
    memset(held, 0, sizeof(held));
    void *mapper = pc_xt_keyboard_v1_create(PC_XT_KEYBOARD_V1_AT_SET1);
    assert(mapper);
    pc_xt_keyboard_v1_input_event input = {
        .key = { .kind = kind }, .modifiers = modifiers,
        .kind = PC_XT_KEYBOARD_V1_PRESS
    };
    send_event(mapper, &input);
    assert(held[expected]);
    input.kind = PC_XT_KEYBOARD_V1_RELEASE;
    send_event(mapper, &input);
    for (size_t i = 0; i < sizeof(held); i++)
        assert(!held[i]);
    pc_xt_keyboard_v1_destroy(mapper);
}

int main(void)
{
    exercise(PC_XT_KEYBOARD_V1_KEY_PRINT_SCREEN, PC_XT_KEYBOARD_V1_MOD_ALT, 0x54);
    exercise(PC_XT_KEYBOARD_V1_KEY_PAUSE, PC_XT_KEYBOARD_V1_MOD_CONTROL, 0x146);
    exercise(PC_XT_KEYBOARD_V1_KEY_PRINT_SCREEN, 0, 0x137);
    exercise(PC_XT_KEYBOARD_V1_KEY_PAUSE, 0, 0x45);
    puts("PASS special keys release their press-time guest identities");
    return 0;
}
