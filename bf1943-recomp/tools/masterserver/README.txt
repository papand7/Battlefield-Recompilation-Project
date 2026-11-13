Master server (masterserver.dankelsoft.dk): 007 NightFire, Battlefield 1943 and Bad Company PC

Upload the two files to the ROOT of the site (public_html), keeping them together:
  api.php    the server list API (announce, list, remove, games)
  index.html the website

The folder must be writable by PHP (the cPanel default). servers.json is created by the API.
Check it works: open https://masterserver.dankelsoft.dk/api.php?a=selftest
It should say "ok": true. If can_send_udp is false, the host blocks outgoing UDP.
