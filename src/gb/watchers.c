/* watchers.c - OPEN WORLD Watchers (stretch): tall pale figures at night and in the Ash. */
#pragma bank 255
#include <gb/gb.h>
#include "game.h"
#include "gfx.h"

void watchers_reset(void) BANKED
{
    uint8_t i;
    for (i = 0; i < 4; i++) spr_hide((uint8_t)(SP_WATCH + i));
}

void watchers_update(void) BANKED
{
}
