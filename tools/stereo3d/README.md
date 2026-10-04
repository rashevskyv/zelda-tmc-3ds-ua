# Stereoscopic 3D: notes and helper scripts

- `NOTES.md` — the working log (Ukrainian): state, rules, how the 3D works, what is next. Start here.
- `../stereo_editor/` — the PC relief editor (open `index.html` in a browser).
- `link/` — talk to a running game over its stereo link (port 8333):
  `frame.py HOST out.png` (both eyes), `bottom.py HOST out.png` (bottom panel),
  `disparity.py frame.png name:x0,x1,y0,y1` (eye shift of regions), `render_room.py room.bin out` (draw a /room blob).
- `azahar/` — drive Azahar from WSL for checks (see its README). The scripts expect Azahar's own
  config, save and ini backed up beside them (`qt-config.ini.bak`, `emu-tmc.sav.bak`, `emu-tmc3ds.ini.bak`,
  not in the repo) and `TMC_EMU_BACKUPS` pointing at that folder; PowerShell parts run from `%TEMP%\tmc3d`.
