#pragma once
#include <stdbool.h>
#include <stdint.h>
void snesrecomp_android_input(uint32_t buttons);
void snesrecomp_android_pause(int paused);
void snesrecomp_android_quit(void);
void snesrecomp_android_slot(int save);
uint32_t snesrecomp_android_buttons(void);
int snesrecomp_android_commands(bool *running, uint8_t *paused);
