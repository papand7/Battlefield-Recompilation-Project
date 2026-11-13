#!/bin/sh
# Battlefield 1943 dedicated server on Linux (bf1943_server, see docs/DEVELOPERS.md):
# hosts on your network (UDP 1003, browsers on UDP 19430).
# Edit the name, maps and bots below. Maps: Wake_island_s Iwo_Jima_s Guadal_Canal Coral_sea
GAME=original/v106
NAME="My Server"
MAPS=Wake_island_s,Iwo_Jima_s,Guadal_Canal,Coral_sea
BOTS=0
# PUBLISH=1: also listed on the internet (the master server), not only on your network;
# your router must pass UDP ports 1003 and 19430 to this machine.
PUBLISH=0
# BOT_LLM=1: in difficult spots (stuck, losing a fight) the bots ask a small vision model
# (Qwen3.5 4B through llama.cpp, run on this machine) where to go. Installed into llm/ the
# first time (about 3.5 GB). Faster with a graphics card (Vulkan).
BOT_LLM=0
# TRAIN=1: a training run instead: nobody can join, 16 bots, the game 4x as fast, the bots
# ask about more (with BOT_LLM=1) and learn; it ends after TRAIN_MINUTES (real time) and
# writes a viewer of the whole run: server_data/bot_advice/runs/<map>_<date>/index.html
TRAIN=0
TRAIN_MINUTES=30
EXTRA=
[ "$PUBLISH" = "1" ] && EXTRA=--publish
[ "$TRAIN" = "1" ] && EXTRA="--train --speed 4 --train-minutes $TRAIN_MINUTES"
cd "$(dirname "$0")" || exit 1
if [ "$BOT_LLM" = "1" ]; then
  if [ -f llm/bot_llm.ini ] || python3 tools/setup_bot_llm.py --dir llm; then EXTRA="$EXTRA --bot-llm llm"; fi
fi
exec build/linux/bf1943_server --game "$GAME" --user server_data --dedicated --name "$NAME" --maps "$MAPS" --bots "$BOTS" $EXTRA
