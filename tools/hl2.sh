#!/bin/sh
# hl2.sh NAME LIST...: for each LIST: restart (test profile, force close ok), HideList=LIST, A, hits at the back, shot shots/NAME_<i>.png
G=/d/Games/FH2/azimuth-windows; INI=$G/fh2-mods/FH2Deform.ini
T=/c/Users/ADMIN/Documents/GitHub/FH2Mouse/tools; F=/c/Users/ADMIN/Documents/GitHub/FH2Neon/build/tools/fh2mod.exe
N=$1; shift; i=0
for L in "$@"; do
  i=$((i+1))
  taskkill //IM azimuth.exe //F >/dev/null 2>&1; sleep 4
  sed -i "s/^HideList=.*/HideList=$L/; s/^GlassPixel=.*/GlassPixel=1/" $INI
  (cd $G && ./azimuth.exe --skip-launcher >/dev/null 2>&1 &)
  sleep 32; cd $T
  python drive.py "focus" | head -1; python pad.py "press A"; python drive.py "wait 30000"
  for s in 1 2 3 4; do $F FH2Deform hit back 25 | head -1; done
  python drive.py "wait 2500"
  python -c "
import ctypes; ctypes.windll.user32.SetProcessDPIAware()
from PIL import ImageGrab
img=ImageGrab.grab(); w,h=img.size; img.crop((w//4,h//5,3*w//4,h)).save('shots/${N}_$i.png')"
  echo "$i = $L"
done
