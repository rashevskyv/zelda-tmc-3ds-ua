#!/usr/bin/env bash
# usage: emu_run.sh <out.png> [mash rounds]  -- launches the current 3D build in Azahar (stereo SBS) and plays to Link's house
set -u
S=${TMC_EMU_BACKUPS:-/tmp/claude-1000/-home-xhr-dev-zeldamc-zelda-tmc-3ds-ua/316ba6fc-14b1-45cf-86be-88cedd79fdfe/scratchpad}
U=/mnt/c/Users/Administrator/AppData/Roaming/Azahar
T=/mnt/c/Users/Administrator/AppData/Local/Temp/tmc3d
PS="powershell.exe -NoProfile -ExecutionPolicy Bypass -File"
powershell.exe -NoProfile -Command "Get-Process azahar -ErrorAction SilentlyContinue | Stop-Process -Force" >/dev/null 2>&1; sleep 2
python3 - "$U/config/qt-config.ini" <<'PY'
import sys,re
p=sys.argv[1]; s=open(p,encoding='utf-8').read()
for k,v in (('render_3d','1'),('factor_3d','100'),('confirmClose','false')):
    s=re.sub(r'(?m)^'+k+r'\\default=.*$', lambda m: k+'\\default=false', s)
    s=re.sub(r'(?m)^'+k+r'=.*$', lambda m: k+'='+v, s)
open(p,'w',encoding='utf-8',newline='').write(s)
PY
cp "$S/emu-tmc3ds.ini.bak" "$U/sdmc/3ds/The Minish Cap 3DS/tmc3ds.ini"
if [ -n "${3:-}" ]; then printf 'debug_warp=%s\n' "$3" >> "$U/sdmc/3ds/The Minish Cap 3DS/tmc3ds.ini"; fi
if [ -n "${4:-}" ]; then printf '%s\n' "$4" >> "$U/sdmc/3ds/The Minish Cap 3DS/tmc3ds.ini"; fi
cp /home/xhr/dev/zeldamc/zelda-tmc-3ds-3d/build-3ds/game/tmc-3ds-v2.2.3dsx "$U/sdmc/3ds/The Minish Cap 3DS/tmc-3ds-3dtest.3dsx"
cd $T
$PS shot.ps1 -Wait 6 -Out launch.png >/dev/null 2>&1
sleep 18
rounds=${2:-15}
if [ "$rounds" != "0" ]; then
  $PS key.ps1 -Keys "m,w2500,a,w1500,a,w3000" >/dev/null 2>&1
  for i in $(seq 1 $rounds); do $PS key.ps1 -Keys "a,m,a,a,s,a,a,m,a,a" -Gap 500 >/dev/null 2>&1; done
fi
if [ -n "${3:-}" ]; then sleep 7; fi
$PS grab.ps1 -Out "$1" 2>&1 | tail -1
