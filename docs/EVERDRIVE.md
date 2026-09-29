# Playing Open World on real hardware (EverDrive)

This guide is for an **EverDrive GB X-series** cart (X3 / X5 / X7) in any Game Boy,
Game Boy Color, or GBC-compatible handheld such as the **ModRetro Chromatic**.

The ROM is a standard **MBC5 + RAM + battery** cartridge image (8 KB save RAM).
It works on original grey Game Boys and runs in full colour and double speed on Color hardware.

## 1. Get the ROM

1. Open the repository's **Releases** page. There is only ever one release; its tag is the build date (e.g. `2026.09.27`).
2. Download `open-world-YYYY.MM.DD.gb`.

   (You can ignore the `.sym` file. It holds debug symbols for emulators.)

## 2. Prepare the SD card (first time only)

1. Format a microSD card as **FAT32**. For cards over 32 GB, use a tool that can force FAT32, such as "Rufus" on Windows or `mkfs.vfat -F 32` on Linux. On macOS, use Disk Utility and choose "MS-DOS (FAT)".
2. The EverDrive needs its OS folder, **`GBCSYS`**, in the root of the card.
   - If your card came with the EverDrive, it probably already has it.
   - If not, download the latest OS for your model from krikzz.com (look for the "OS" / "firmware" zip for EverDrive GB X-series) and copy the `GBCSYS` folder to the root of the card. The current X-series OS is v1.06; the zip contains `GBCSYS/GBCOS.BIN`.

## 3. Copy the game

1. Make a folder for games if you like, e.g. `GB/`.
2. Copy `open-world-YYYY.MM.DD.gb` into it. Renaming it `Open World.gb` is fine.
3. Safely eject the card.

```
SD card
├── GBCSYS/           <- EverDrive OS (don't touch)
│   └── GBCOS.BIN
└── GB/
    └── Open World.gb
```

## 4. Play

1. Put the microSD card into the EverDrive, and the EverDrive into the handheld (label facing out on a Chromatic, same as any cartridge).
2. Power on. The EverDrive menu appears.
3. Go to the folder, highlight **Open World**, and press **A**. The game boots straight to the title screen.
4. Press **A** to continue your world, or **SELECT** to begin a new one.

## 5. Saves

The game keeps your world (position, lit fires, cairns, the map you have walked) in battery-backed save RAM. The EverDrive copies that RAM to `GBCSYS/SAVE/` as a `.srm` file whose name matches the ROM (`Open World.srm`). Cart save states, from the in-game menu, go in `GBCSYS/SNAP/` and are not the game's own progress.

- Progress is written whenever you light a fire or beacon, build a cairn, or take an item, so you can just switch off the handheld.
- EverDrive models differ in *when* they copy save RAM to the SD card:
  - Some write it when you return to the menu.
  - Some write it on the next power-on, before the menu appears.

  If your progress ever seems to vanish, do this: after playing, power-cycle the handheld once and let the EverDrive menu load before removing the SD card. For details, see your cart's manual on krikzz.com.
- To wipe your progress, delete `GBCSYS/SAVE/Open World.srm` from the card.

## Updating

When a new build comes out, the release tag changes to the new date. Download the new `.gb` file and copy it over the old one. Keep the file name the same so it keeps using your existing `.srm`.

## Emulators

Any good emulator works, including SameBoy, Gambatte, mGBA, BGB, Emulicious and PyBoy. Load the `.gb` file. Emulators in Game Boy Color mode get colour graphics.

## Troubleshooting

| Symptom | Fix |
| --- | --- |
| EverDrive says "file not found" / no menu | `GBCSYS` is missing or the card isn't FAT32. |
| Garbled graphics on boot | Re-seat the cartridge and clean the contacts. Make sure the EverDrive OS is current. |
| Game is grey on a Color handheld | That happens if the handheld is forcing DMG mode. Normally the game picks colour automatically. |
| Save doesn't persist | See **Saves** above, and check that the card isn't write-protected. |
