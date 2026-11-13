#!/bin/sh
# The dedicated server's window (tools/server_gui.py): name, maps, bots, internet,
# the bots' advisor, training runs; START / STOP, its log, the training replay.
# Needs python3 with tkinter (Debian/Ubuntu: sudo apt install python3-tk).
cd "$(dirname "$0")" || exit 1
exec python3 tools/server_gui.py "$@"
