#!/usr/bin/env bash
set -u
EV=/tmp/opencode/x11-slow-ab
APP=/home/archerc/code/lsfg-vk/build/lsfg-vk-app/lsfg-vk-app
CONF=$HOME/.config/lsfg-vk/conf.toml

run_one() {
  local tag=$1; shift
  local out=$EV/$tag
  mkdir -p $out
  : > $out/app.log; : > $out/game.log
  env "$@" LSFGVK_CONFIG=$CONF XAUTHORITY=/run/user/1000/xauth_OKuvCG \
    nohup $APP --profile app-oneway --session x11 -v > $out/app.log 2>&1 &
  local apid=$!; echo $apid > $out/app.pid
  sleep 3
  VK_LAYER_PATH=/tmp/opencode/layer-test \
  VK_INSTANCE_LAYERS=VK_LAYER_LSFGVK_frame_generation \
  LSFGVK_CONFIG=$CONF \
  MESA_VK_DEVICE_SELECT=1002:7550 \
  nohup vkcube --present_mode fifo --wsi xcb --suppress_popups > $out/game.log 2>&1 &
  local gpid=$!; echo $gpid > $out/game.pid
  sleep 35
  kill -INT $apid; sleep 2
  kill $gpid 2>/dev/null; sleep 1
  echo "== $tag =="
  grep -c '\[gen x 1 + real\]' $out/app.log | xargs echo "   cycles:"
  grep 'fps game' $out/app.log | tail -6 | sed 's/^/   /'
  grep -m1 'lsfg-vk-app: ' $out/app.log | head -1 > /dev/null
  kill -0 $apid 2>/dev/null && kill -9 $apid
}

run_one x11-fs
run_one x11-nofs LSFGVK_APP_NO_FS=1
