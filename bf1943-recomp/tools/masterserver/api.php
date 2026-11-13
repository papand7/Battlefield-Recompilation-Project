<?php
// Master server list for several games: one API file for plain cPanel PHP hosting (no terminal, no database); the
// website is index.html beside it. Upload this folder to the site's root (public_html of masterserver.dankelsoft.dk),
// keep it writable (the default on cPanel) and open  api.php?a=selftest  once.
//
//   api.php?a=games                              the games it lists, with their server and player counts.
//   api.php?a=list&game=nightfire                that game's servers heard from in the last 90 s (JSON; format=lines:
//                                                one tab-separated line each, for the game).
//   api.php?a=announce&game=nightfire&port=27787 a host says "I am here" (the game does it every 30 s while its lobby
//        [&mode=Deathmatch&max=8&v=1.5.13]       is open). The master sends the game's own UDP query to the caller's
//                                                address and lists only what the host answers itself (name, map,
//                                                players): nobody can list an unreachable server or lie about its name,
//                                                map or players. mode and max are the host's word (the game's query has
//                                                no field for them yet).
//   api.php?a=remove&game=nightfire&port=27787   a host leaving.
//   api.php?a=selftest                           checks this hosting can write its file and send UDP.
//
// A new game: one entry in GAMES below (name, colour, a function that asks a server). NightFire's query is its LAN
// discovery packet (src/game/lan_play.cpp): a 12-byte header "NFL2" (1.5.13 and before "NFL1"), type 9; the answer
// is type 10 with host[24],
// map[24], players, pad, port (u16): 64 bytes.

const KEEP = 90;          // seconds a server stays listed after its last announce
const MAX_PER_IP = 3;     // servers one address may list
const MAX_TOTAL = 500;
const MIN_GAP = 10;       // seconds between announces for one address and port
const QUERY_WAIT = 1.5;   // seconds to wait for a host's answer

// "who hosts?" packets to ip:port (each its own datagram); the first answer for which $parse returns an array, or null
function ask_udp($ip, $port, array $packets, $parse) {
    $s = @stream_socket_client("udp://$ip:$port", $errno, $errstr, 2);
    if (!$s) return null;
    stream_set_timeout($s, 0, (int)(QUERY_WAIT * 1000000));
    foreach ($packets as $packet) fwrite($s, $packet);
    $end = microtime(true) + QUERY_WAIT;
    $info = null;
    while (microtime(true) < $end) {
        $buf = fread($s, 1500);
        if ($buf === false || $buf === '') break;
        if ($info = $parse($buf)) break;
    }
    fclose($s);
    return $info;
}
function text($t) { return preg_replace('/[^\x20-\x7E]/', '', rtrim((string)$t, "\0")); }

// the query in the protocol 1.5.14 speaks ("NFL2"), then the one before ("NFL1"): a host answers only its own
function ask_nightfire($ip, $port) {
    $q = fn($magic) => pack('a4CCCCV', $magic, 9, 0, 0, 0, time() & 0xFFFFFFFF);
    return ask_udp($ip, $port, [$q('NFL2'), $q('NFL1')], function ($buf) {
        $magic = substr($buf, 0, 4);
        if (strlen($buf) < 64 || ($magic !== 'NFL2' && $magic !== 'NFL1') || ord($buf[4]) !== 10) return null;
        $d = unpack('a24host/a24map/Cplayers/Cpad/vport', substr($buf, 12, 52));
        return ['name' => text($d['host']), 'map' => text($d['map']), 'players' => $d['players']];
    });
}

// Battlefield 1943 and Bad Company PC (bf1943-recomp runtime/lan.cpp): the LAN browser's beacon answers
// "<tag>?" on its UDP port with "<tag>!host|map|port[|players|max]" (tag BF43 on 19430, BFBC on 19431). The
// beacon is asked on its own port; the answer must name the announced game port.
function ask_bf_beacon($ip, $port, $tag, $beacon_port) {
    return ask_udp($ip, $beacon_port, [$tag . '?'], function ($buf) use ($tag, $port) {
        if (strlen($buf) < 6 || substr($buf, 0, 5) !== $tag . '!') return null;
        $f = explode('|', substr($buf, 5));
        if (count($f) < 3 || (int)$f[2] !== $port) return null;
        return ['name' => text($f[0]), 'map' => text(str_replace('_', ' ', $f[1])),
                'players' => isset($f[3]) ? max(0, min(64, (int)$f[3])) : 0];
    });
}
function ask_bf1943($ip, $port) { return ask_bf_beacon($ip, $port, 'BF43', 19430); }
function ask_bfbc1($ip, $port) { return ask_bf_beacon($ip, $port, 'BFBC', 19431); }

// id => the game's name, its accent colour on the website, and the function asking one of its servers
const GAMES = [
    'nightfire' => ['name' => '007: NightFire', 'color' => '#3aa0ff', 'ask' => 'ask_nightfire'],
    'bf1943' => ['name' => 'Battlefield 1943', 'color' => '#d9a441', 'ask' => 'ask_bf1943'],
    'bfbc1' => ['name' => 'Battlefield: Bad Company', 'color' => '#8fb04a', 'ask' => 'ask_bfbc1'],
];

header('Content-Type: application/json');
header('Access-Control-Allow-Origin: *');
header('Cache-Control: no-store');

function done($data, $code = 200) {
    http_response_code($code);
    echo json_encode($data, JSON_UNESCAPED_SLASHES);
    exit;
}

$file = __DIR__ . '/servers.json';

