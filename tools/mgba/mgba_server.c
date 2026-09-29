/* mgba_server - a tiny headless mGBA front end driven over stdin/stdout, so Python tests
 * can run the ROM on a cycle-accurate emulator (tools/mgba/mgba_pyboy.py wraps it in a
 * PyBoy-like API).  Build: make build/mgba_server   (needs libmgba-dev).
 *
 * Commands (one per line), replies one line each:
 *   t N          run N frames                         -> "ok"
 *   k MASK       set held keys (A=1 B=2 SEL=4 ST=8 R=16 L=32 U=64 D=128) -> "ok"
 *   r ADDR       read a byte on the bus               -> "VAL"
 *   R ADDR LEN   read LEN bytes                       -> hex string
 *   w ADDR VAL   write a byte (raw)                   -> "ok"
 *   s PATH       save the screen as a binary PPM      -> "ok"
 *   q            quit
 */
#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/log.h>
#include <mgba-util/vfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void quiet_log(struct mLogger *l, int cat, enum mLogLevel lvl, const char *fmt, va_list a)
{ (void)l; (void)cat; (void)lvl; (void)fmt; (void)a; }
static struct mLogger logger = { .log = quiet_log };

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: mgba_server ROM dmg|cgb [SAVE]\n"); return 2; }
    mLogSetDefaultLogger(&logger);
    struct mCore *core = mCoreFind(argv[1]);
    if (!core || !core->init(core)) { fprintf(stderr, "no core\n"); return 1; }
    mCoreInitConfig(core, NULL);
    /* a CGB-compatible ROM takes its model from cgb.model (gb.model is for DMG-only ROMs) */
    mCoreConfigSetValue(&core->config, "gb.model", strcmp(argv[2], "dmg") == 0 ? "DMG" : "CGB");
    mCoreConfigSetValue(&core->config, "cgb.model", strcmp(argv[2], "dmg") == 0 ? "DMG" : "CGB");
    mCoreConfigSetValue(&core->config, "cgb.hybridModel", strcmp(argv[2], "dmg") == 0 ? "DMG" : "CGB");
    mCoreConfigSetValue(&core->config, "useBios", "0");
    core->loadConfig(core, &core->config);
    unsigned w, h;
    core->desiredVideoDimensions(core, &w, &h);
    color_t *buf = calloc((size_t)w * h, sizeof(color_t));
    core->setVideoBuffer(core, buf, w);
    if (!mCoreLoadFile(core, argv[1])) { fprintf(stderr, "load failed\n"); return 1; }
    if (argc > 3) {
        struct VFile *vf = VFileOpen(argv[3], O_RDWR | O_CREAT);
        if (vf) core->loadSave(core, vf);
    }
    core->reset(core);
    char line[512];
    setvbuf(stdout, NULL, _IOLBF, 0);
    while (fgets(line, sizeof line, stdin)) {
        unsigned a = 0, b = 0;
        switch (line[0]) {
        case 't': sscanf(line + 1, "%u", &a); while (a--) core->runFrame(core); puts("ok"); break;
        case 'k': sscanf(line + 1, "%u", &a); core->setKeys(core, a); puts("ok"); break;
        case 'r': sscanf(line + 1, "%u", &a); printf("%u\n", core->busRead8(core, a)); break;
        case 'R': sscanf(line + 1, "%u %u", &a, &b);
            for (unsigned i = 0; i < b; i++) printf("%02x", core->busRead8(core, a + i));
            putchar('\n'); break;
        case 'w': sscanf(line + 1, "%u %u", &a, &b); core->rawWrite8(core, a, -1, (uint8_t)b); puts("ok"); break;
        case 's': {
            char path[400]; sscanf(line + 1, "%399s", path);
            FILE *f = fopen(path, "wb");
            if (f) {
                fprintf(f, "P6\n%u %u\n255\n", 160u, 144u);
                for (unsigned y = 0; y < 144; y++) for (unsigned x = 0; x < 160; x++) {
                    color_t c = buf[y * w + x];
                    unsigned char px[3] = { (unsigned char)(c & 0xFF), (unsigned char)((c >> 8) & 0xFF), (unsigned char)((c >> 16) & 0xFF) };
                    fwrite(px, 1, 3, f);
                }
                fclose(f);
            }
            puts("ok"); break; }
        case 'q': core->deinit(core); return 0;
        default: puts("?"); break;
        }
    }
    core->deinit(core);
    return 0;
}
