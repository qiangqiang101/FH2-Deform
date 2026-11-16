#!/bin/sh
# cycle.sh MODE JOIN NAME: restart the game with [Test] TestAllVariants=MODE, TestJoinPush=JOIN, drive the same
# crashes as before, and save a screenshot as FH2Mouse/tools/shots/NAME.png
T=/c/Users/ADMIN/Documents/GitHub/FH2Mouse/tools
G=/d/Games/FH2/azimuth-windows
sh $T/close_game.sh
INI=$G/fh2-mods/FH2Deform.ini
sed -i "s/^TestAllVariants=.*/TestAllVariants=$1/; s/^TestJoinPush=.*/TestJoinPush=$2/" $INI
cp /c/Users/ADMIN/Documents/GitHub/FH2-Deform/build/Release/FH2Deform.dll $G/fh2-mods/FH2Deform.dll
cd $G && (./azimuth.exe --skip-launcher >/dev/null 2>&1 &)
for i in $(seq 1 60); do grep -q "FH2Deform\] shaders: Dent" $G/fh2-mods/platform.log 2>/dev/null && break; sleep 1; done
grep -a "FH2Deform\] shaders" $G/fh2-mods/platform.log | tail -2
cd $T
python drive.py "focus" | head -1
python pad.py "press A"
python drive.py "wait 9500"
python drive.py "focus" | head -1
python pad.py "trigger 255 0 2500; stick 32000 0 600; trigger 0 255 4000; trigger 255 0 1500; stick -32000 0 700; trigger 0 255 4000; trigger 255 0 1500; trigger 0 255 4000"
python drive.py "shot $3"
