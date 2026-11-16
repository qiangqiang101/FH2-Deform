#!/bin/sh
# gx.sh N NAME: restart with [Test] GlassExtra=N and GlassBreaksAt=${3:-0.05}, hit the back four times, tilt the camera, save the crop
T=/c/Users/ADMIN/Documents/GitHub/FH2Mouse/tools
G=/d/Games/FH2/azimuth-windows
F=/c/Users/ADMIN/Documents/GitHub/FH2Neon/build/tools/fh2mod.exe
sh $T/close_game.sh
INI=$G/fh2-mods/FH2Deform.ini
grep -q "^GlassExtra=" $INI || sed -i 's/^\[Test\]/[Test]\nGlassExtra=0/' $INI
sed -i "s/^GlassBreaksAt=.*/GlassBreaksAt=${3:-0.05}/; s/^GlassExtra=.*/GlassExtra=$1/" $INI
cp /c/Users/ADMIN/Documents/GitHub/FH2-Deform/build/Release/FH2Deform.dll $G/fh2-mods/FH2Deform.dll
cd $G && (./azimuth.exe --skip-launcher >/dev/null 2>&1 &)
for i in $(seq 1 60); do grep -q "FH2Deform\] shaders: Dent" $G/fh2-mods/platform.log 2>/dev/null && break; sleep 1; done
grep -a "FH2Deform\] shaders" $G/fh2-mods/platform.log | tail -1 | grep -o "on [0-9]* car shaders"
cd $T
python drive.py "focus" | head -1
python pad.py "press A"
python drive.py "wait 26000"
python drive.py "focus" | head -1
for i in 1 2 3 4; do $F FH2Deform hit back 25 | head -1; done
python pad.py "rstick 0 -32767 700"
python drive.py "wait 1500"
python -c "
import ctypes; ctypes.windll.user32.SetProcessDPIAware()
from PIL import ImageGrab
img=ImageGrab.grab(); w,h=img.size; img.crop((w//4,h//4,3*w//4,h)).save('shots/$2.png')"
