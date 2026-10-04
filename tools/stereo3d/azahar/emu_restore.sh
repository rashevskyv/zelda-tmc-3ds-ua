#!/usr/bin/env bash
S=${TMC_EMU_BACKUPS:-/tmp/claude-1000/-home-xhr-dev-zeldamc-zelda-tmc-3ds-ua/316ba6fc-14b1-45cf-86be-88cedd79fdfe/scratchpad}
U=/mnt/c/Users/Administrator/AppData/Roaming/Azahar
# Someone may be playing something else in Azahar: then touch nothing (its
# window, its config) and stop. Only our own test windows are closed.
other=$(powershell.exe -NoProfile -Command "Get-Process azahar -ErrorAction SilentlyContinue | Where-Object { \$_.MainWindowTitle -match '\|' -and \$_.MainWindowTitle -notmatch 'Minish' } | ForEach-Object { \$_.MainWindowTitle }" 2>/dev/null | tr -d '\r')
if [ -n "$other" ]; then echo "Azahar is busy with another game ($other); not touching it" >&2; exit 3; fi
powershell.exe -NoProfile -Command "Get-Process azahar -ErrorAction SilentlyContinue | Where-Object { \$_.MainWindowTitle -notmatch '\|' -or \$_.MainWindowTitle -match 'Minish' } | Stop-Process -Force" >/dev/null 2>&1; sleep 2
cp "$S/qt-config.ini.bak" "$U/config/qt-config.ini"
cp "$S/emu-tmc.sav.bak" "$U/sdmc/3ds/The Minish Cap 3DS/tmc.sav"
cp "$S/emu-tmc3ds.ini.bak" "$U/sdmc/3ds/The Minish Cap 3DS/tmc3ds.ini"
rm -f "$U/sdmc/3ds/The Minish Cap 3DS/tmc-3ds-3dtest.3dsx"
echo restored
