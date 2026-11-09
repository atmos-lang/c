#!/bin/bash
# setup.sh : builds everything under $W (default ~/atmos-bench): the games
# (rewritten copies), the reference (atmos-lang/atmos + lua-atmos/atmos,
# main, in a local luarocks tree) and pico-sdl v0.7 for both interpreters.
# Needs (Ubuntu): libsdl2{,-image,-ttf,-mixer,-gfx}-dev lua5.4 liblua5.4-dev
# luarocks xvfb python3 (python3-pil for compare.py).
set -e
H=$(cd "$(dirname "$0")" && pwd)
C=$(cd "$H/../.." && pwd)                 # this implementation
W=${W:-$HOME/atmos-bench}
mkdir -p $W/src $W/ours/atmos/env $W/inc/lua5.4
clone () { [ -d $W/src/$1 ] || git clone -q ${2:+--depth 1} https://github.com/$1 $W/src/$1; }
clone atmos-lang/pico-rocks 1; clone atmos-lang/pico-birds 1; clone kboltiz/snkrx 1
clone atmos-lang/atmos 1; clone lua-atmos/atmos 1; clone fsantanna/pico-sdl
git -C $W/src/fsantanna/pico-sdl checkout -q v0.7
# the games
python3 $H/prepare.py $W/src $W/games
# the reference: lua-atmos/atmos + atmos-lang/atmos (main) in $W/refdev,
# pico-sdl 0.7 rock (f-streams comes from 'luarocks install atmos-lang')
luarocks --lua-version=5.4 install pico-sdl 0.7
(cd $W/src/lua-atmos/atmos && luarocks --lua-version=5.4 --tree=$W/refdev make --deps-mode=none atmos-dev-4.rockspec)
(cd $W/src/atmos-lang/atmos && luarocks --lua-version=5.4 --tree=$W/refdev make --deps-mode=none atmos-lang-dev-4.rockspec)
# this implementation: pico against our headers, pico's wrapper and the
# benchmark env converted to Atmos
for h in lua.h lauxlib.h luaconf.h lualib.h; do ln -sf $C/$h $W/inc/lua5.4/$h; done
P=$W/src/fsantanna/pico-sdl
gcc -O2 -fPIC -shared -I$W/inc -I$C -I$P/src -I/usr/include/SDL2 $P/lua/pico.c \
    $P/src/{colors,geom,get-set,input,layer,mem,output,pico,video}.c \
    -lSDL2 -lSDL2_image -lSDL2_mixer -lSDL2_ttf -lSDL2_gfx -lm -o $W/ours/pico_native.so
lua5.4 $C/tools/lua2atm.lua $P/lua/pico/init.lua > $W/ours/pico.atm
O=$W/ours/atmos/env/pico.atm
lua5.4 $C/tools/lua2atm.lua $H/ref/atmos/env/pico/init.lua | sed 's/emit_(/emit(/g' > $O
# Ubuntu's lua5.4 (the reference) is built with the 5.3 compat math.pow
sed -i "s|^val pico = require(\"pico\")\$|&\nset math.pow = math.pow \|\| (func (x, y) { x ** y })|" $O
echo "ready: $W"
