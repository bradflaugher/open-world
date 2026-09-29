
#include <gb/gb.h>
#include <stdint.h>
#include "sound.h"
/* mailbox at 0xD800:
   [0] mode req (0xFF none)  [1] sfx req (0xFF none)  [2] biome [3] phase [4] weather
   [5] set flag (1 = call ambient_set)  [6] beacons (0xFF none)  [7] tempo (0xFF none)
   [8..9] last tick cost  [10..11] max  [12..15] sum (units of 16 M-cycles)
   [16..17] seed, [18] seed flag  [19] frame ack counter */
#define MB ((volatile uint8_t *)0xD800)
void main(void)
{
    uint16_t cost, mx = 0; uint32_t sum = 0;
    MB[0] = 0xFF; MB[1] = 0xFF; MB[5] = 0; MB[6] = 0xFF; MB[7] = 0xFF; MB[18] = 0;
    sound_init();
    TAC_REG = 0x06;                    /* 65536 Hz: 1 count = 64 T = 16 M-cycles */
    while (1) {
        wait_vbl_done();
        if (MB[18]) { ambient_seed(MB[16] | ((uint16_t)MB[17] << 8)); MB[18] = 0; }
        if (MB[5]) { ambient_set(MB[2], MB[3], MB[4]); MB[5] = 0; }
        if (MB[6] != 0xFF) { ambient_beacons(MB[6]); MB[6] = 0xFF; }
        if (MB[7] != 0xFF) { ambient_tempo(MB[7]); MB[7] = 0xFF; }
        if (MB[0] != 0xFF) { ambient_mode(MB[0]); MB[0] = 0xFF; }
        if (MB[1] != 0xFF) { sfx_play(MB[1]); MB[1] = 0xFF; }
        disable_interrupts();
        TIMA_REG = 0; IF_REG &= ~TIM_IFLAG;
        sound_tick();
        cost = TIMA_REG;
        if (IF_REG & TIM_IFLAG) cost += 256;
        enable_interrupts();
        if (cost > mx) mx = cost;
        sum += cost;
        MB[8] = cost; MB[9] = cost >> 8; MB[10] = mx; MB[11] = mx >> 8;
        MB[12] = sum; MB[13] = sum >> 8; MB[14] = sum >> 16; MB[15] = sum >> 24;
        MB[19]++;
    }
}
