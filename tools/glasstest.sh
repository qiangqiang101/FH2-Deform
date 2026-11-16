#!/bin/sh
# glasstest.sh THRESHOLD NAME: restart with GlassBreaksAt=THRESHOLD, hit the left side of the car four times, look at that
# side from outside, save a full-resolution crop as FH2Mouse/tools/shots/NAME.png
T=/c/Users/ADMIN/Documents/GitHub/FH2Mouse/tools
G=/d/Games/FH2/azimuth-windows
F=/c/Users/ADMIN/Documents/GitHub/FH2Neon/build/tools/fh2mod.exe
sh $T/close_game.sh
sed -i "s/^GlassBreaksAt=.*/GlassBreaksAt=$1/; s/^TestAllVariants=.*/TestAllVariants=${3:-0}/; s/^TestJoinPush=.*/TestJoinPush=${4:-0}/" $G/fh2-mods/FH2Deform.ini
cp /c/Users/ADMIN/Documents/GitHub/FH2-Deform/build/Release/FH2Deform.dll $G/fh2-mods/FH2Deform.dll
cd $G && (./azimuth.exe --skip-launcher >/dev/null 2>&1 &)
for i in $(seq 1 60); do grep -q "FH2Deform\] shaders: Dent" $G/fh2-mods/platform.log 2>/dev/null && break; sleep 1; done
cd $T
python drive.py "focus" | head -1
python pad.py "press A"
python drive.py "wait 16000"
python drive.py "focus" | head -1
for i in 1 2 3 4; do $F FH2Deform hit back 25; done
python drive.py "focus" | head -1
python drive.py "wait 1500"
sleep 1.5
python -c "
import ctypes; ctypes.windll.user32.SetProcessDPIAware()
from PIL import ImageGrab
img=ImageGrab.grab(); img.save('shots/$2_full.png')"
