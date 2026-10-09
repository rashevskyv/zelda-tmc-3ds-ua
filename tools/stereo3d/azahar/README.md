# Testing the 3DS build in Azahar from WSL

Azahar: `C:\Program Files\Azahar\azahar.exe`, data in `%APPDATA%\Azahar`
(`/mnt/c/Users/Administrator/AppData/Roaming/Azahar`). Its emulated SD already has the ROM and a
save in `sdmc/3ds/The Minish Cap 3DS/`; the game's own log is `tmc3ds.log` there.

1. Stereo side by side: in `config/qt-config.ini` set `render_3d=1`, `factor_3d=100` (the 3D
   slider, 0..100) and their `\default=false`; `confirmClose=false`. Back the file up first and
   restore it afterwards.
2. Copy the build to `sdmc/3ds/The Minish Cap 3DS/tmc-3ds-3dtest.3dsx`.
3. Run the scripts from a Windows path (copy them to `%TEMP%\tmc3d`):
   - `powershell.exe -NoProfile -ExecutionPolicy Bypass -File shot.ps1 -Wait 24` starts Azahar on
     that 3dsx and waits for the title screen.
   - `grab.ps1 -Out grab.png` captures the window with PrintWindow, so it works while the window
     is covered (a plain screen copy does not, and SetForegroundWindow is refused).
   - `key.ps1 -Keys "m,w2500,a"` posts key presses: A=`a`, B=`s`, Start=`m`, D-pad `t g f h`,
     circle pad arrows (`up down left right`), `wN` waits N ms, `a*500` holds 500 ms.
   - Title -> `m` -> file select -> `a`, `a` -> intro; mashing `a`/`m` for ~2.5 min reaches
     Link's house (free play).
4. `python3 eyes.py grab.png link:104,122,32,62 floor:30,95,70,125` measures per-region disparity.
5. Stop it with `Get-Process azahar | Stop-Process -Force`.

## Notes

- `dispmap.py grab.png` prints a per-cell disparity map; periodic textures (floor boards) can fool
  it, so confirm with `eyes.py` regions or by diffing against a capture of the same room.
- `touch.ps1 -X 160 -Y 120 [-SideBySide]` posts the press to the OpenGL widget (from the Emerald 3DS port; untested here).
- Touching the emulated bottom screen does not work with posted mouse messages (`click.ps1` is
  kept for a later attempt); the settings panel cannot be driven this way yet.
- The save on the emulated SD is a new game: after the intro, ~21 rounds of mashing end in Link's
  bedroom. Back up `tmc.sav`, `tmc3ds.ini` and `config/qt-config.ini` first and restore them after.
- The logo screens last about two seconds; grab within 6-9 s of launch to catch them.

## Scripted runs

`emu_run.sh out.png [rounds] [warp] [ini line]` does the whole trip: stereo config, copy the build
from the 3D worktree, launch, mash through the intro (21 rounds ends in Link's bedroom), grab.
`emu_restore.sh` puts Azahar's config, save and ini back. The user's original files, as they were
before any test, are kept beside these scripts (`qt-config.ini.bak`, `emu-tmc.sav.bak`,
`emu-tmc3ds.ini.bak`); run both scripts with `TMC_EMU_BACKUPS=~/dev/zeldamc/azahar-test`.

- `warp` is `area,room,x,y,layer` for the build's ini-only `debug_warp` key (3D worktree,
  `platform/3ds/source/debug_warp_3ds.c`): one second after Link can move he is sent there.
  Area ids are the `Area` enum in `include/area.h` (Minish Woods 0, Hyrule Town 2, Hyrule Field 3,
  Lake Hylia 11), rooms are in `include/roomid.h`. Landing inside a wall is fine for a look.
  Example: `emu_run.sh field.png 21 "3,1,0x200,0x100,1"` (South Hyrule Field).
- `ini line` is appended to the game's ini, e.g. `stereo_relief=0` for a flat reference.
- `reliefmap.py flat.png relief.png` prints, per 8x8 cell, how much nearer the relief capture is
  than the flat one of the same scene -- the reliable way to see which cells were raised.
