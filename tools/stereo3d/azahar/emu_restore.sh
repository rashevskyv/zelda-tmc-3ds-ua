#!/usr/bin/env bash
S=${TMC_EMU_BACKUPS:-/tmp/claude-1000/-home-xhr-dev-zeldamc-zelda-tmc-3ds-ua/316ba6fc-14b1-45cf-86be-88cedd79fdfe/scratchpad}
U=/mnt/c/Users/Administrator/AppData/Roaming/Azahar
powershell.exe -NoProfile -Command "Get-Process azahar -ErrorAction SilentlyContinue | Stop-Process -Force" >/dev/null 2>&1; sleep 2
cp "$S/qt-config.ini.bak" "$U/config/qt-config.ini"
cp "$S/emu-tmc.sav.bak" "$U/sdmc/3ds/The Minish Cap 3DS/tmc.sav"
cp "$S/emu-tmc3ds.ini.bak" "$U/sdmc/3ds/The Minish Cap 3DS/tmc3ds.ini"
rm -f "$U/sdmc/3ds/The Minish Cap 3DS/tmc-3ds-3dtest.3dsx"
echo restored
