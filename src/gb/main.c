/* OPEN WORLD - a wordless open-world wanderer for Game Boy / Game Boy Color.
 * Bank 0 entry point. The game state machine lives in a switchable bank (game.c); bank 0
 * keeps the interrupts, the streaming land ring, the world core and the sound engine. */
#include <gb/gb.h>
#include "game.h"

void main(void)
{
    game_main();
}
