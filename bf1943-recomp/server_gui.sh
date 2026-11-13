#!/bin/sh
# The dedicated server's window: the launcher, opened on its SERVER page (name, maps,
# bots, internet, the bots' advisor, training runs; START / STOP, its log, the replay).
# A server without a window: start_server.sh.
cd "$(dirname "$0")" || exit 1
# The AppImage next to BattlefieldRecomp/ (the newest), or a release folder's Launcher/.
app=$(ls -t ../../BattlefieldLauncher-*.AppImage 2>/dev/null | head -n 1)
[ -n "$app" ] && exec "$app" --server "$@"
[ -x ../Launcher/battlefield-launcher ] && exec ../Launcher/battlefield-launcher --server "$@"
# From the sources: Electron (npm install in launcher/ first).
[ -x launcher/node_modules/electron/dist/electron ] && exec launcher/node_modules/electron/dist/electron launcher --server "$@"
echo 'The launcher was not found: run "npm install" in launcher/ (from the sources), or use' >&2
echo "the launcher's SERVER page. start_server.sh runs a server without a window." >&2
exit 1
