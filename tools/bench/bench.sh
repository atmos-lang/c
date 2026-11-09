#!/bin/bash
# bench.sh <ref|ours> <rocks|birds|snkrx> <frames> <draw 1|0> [shot-prefix]
# prints one 'BENCH ...' line (see the benchmark env for the fields)
H=$(cd "$(dirname "$0")" && pwd)
C=$(cd "$H/../.." && pwd)
W=${W:-$HOME/atmos-bench}
case $2 in
  rocks) dir=$W/games/pico-rocks; main=main.atm ;;
  birds) dir=$W/games/pico-birds; main=birds-11.atm ;;
  snkrx) dir=$W/games/snkrx/atmos; main=main.atm ;;
esac
cd $dir
export BENCH_GAME=$2 BENCH_FRAMES=$3 BENCH_DRAW=$4 BENCH_SHOT=$5
export SDL_AUDIODRIVER=dummy SDL_RENDER_DRIVER=software
if [ $1 = ref ]; then
  T=$W/refdev
  exec xvfb-run -a -s "-screen 0 1280x720x24" /usr/bin/lua5.4 -e "package.path='$H/ref/?.lua;$H/ref/?/init.lua;$T/share/lua/5.4/?.lua;$T/share/lua/5.4/?/init.lua;/usr/local/share/lua/5.4/?.lua;/usr/local/share/lua/5.4/?/init.lua;'..package.path;package.cpath='/usr/local/lib/lua/5.4/?.so;'..package.cpath" $T/lib/luarocks/rocks-5.4/atmos-lang/dev-4/bin/atmos $main
else
  export LUA_PATH_5_5="$W/ours/?.atm;$W/ours/?/init.atm;./?.atm;./?/init.atm"
  export LUA_CPATH_5_5="$W/ours/?.so"
  exec xvfb-run -a -s "-screen 0 1280x720x24" $C/lua $main
fi