// the stored servers, locked while one request changes them; $change(&$servers, $now) may return a value
function with_servers($file, $change) {
    $f = fopen($file, 'c+');
    if (!$f) done(['ok' => false, 'error' => 'the master cannot write its list'], 500);
    flock($f, LOCK_EX);
    $raw = stream_get_contents($f);
    $servers = $raw ? json_decode($raw, true) : [];
    if (!is_array($servers)) $servers = [];
    $now = time();
    $servers = array_values(array_filter($servers, fn($s) => $now - $s['seen'] <= KEEP));
    $result = $change($servers, $now);
    ftruncate($f, 0);
    rewind($f);
    fwrite($f, json_encode($servers));
    flock($f, LOCK_UN);
    fclose($f);
    return $result;
}

$ip = $_SERVER['REMOTE_ADDR'] ?? '';
$action = $_GET['a'] ?? 'list';
$game = preg_replace('/[^a-z0-9_]/', '', strtolower($_GET['game'] ?? 'nightfire'));
$of_game = fn($s) => $s['game'] === $game;

if ($action === 'games') {
    $all = with_servers($file, fn(&$servers) => $servers);
    $out = [];
    foreach (GAMES as $id => $g) {
        $mine = array_filter($all, fn($s) => $s['game'] === $id);
        $out[] = ['id' => $id, 'name' => $g['name'], 'color' => $g['color'], 'servers' => count($mine),
                  'players' => array_sum(array_column($mine, 'players'))];
    }
    done(['ok' => true, 'games' => $out]);
}

if ($action === 'list') {
    if (!isset(GAMES[$game])) done(['ok' => false, 'error' => 'unknown game'], 400);
    $out = with_servers($file, fn(&$servers) => array_values(array_map(fn($s) => [
        'address' => $s['ip'], 'port' => $s['port'], 'name' => $s['name'], 'map' => $s['map'], 'players' => $s['players'],
        'max' => $s['max'], 'mode' => $s['mode'], 'version' => $s['version'],
    ], array_filter($servers, $of_game))));
    if (($_GET['format'] ?? '') === 'lines') {  // for the game: one server a line, tab-separated, no JSON to parse
        header('Content-Type: text/plain; charset=utf-8');
        foreach ($out as $s)
            echo implode("	", array_map(fn($v) => str_replace(["	", "
", ""], ' ', (string)$v),
                [$s['address'], $s['port'], $s['name'], $s['map'], $s['players'], $s['max'], $s['mode'], $s['version']])), "
";
        exit;
    }
    done(['ok' => true, 'game' => $game, 'servers' => $out]);
}

if ($action === 'announce' || $action === 'remove') {
    if (!isset(GAMES[$game])) done(['ok' => false, 'error' => 'unknown game'], 400);
    if (!filter_var($ip, FILTER_VALIDATE_IP, FILTER_FLAG_IPV4))
        done(['ok' => false, 'error' => 'IPv4 only for now'], 400);
    $port = (int)($_GET['port'] ?? $_POST['port'] ?? 0);
    if ($port < 1000 || $port > 65535) done(['ok' => false, 'error' => 'bad port'], 400);  // (Battlefield: 1003)
    $mine = fn($s) => $s['game'] === $game && $s['ip'] === $ip && $s['port'] === $port;

    if ($action === 'remove') {
        with_servers($file, function (&$servers) use ($mine) {
            $servers = array_values(array_filter($servers, fn($s) => !$mine($s)));
        });
        done(['ok' => true]);
    }

    $version = substr(preg_replace('/[^0-9A-Za-z.\-]/', '', $_GET['v'] ?? ''), 0, 16);
    $mode = substr(text($_GET['mode'] ?? ''), 0, 24);
    $max = max(0, min(64, (int)($_GET['max'] ?? 0)));

    // too soon after the last one from here, or too many from this address
    $check = with_servers($file, function (&$servers, $now) use ($ip, $mine) {
        $count = 0;
        $known = false;
        foreach ($servers as $s) {
            if ($s['ip'] === $ip) $count++;
            if ($mine($s)) {
                $known = true;
                if ($now - $s['seen'] < MIN_GAP) return 'wait';
            }
        }
        if (!$known && $count >= MAX_PER_IP) return 'many';
        if (!$known && count($servers) >= MAX_TOTAL) return 'full';
        return 'ok';
    });
    if ($check === 'wait') done(['ok' => true, 'listed' => true, 'note' => 'announced a moment ago']);
    if ($check !== 'ok') done(['ok' => false, 'error' => 'too many servers listed from this address'], 429);

    $info = (GAMES[$game]['ask'])($ip, $port);  // outside the lock: it waits
    if (!$info) {
        $also = $game === 'bf1943' ? ' and 19430' : ($game === 'bfbc1' ? ' and 19431' : '');
        done(['ok' => false, 'listed' => false,
              'error' => "no answer from $ip:$port: start the lobby, and open UDP port $port$also in the firewall and router"]);
    }
    $entry = $info + ['game' => $game, 'ip' => $ip, 'port' => $port, 'version' => $version, 'mode' => $mode, 'max' => $max];
    with_servers($file, function (&$servers, $now) use ($mine, $entry) {
        $entry['seen'] = $now;
        foreach ($servers as $i => $s)
            if ($mine($s)) {
                $servers[$i] = $entry;
                return;
            }
        $servers[] = $entry;
    });
    done(['ok' => true, 'listed' => true, 'name' => $info['name'], 'map' => $info['map'], 'players' => $info['players']]);
}

if ($action === 'selftest') {
    $write = is_writable(__DIR__);
    $udp = @stream_socket_client('udp://8.8.8.8:53', $e, $es, 2) !== false;
    done(['ok' => $write && $udp, 'can_write_list' => $write, 'can_send_udp' => $udp, 'php' => PHP_VERSION,
          'note' => $udp ? '' : 'UDP out is blocked on this hosting: ask the host to allow it']);
}

done(['ok' => false, 'error' => 'unknown action'], 400);
