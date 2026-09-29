"""A PyBoy-like wrapper around build/mgba_server (headless mGBA), so the same Python test
and autopilot code can run the ROM on a cycle-accurate emulator.

Only the small API surface our tools use is provided: tick(), button_press/release,
memory[addr] (reads cached per 256-byte page per frame), memory[bank, addr] for SRAM/VRAM
bank 0, symbol_lookup(), screen.image, set_emulation_speed(), stop().
"""
import io
import os
import subprocess
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SERVER = os.path.join(ROOT, 'build', 'mgba_server')
KEYS = {'a': 1, 'b': 2, 'select': 4, 'start': 8, 'right': 16, 'left': 32, 'up': 64, 'down': 128}


class _Mem:
    def __init__(self, emu):
        self.e = emu
        self.pages = {}

    def _page(self, p):
        d = self.pages.get(p)
        if d is None:
            d = bytes.fromhex(self.e.cmd(f'R {p << 8} 256'))
            self.pages[p] = d
        return d

    def __getitem__(self, k):
        if isinstance(k, tuple):
            k = k[1]
        if isinstance(k, slice):
            return [self[i] for i in range(k.start, k.stop, k.step or 1)]
        return self._page(k >> 8)[k & 0xFF]

    def __setitem__(self, k, v):
        if isinstance(k, tuple):
            k = k[1]
        self.e.cmd(f'w {k} {v & 0xFF}')
        self.pages.pop(k >> 8, None)


class _Screen:
    def __init__(self, emu):
        self.e = emu

    @property
    def image(self):
        from PIL import Image
        fd, path = tempfile.mkstemp(suffix='.ppm')
        os.close(fd)
        self.e.cmd(f's {path}')
        im = Image.open(path).convert('RGB')
        im.load()
        os.remove(path)
        return im


class MgbaBoy:
    def __init__(self, rom, window='null', cgb=True, symbols=None, sound_emulated=False, ram_file=None, **_):
        self.syms = {}
        if symbols:
            for line in open(symbols):
                parts = line.split()
                if len(parts) == 2 and ':' in parts[0]:
                    bank, addr = parts[0].split(':')
                    self.syms[parts[1]] = (int(bank, 16), int(addr, 16))
        self._sav = None
        args = [SERVER, rom, 'cgb' if cgb else 'dmg']
        if ram_file is not None:
            fd, self._sav = tempfile.mkstemp(suffix='.sav')
            with os.fdopen(fd, 'wb') as f:
                f.write(ram_file.getvalue() if isinstance(ram_file, io.BytesIO) else ram_file.read())
            args.append(self._sav)
        self.p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self.keys = 0
        self.memory = _Mem(self)
        self.screen = _Screen(self)
        self.frame_count = 0

    def cmd(self, s):
        self.p.stdin.write(s + '\n')
        self.p.stdin.flush()
        return self.p.stdout.readline().strip()

    def set_emulation_speed(self, _):
        pass

    def symbol_lookup(self, name):
        return self.syms[name]

    def tick(self, n=1, render=True, *_):
        self.cmd(f't {n}')
        self.memory.pages.clear()
        self.frame_count += n
        return True

    def button_press(self, b):
        self.keys |= KEYS[b]
        self.cmd(f'k {self.keys}')

    def button_release(self, b):
        self.keys &= ~KEYS[b]
        self.cmd(f'k {self.keys}')

    def stop(self, save=True):
        try:
            self.cmd('q')
        except Exception:
            pass
        self.p.wait()
        if self._sav:
            os.remove(self._sav)
