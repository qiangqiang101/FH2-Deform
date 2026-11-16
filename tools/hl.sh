#!/bin/sh
# hl.sh LIST NAME: restart with [Test] HideList=LIST (always collapsed), hit the back, save the crop
G=/d/Games/FH2/azimuth-windows; INI=$G/fh2-mods/FH2Deform.ini
sh /c/Users/ADMIN/Documents/GitHub/FH2Mouse/tools/close_game.sh >/dev/null
grep -q "^HideList=" $INI || sed -i 's/^\[Test\]/[Test]\nHideList=/' $INI
sed -i "s/^HideList=.*/HideList=$1/; s/^GlassBreaksAt=.*/GlassBreaksAt=0.30/; s/^GlassExtra=.*/GlassExtra=0/" $INI
sh /c/Users/ADMIN/Documents/GitHub/FH2-Deform/tools/gx.sh 0 $2 0.30 | tail -1
