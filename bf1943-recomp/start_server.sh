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
EXTRA=
[ "$PUBLISH" = "1" ] && EXTRA=--publish
exec build/linux/bf1943_server --game "$GAME" --user server_data --dedicated --name "$NAME" --maps "$MAPS" --bots "$BOTS" $EXTRA
