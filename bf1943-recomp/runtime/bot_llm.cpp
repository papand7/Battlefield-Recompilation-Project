// The bots' advisor: see bot_llm.h. llama-server (llama.cpp) on 127.0.0.1,
// started from <folder>/bot_llm.ini, or one already running; asked over its
// OpenAI-style HTTP API (/v1/chat/completions) with the map as a PNG, the
// answer held to a JSON schema ({"why": ..., "place": 0..n}).
#include "kernel.h"

#include "bot_llm.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include "posix_net.h"
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../thirdparty/xenia/third_party/stb/stb_image_write.h"

namespace rt::botllm {
namespace {

constexpr int kPort = 8794;  // (one server for every process on this machine)
constexpr int kSize = 256;   // the map image, pixels each way

std::string g_host = "127.0.0.1";
int g_port = kPort;
std::atomic<bool> g_enabled{false}, g_ready{false};

int64_t Now() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- HTTP (plain, one request per connection) --------------------------------
bool Http(const char* method, const std::string& path, const std::string& body, std::string* out, int timeout_ms) {
  static std::once_flag wsa;
  std::call_once(wsa, [] {
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
  });
  addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(g_host.c_str(), std::to_string(g_port).c_str(), &hints, &res) != 0 || !res) return false;
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return freeaddrinfo(res), false;
#ifdef _WIN32
  DWORD tv = DWORD(timeout_ms);
#else
  timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
#endif
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
  bool ok = connect(s, res->ai_addr, int(res->ai_addrlen)) == 0;
  freeaddrinfo(res);
  std::string req = std::string(method) + " " + path + " HTTP/1.1\r\nHost: " + g_host + "\r\nConnection: close\r\n";
  if (!body.empty()) req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
  req += "\r\n" + body;
  for (size_t sent = 0; ok && sent < req.size();) {
    int n = send(s, req.data() + sent, int(std::min<size_t>(req.size() - sent, 1 << 16)), 0);
    if (n <= 0) ok = false;
    else sent += size_t(n);
  }
  std::string got;
  char buf[16384];
  for (int n; ok && (n = recv(s, buf, sizeof(buf), 0)) > 0;) got.append(buf, size_t(n));
  closesocket(s);
  size_t head = got.find("\r\n\r\n");
  if (!ok || head == std::string::npos || got.compare(0, 9, "HTTP/1.1 ") != 0) return false;
  int status = atoi(got.c_str() + 9);
  std::string headers = got.substr(0, head), data = got.substr(head + 4);
  std::transform(headers.begin(), headers.end(), headers.begin(), [](char c) { return char(tolower(c)); });
  if (headers.find("transfer-encoding: chunked") != std::string::npos) {
    std::string plain;
    for (size_t p = 0; p < data.size();) {
      size_t eol = data.find("\r\n", p);
      if (eol == std::string::npos) break;
      size_t len = strtoul(data.c_str() + p, nullptr, 16);
      if (!len) break;
      plain.append(data, eol + 2, len);
      p = eol + 2 + len + 2;
    }
    data.swap(plain);
  }
  if (out) *out = data;
  return status == 200;
}

// ---- JSON, as little as is needed -----------------------------------------------
std::string Esc(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') o += '\\', o += char(c);
    else if (c == '\n') o += "\\n";
    else if (c < 0x20) o += ' ';
    else o += char(c);
  }
  return o;
}
// The string value of the first "key" at or after `from`.
std::string JsonString(const std::string& j, const std::string& key, size_t from = 0) {
  size_t p = j.find("\"" + key + "\"", from);
  if (p == std::string::npos) return {};
  p = j.find(':', p);
  if (p == std::string::npos) return {};
  p = j.find_first_not_of(" \t\r\n", p + 1);
  if (p == std::string::npos || j[p] != '"') return {};
  std::string o;
  for (++p; p < j.size() && j[p] != '"'; ++p) {
    if (j[p] != '\\' || p + 1 >= j.size()) { o += j[p]; continue; }
    char e = j[++p];
    if (e == 'n') o += '\n';
    else if (e == 't') o += '\t';
    else if (e == 'u') o += '?', p += 4;
    else o += e;
  }
  return o;
}
bool JsonInt(const std::string& j, const std::string& key, int* v) {
  size_t p = j.find("\"" + key + "\"");
  if (p == std::string::npos || (p = j.find(':', p)) == std::string::npos) return false;
  char* end = nullptr;
  long n = strtol(j.c_str() + p + 1, &end, 10);
  if (end == j.c_str() + p + 1) return false;
  *v = int(n);
  return true;
}
std::string Base64(const std::vector<uint8_t>& d) {
  static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string o;
  o.reserve((d.size() + 2) / 3 * 4);
  for (size_t i = 0; i < d.size(); i += 3) {
    uint32_t v = uint32_t(d[i]) << 16 | (i + 1 < d.size() ? d[i + 1] << 8 : 0) | (i + 2 < d.size() ? d[i + 2] : 0);
    o += k[v >> 18 & 63], o += k[v >> 12 & 63];
    o += i + 1 < d.size() ? k[v >> 6 & 63] : '=';
    o += i + 2 < d.size() ? k[v & 63] : '=';
  }
  return o;
}

// ---- llama-server ------------------------------------------------------------------
#ifdef _WIN32
HANDLE g_job = nullptr;  // (closes the server with this process)
HANDLE g_server_process = nullptr;  // the llama-server started (StopServer)
#else
pid_t g_server_pid = 0;
#endif

std::string Quote(const std::string& s) { return s.find(' ') == std::string::npos ? s : "\"" + s + "\""; }

// Starts it from <dir>/bot_llm.ini: server=, model=, mmproj= (paths relative
// to the folder), args= (more of llama-server's options).
bool StartServer(const std::filesystem::path& dir) {
  std::ifstream ini(dir / "bot_llm.ini");
  if (!ini) {
    RT_WARN("bot llm: no %s (run tools/setup_bot_llm.py)", (dir / "bot_llm.ini").string().c_str());
    return false;
  }
  std::map<std::string, std::string> kv;
  for (std::string line; std::getline(ini, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t eq = line.find('=');
    if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
    kv[line.substr(0, eq)] = line.substr(eq + 1);
  }
  auto file = [&](const char* k) {
    std::filesystem::path p = kv[k];
    return p.empty() ? p : p.is_absolute() ? p : dir / p;
  };
  std::filesystem::path server = file("server"), model = file("model"), mmproj = file("mmproj");
  std::error_code ec;
  if (server.empty() || !std::filesystem::exists(server, ec) || model.empty() || !std::filesystem::exists(model, ec)) {
    RT_WARN("bot llm: %s names a server or model that is not there", (dir / "bot_llm.ini").string().c_str());
    return false;
  }
  // Half the cores at most: the game's server runs beside it.
  int threads = std::max(1, int(std::thread::hardware_concurrency()) / 2 - 1);
  std::vector<std::string> args = {server.string(), "-m", model.string(), "--host", "127.0.0.1", "--port",
                                   std::to_string(g_port), "-c", "8192", "-np", "2", "-t", std::to_string(threads),
                                   "--no-webui"};
  if (!mmproj.empty()) args.insert(args.end(), {"--mmproj", mmproj.string()});
  for (size_t p = 0; p < kv["args"].size();) {
    size_t q = kv["args"].find(' ', p);
    if (q == std::string::npos) q = kv["args"].size();
    if (q > p) args.push_back(kv["args"].substr(p, q - p));
    p = q + 1;
  }
  std::string log = (dir / "llama-server.log").string();
  RT_INFO("bot llm: starting %s (%s), log %s", server.filename().string().c_str(), model.filename().string().c_str(),
          log.c_str());
#ifdef _WIN32
  std::string cmd;
  for (const std::string& a : args) cmd += (cmd.empty() ? "" : " ") + Quote(a);
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE out = ::CreateFileA(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
  STARTUPINFOA si{sizeof(si)};
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = si.hStdError = out;
  si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION pi{};
  std::vector<char> line(cmd.begin(), cmd.end());
  line.push_back(0);
  bool ok = ::CreateProcessA(nullptr, line.data(), nullptr, nullptr, TRUE,
                           // (training: as the game's; else behind it)
                           CREATE_NO_WINDOW | (g_training ? NORMAL_PRIORITY_CLASS : BELOW_NORMAL_PRIORITY_CLASS) |
                               CREATE_SUSPENDED,
                           nullptr,
                           server.parent_path().string().c_str(), &si, &pi);
  if (out != INVALID_HANDLE_VALUE) ::CloseHandle(out);
  if (!ok) {
    RT_WARN("bot llm: could not start %s (%lu)", server.string().c_str(), GetLastError());
    return false;
  }
  g_job = CreateJobObjectA(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{};
  lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &lim, sizeof(lim));
  if (!AssignProcessToJobObject(g_job, pi.hProcess))
    RT_WARN("bot llm: llama-server is not tied to this process (%lu): StopServer ends it", GetLastError());
  ::ResumeThread(pi.hThread);
  ::CloseHandle(pi.hThread);
  g_server_process = pi.hProcess;
#else
  pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    prctl(PR_SET_PDEATHSIG, SIGKILL);  // (closes with this process)
    if (!g_training) setpriority(PRIO_PROCESS, 0, 10);  // (training: as the game's; else behind it)
    int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) dup2(fd, 1), dup2(fd, 2), close(fd);
    std::string lib = server.parent_path().string();  // (its libggml*.so beside it)
    const char* old = getenv("LD_LIBRARY_PATH");
    setenv("LD_LIBRARY_PATH", (lib + (old ? ":" + std::string(old) : "")).c_str(), 1);
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    execv(argv[0], argv.data());
    _exit(127);
  }
  g_server_pid = pid;
#endif
  return true;
}

// ---- the questions -------------------------------------------------------------------
struct Job {
  int bot;
  std::string prompt;
  std::vector<uint8_t> png;
  int places;
  int64_t at, id;
  std::string map;
  std::vector<uint8_t> eye_png;  // the eye view, if there is one
  int kind;                      // (the training run's record)
  float x, z;
  std::vector<std::array<float, 2>> places_at;  // place k + 1 at
};
struct BotState {
  bool busy = false, answered = false;
  int place = 0;
  std::string why;
  int64_t last = -1000000, id = 0;
};
std::mutex g_mutex;
std::condition_variable g_cv;
std::deque<Job> g_jobs;
std::map<int, BotState> g_bots;
std::string g_map;  // (under g_mutex)

// ---- what was learned: per map, cases that worked or didn't ------------------------
struct Learned {
  int kind;
  float at[3], dir, place[3];
  int wins, losses;
};
std::mutex g_learn_mutex;
std::string g_learn_map;
std::vector<Learned> g_learned;

std::filesystem::path AdviceDir() { return g_user_root / "bot_advice"; }
void SaveLearned() {  // (g_learn_mutex held)
  std::error_code ec;
  std::filesystem::create_directories(AdviceDir(), ec);
  std::filesystem::path path = AdviceDir() / (g_learn_map + ".txt"), tmp = path;
  tmp += ".tmp";
  if (FILE* f = fopen(tmp.string().c_str(), "w")) {
    fprintf(f, "# kind x y z dir px py pz wins losses (bot_llm.h)\n");
    for (const Learned& l : g_learned)
      fprintf(f, "%d %.2f %.2f %.2f %.3f %.2f %.2f %.2f %d %d\n", l.kind, l.at[0], l.at[1], l.at[2], l.dir, l.place[0],
              l.place[1], l.place[2], l.wins, l.losses);
    fclose(f);
    std::filesystem::rename(tmp, path, ec);
  }
}
float AngleOff(float a, float b) {
  float d = std::fmod(std::fabs(a - b), 6.2831853f);
  return d > 3.14159265f ? 6.2831853f - d : d;
}

// ---- the training run's record (--train) -----------------------------------------------
struct RunCase {
  int64_t id;
  int bot, kind, place;
  double t;
  float x, z, px, pz;
  bool picked, eye;
  std::string prompt, why;
  int worked = -1;
};
struct RunLearned {
  double t;
  int kind;
  float x, z;
  bool worked;
};
std::mutex g_run_mutex;
std::filesystem::path g_run_dir;  // empty: no run
std::string g_run_map;
double g_run_t0 = 0;
float g_run_bounds[3] = {0, 0, 1};
std::map<int, std::pair<int, std::vector<std::array<float, 3>>>> g_tracks;  // bot: team, (t x z)
std::vector<RunCase> g_run_cases;
std::vector<RunLearned> g_run_learned;
struct RunShot { float t, fx, fz, tx, tz; int bot; bool hit; };
struct RunKill { float t, x, z; int team, killer; };
std::vector<RunShot> g_run_shots;
std::vector<RunKill> g_run_kills;

const char* kViewer = R"VIEWER(<!DOCTYPE html><html><head><meta charset="utf-8"><title>Bot training</title>
<style>
body{margin:0;font:13px sans-serif;background:#16181c;color:#ddd;display:flex;height:100vh}
#left{flex:1;position:relative;overflow:hidden}
canvas{position:absolute;top:0}
#bar{position:absolute;left:0;right:0;bottom:0;background:#000b;padding:6px;display:flex;gap:8px;align-items:center}
#bar input[type=range]{flex:1}
#side{width:400px;overflow:auto;background:#1f2228;padding:8px}
.case{padding:3px 4px;border-bottom:1px solid #333;cursor:pointer}.case:hover{background:#2a2e36}
.ok{color:#5d5}.bad{color:#f66}.unk{color:#999}
#detail img{width:100%;image-rendering:pixelated}
pre{white-space:pre-wrap;font-size:11px;color:#aaa}
</style></head><body>
<div id="left"><canvas id="c"></canvas>
<div id="bar"><button id="play">Play</button><select id="spd"><option>1</option><option selected>10</option><option>30</option><option>100</option></select>x
<input type="range" id="t" min="0" max="1" value="1"><span id="clock"></span></div></div>
<div id="side"><div id="sum"></div><div id="detail"></div><div id="list"></div></div>
<script src="data.js"></script>
<script>
const R=RUN,cv=document.getElementById('c'),g=cv.getContext('2d'),img=new Image();img.src=R.image;
const K={1:['S','stuck'],2:['C','cover'],3:['V','vehicle stuck'],4:['I','idle'],5:['F','where to fight']};
let T=0,end=1,playing=false,sel=null;
for(const b of R.bots)if(b.track.length)end=Math.max(end,b.track[b.track.length-1][0]);
for(const c of R.cases)end=Math.max(end,c.t);
const tr=document.getElementById('t');tr.max=Math.ceil(end);tr.value=tr.max;T=end;
const fmt=s=>{s=Math.floor(s);return Math.floor(s/60)+':'+String(s%60).padStart(2,'0');};
const esc=s=>(s||'').replace(/[&<>]/g,m=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[m]));
const kind=c=>K[c.kind]?K[c.kind][1]:'';
// The view (wheel: zoom, drag: pan): at first, where the bots went.
const V={cx:R.bounds.cx,cz:R.bounds.cz,half:R.bounds.half};
{let x0=1e9,x1=-1e9,z0=1e9,z1=-1e9;for(const b of R.bots)for(const p of b.track){x0=Math.min(x0,p[1]);x1=Math.max(x1,p[1]);z0=Math.min(z0,p[2]);z1=Math.max(z1,p[2]);}
 if(x1>x0){V.cx=(x0+x1)/2;V.cz=(z0+z1)/2;V.half=Math.max(x1-x0,z1-z0)/2+40;}}
function P(x,z){const s=cv.width;return[(x-(V.cx-V.half))/(2*V.half)*s,((V.cz+V.half)-z)/(2*V.half)*s];}
function W(px,py){const s=cv.width;return[V.cx-V.half+px/s*2*V.half,V.cz+V.half-py/s*2*V.half];}
cv.onwheel=e=>{e.preventDefault();const r=cv.getBoundingClientRect(),[x,z]=W(e.clientX-r.left,e.clientY-r.top),f=e.deltaY>0?1.2:1/1.2;
 V.cx=x+(V.cx-x)*f;V.cz=z+(V.cz-z)*f;V.half*=f;draw();};
let drag=null;cv.onmousedown=e=>{drag={x:e.clientX,y:e.clientY,cx:V.cx,cz:V.cz,moved:false};};
window.onmousemove=e=>{if(!drag)return;const k=2*V.half/cv.width,dx=e.clientX-drag.x,dy=e.clientY-drag.y;if(Math.abs(dx)+Math.abs(dy)>3)drag.moved=true;
 V.cx=drag.cx-dx*k;V.cz=drag.cz+dy*k;draw();};
window.onmouseup=()=>{setTimeout(()=>drag=null,0);};
function col(team,i,a){const h=team==1?195+(i*13)%45:(i*11)%35;return`hsla(${h},90%,${55+(i%3)*8}%,${a})`;}
function fit(){const l=document.getElementById('left'),s=Math.min(l.clientWidth,l.clientHeight-36);cv.width=cv.height=s;cv.style.left=((l.clientWidth-s)/2)+'px';draw();}
function draw(){
 g.fillStyle='#0b0c0e';g.fillRect(0,0,cv.width,cv.height);
 if(img.complete&&img.width){const B=R.bounds,[a,b]=P(B.cx-B.half,B.cz+B.half),[c,d]=P(B.cx+B.half,B.cz-B.half);g.drawImage(img,a,b,c-a,d-b);}
 R.bots.forEach((b,i)=>{let last=null;g.lineWidth=1.5;
  for(const p of b.track){if(p[0]>T)break;
   if(last&&p[0]-last[0]<5){g.strokeStyle=col(b.team,i,Math.max(0.1,1-(T-p[0])/120));const[a,c]=P(last[1],last[2]),[d,e]=P(p[1],p[2]);g.beginPath();g.moveTo(a,c);g.lineTo(d,e);g.stroke();}
   last=p;}
  if(last&&T-last[0]<3){const[x,y]=P(last[1],last[2]);g.fillStyle=col(b.team,i,1);g.beginPath();g.arc(x,y,5,0,7);g.fill();g.fillStyle='#fff';g.font='10px sans-serif';g.fillText(b.id,x+6,y-6);}});
 const S=R.shots||[],X=R.kills||[],bi={};R.bots.forEach((b,i)=>bi[b.id]=[b.team,i]);
 for(const s of S){if(s[0]>T||T-s[0]>1.5)continue;const[tm,i]=bi[s[1]]||[1,0],[a,b]=P(s[2],s[3]),[c,d]=P(s[4],s[5]);
  g.globalAlpha=(s[6]?1:0.45)*(1-(T-s[0])/1.5);g.strokeStyle=s[6]?'#fff':col(tm,i,1);g.lineWidth=s[6]?2:1;
  g.beginPath();g.moveTo(a,b);g.lineTo(c,d);g.stroke();if(s[6]){g.fillStyle='#f33';g.beginPath();g.arc(c,d,3,0,7);g.fill();}g.globalAlpha=1;}
 for(const k of X){if(k[0]>T||T-k[0]>60)continue;const[x,y]=P(k[1],k[2]);g.globalAlpha=Math.max(0.15,1-(T-k[0])/60);
  g.strokeStyle=k[3]==1?'#6cf':'#f96';g.lineWidth=2;g.beginPath();g.moveTo(x-5,y-5);g.lineTo(x+5,y+5);g.moveTo(x+5,y-5);g.lineTo(x-5,y+5);g.stroke();
  if(k[4]>=0){g.fillStyle='#fff';g.font='9px sans-serif';g.fillText('by '+k[4],x+6,y+10);}g.globalAlpha=1;}
 for(const l of R.learned){if(l.t>T)continue;const[x,y]=P(l.x,l.z);g.fillStyle=l.worked?'#3c3':'#e33';g.globalAlpha=Math.max(0.2,1-(T-l.t)/300);g.fillRect(x-3,y-3,6,6);g.globalAlpha=1;}
 for(const c of R.cases){if(c.t>T)continue;const[x,y]=P(c.x,c.z),f=c.worked===true?'#3c3':c.worked===false?'#e33':'#bbb';
  g.globalAlpha=Math.max(0.25,1-(T-c.t)/300);
  if(c.px!==undefined){const[q,r]=P(c.px,c.pz);g.strokeStyle=f;g.setLineDash([3,3]);g.beginPath();g.moveTo(x,y);g.lineTo(q,r);g.stroke();g.setLineDash([]);}
  g.fillStyle=f;g.beginPath();g.arc(x,y,7,0,7);g.fill();g.fillStyle='#000';g.font='bold 9px sans-serif';g.fillText(K[c.kind]?K[c.kind][0]:'?',x-3,y+3);g.globalAlpha=1;
  if(sel===c){g.strokeStyle='#ff0';g.lineWidth=2;g.beginPath();g.arc(x,y,11,0,7);g.stroke();}}
 document.getElementById('clock').textContent=fmt(T)+' / '+fmt(end);}
// (a picture not in the run's folder: the server's cases folder, two up)
const fb="if(!this.dataset.f){this.dataset.f=1;this.src='../../'+this.getAttribute('src');}";
function show(c){sel=c;const w=c.worked===true?'<b class=ok>worked</b>':c.worked===false?'<b class=bad>failed</b>':'<span class=unk>not judged</span>';
 document.getElementById('detail').innerHTML=`<h3>Bot ${c.bot}: ${kind(c)} at ${fmt(c.t)}</h3><p>Answer: place ${c.place}: ${esc(c.why)}<br>Outcome: ${w}</p><p class=unk>The map it was shown:</p><img src="${c.image}" onerror="${fb}">${c.eye?`<p class=unk>Its own view ahead:</p><img src="${c.eye}" onerror="${fb}">`:''}<pre>${esc(c.prompt)}</pre><hr>`;
 T=c.t;tr.value=Math.floor(T);draw();}
const list=document.getElementById('list');
for(const c of R.cases){const e=document.createElement('div'),k=c.worked===true?'ok':c.worked===false?'bad':'unk';e.className='case';
 e.innerHTML=`<span class=${k}>&#9679;</span> ${fmt(c.t)} bot ${c.bot}, ${kind(c)}: place ${c.place}`;e.onclick=()=>show(c);list.appendChild(e);}
const ok=R.cases.filter(c=>c.worked===true).length,bad=R.cases.filter(c=>c.worked===false).length;
document.getElementById('sum').innerHTML=`<h2>${esc(R.map)}</h2><p>${R.bots.length} bots, ${fmt(end)} of game time at ${R.speed}x.<br>${R.cases.length} questions: <span class=ok>${ok} worked</span>, <span class=bad>${bad} failed</span>.<br>${R.learned.length} learned places reused (${R.learned.filter(l=>l.worked).length} worked).<br>${(R.shots||[]).length} shots shown (${(R.shots||[]).filter(s=>s[6]).length} hits), ${(R.kills||[]).length} killed.</p><p class=unk>Circles: questions (S stuck, C cover, V vehicle, I idle, F where to fight), the dashed line to the place picked. Squares: learned places reused. Green worked, red failed. Lines: shots (white with a red dot: a hit). X: a soldier killed (blue US, orange Japan; "by N": the bot that killed it).</p>`;
cv.onclick=e=>{if(drag&&drag.moved)return;const r=cv.getBoundingClientRect(),mx=e.clientX-r.left,my=e.clientY-r.top;let best=null,bd=12;
 for(const c of R.cases){if(c.t>T)continue;const[x,y]=P(c.x,c.z),d=Math.hypot(x-mx,y-my);if(d<bd){bd=d;best=c;}}if(best)show(best);};
tr.oninput=()=>{T=+tr.value;draw();};
document.getElementById('play').onclick=()=>{playing=!playing;document.getElementById('play').textContent=playing?'Pause':'Play';if(playing&&T>=end)T=0;};
let lt=performance.now();
function tick(now){if(playing){T=Math.min(end,T+(now-lt)/1000*+document.getElementById('spd').value);tr.value=Math.floor(T);
 if(T>=end){playing=false;document.getElementById('play').textContent='Play';}draw();}lt=now;requestAnimationFrame(tick);}
requestAnimationFrame(tick);img.onload=draw;window.onresize=fit;fit();
</script></body></html>
)VIEWER";

double RunTime() { return GuestSeconds() - g_run_t0; }

void WriteRun() {  // (g_run_mutex held)
  if (g_run_dir.empty()) return;
  std::string o = "const RUN = {\"map\":\"" + Esc(g_run_map) + "\",\"speed\":" + std::to_string(g_time_speed_target) +
                  ",\"image\":\"map.png\",\"bounds\":{\"cx\":" + std::to_string(g_run_bounds[0]) + ",\"cz\":" +
                  std::to_string(g_run_bounds[1]) + ",\"half\":" + std::to_string(g_run_bounds[2]) + "},\"bots\":[";
  char buf[96];
  bool first = true;
  for (auto& [bot, tr] : g_tracks) {
    o += std::string(first ? "" : ",") + "{\"id\":" + std::to_string(bot) + ",\"team\":" + std::to_string(tr.first) +
         ",\"track\":[";
    for (size_t i = 0; i < tr.second.size(); ++i) {
      snprintf(buf, sizeof(buf), "%s[%.1f,%.1f,%.1f]", i ? "," : "", tr.second[i][0], tr.second[i][1], tr.second[i][2]);
      o += buf;
    }
    o += "]}";
    first = false;
  }
  o += "],\"cases\":[";
  for (size_t i = 0; i < g_run_cases.size(); ++i) {
    const RunCase& c = g_run_cases[i];
    std::string id = std::to_string(c.id);
    snprintf(buf, sizeof(buf), "\"t\":%.1f,\"x\":%.1f,\"z\":%.1f", c.t, c.x, c.z);
    o += std::string(i ? "," : "") + "{\"id\":" + id + ",\"bot\":" + std::to_string(c.bot) + ",\"kind\":" +
         std::to_string(c.kind) + "," + buf + ",\"place\":" + std::to_string(c.place);
    if (c.picked) snprintf(buf, sizeof(buf), ",\"px\":%.1f,\"pz\":%.1f", c.px, c.pz), o += buf;
    o += ",\"worked\":" + std::string(c.worked < 0 ? "null" : c.worked ? "true" : "false") + ",\"image\":\"cases/" +
         id + ".png\"" + (c.eye ? ",\"eye\":\"cases/" + id + "_eye.png\"" : "") + ",\"why\":\"" + Esc(c.why) +
         "\",\"prompt\":\"" + Esc(c.prompt) + "\"}";
  }
  o += "],\"learned\":[";
  for (size_t i = 0; i < g_run_learned.size(); ++i) {
    const RunLearned& l = g_run_learned[i];
    snprintf(buf, sizeof(buf), "%s{\"t\":%.1f,\"kind\":%d,\"x\":%.1f,\"z\":%.1f,\"worked\":%s}", i ? "," : "", l.t, l.kind,
             l.x, l.z, l.worked ? "true" : "false");
    o += buf;
  }
  o += "],\"shots\":[";
  for (size_t i = 0; i < g_run_shots.size(); ++i) {
    const RunShot& s = g_run_shots[i];
    snprintf(buf, sizeof(buf), "%s[%.1f,%d,%.1f,%.1f,%.1f,%.1f,%d]", i ? "," : "", s.t, s.bot, s.fx, s.fz, s.tx, s.tz,
             s.hit ? 1 : 0);
    o += buf;
  }
  o += "],\"kills\":[";
  for (size_t i = 0; i < g_run_kills.size(); ++i) {
    const RunKill& k = g_run_kills[i];
    snprintf(buf, sizeof(buf), "%s[%.1f,%.1f,%.1f,%d,%d]", i ? "," : "", k.t, k.x, k.z, k.team, k.killer);
    o += buf;
  }
  o += "]};\n";
  std::filesystem::path tmp = g_run_dir / "data.js.tmp";
  std::ofstream(tmp, std::ios::binary) << o;
  std::error_code ec;
  std::filesystem::rename(tmp, g_run_dir / "data.js", ec);
}

// The data set: <user>/bot_advice/cases/<id>.png and cases.jsonl (a line
// each for the question and answer, and for what came of it).
std::atomic<int> g_cases_saved{0};
void AppendCase(const std::string& line) {
  std::error_code ec;
  std::filesystem::create_directories(AdviceDir() / "cases", ec);
  std::ofstream(AdviceDir() / "cases" / "cases.jsonl", std::ios::app) << line << "\n";
}

const char* kSystem =
    "You advise one soldier (a bot) in a multiplayer war game. You are shown a top-down map centred on it: "
    "north is up. Colours: green and brown ground (lighter is higher), dark grey where nobody can walk "
    "(buildings, walls, rocks, cliffs, deep water), white arrow: the bot and where it faces, blue dots: its "
    "teammates, red dots: enemies, yellow squares: objectives it is going for, orange squares: objectives the "
    "enemy holds, grey squares: vehicles, white circles with a number: places the bot can walk to. A second "
    "picture, when there is one, is the bot's own view ahead. "
    "Pick the place that best solves its situation, or 0 if none helps. Think like an experienced player: for "
    "cover, a place no enemy can see and away from them (dark grey between it and the enemies); never towards an "
    "enemy in the open; to get unstuck, a place round what blocks the way that leads on towards the objective.";

void Worker() {
  for (;;) {
    Job job;
    {
      std::unique_lock lock(g_mutex);
      g_cv.wait(lock, [] { return !g_jobs.empty(); });
      job = std::move(g_jobs.front());
      g_jobs.pop_front();
    }
    int place = 0;
    std::string why, reply;
    bool ok = false;
    if (Now() - job.at < 20000) {  // (a stale question: its moment is gone)
      std::string schema = "{\"type\":\"object\",\"properties\":{\"why\":{\"type\":\"string\",\"maxLength\":160},"
                           "\"place\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":" + std::to_string(job.places) +
                           "}},\"required\":[\"why\",\"place\"]}";
      std::string body = "{\"model\":\"bot\",\"temperature\":0.3,\"max_tokens\":200,"
                         "\"chat_template_kwargs\":{\"enable_thinking\":false},"
                         "\"response_format\":{\"type\":\"json_schema\",\"json_schema\":{\"name\":\"choice\",\"schema\":" +
                         schema + "}},\"messages\":[{\"role\":\"system\",\"content\":\"" + Esc(kSystem) +
                         "\"},{\"role\":\"user\",\"content\":[{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64," +
                         Base64(job.png) + "\"}}," +
                         (job.eye_png.empty() ? std::string() :
                          "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64," + Base64(job.eye_png) + "\"}},") +
                         "{\"type\":\"text\",\"text\":\"" + Esc(job.prompt) + "\"}]}]}";
      int64_t t0 = Now();
      ok = Http("POST", "/v1/chat/completions", body, &reply, 60000);
      size_t msg = reply.find("\"message\"");
      std::string content = ok && msg != std::string::npos ? JsonString(reply, "content", msg) : "";
      ok = ok && JsonInt(content, "place", &place);
      why = JsonString(content, "why");
      if (place < 0 || place > job.places) place = 0;
      if (!ok) RT_WARN("bot llm: bot %d: no answer (%s)", job.bot, reply.substr(0, 200).c_str());
      else RT_INFO("bot llm: bot %d: place %d in %.1f s (%s)", job.bot, place, (Now() - t0) / 1000.0, why.c_str());
      if (ok && g_cases_saved++ < 20000) {  // (the data set, up to 20000 cases a run)
        std::error_code ec;
        std::filesystem::create_directories(AdviceDir() / "cases", ec);
        std::string id = std::to_string(job.id);
        auto save = [&](const std::string& name, const std::vector<uint8_t>& png) {
          std::ofstream(AdviceDir() / "cases" / name, std::ios::binary)
              .write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
        };
        save(id + ".png", job.png);
        if (!job.eye_png.empty()) save(id + "_eye.png", job.eye_png);
        AppendCase("{\"id\":" + id + ",\"map\":\"" + Esc(job.map) + "\",\"image\":\"" + id + ".png\"," +
                   (job.eye_png.empty() ? std::string() : "\"eye_image\":\"" + id + "_eye.png\",") + "\"system\":\"" +
                   Esc(kSystem) + "\",\"prompt\":\"" + Esc(job.prompt) + "\",\"place\":" + std::to_string(place) +
                   ",\"why\":\"" + Esc(why) + "\"}");
      }
    }
    std::lock_guard lock(g_mutex);
    BotState& b = g_bots[job.bot];
    b.busy = false;
    if (ok) b.answered = true, b.place = place, b.why = why, b.id = job.id;
    if (ok && g_training) {
      std::lock_guard run_lock(g_run_mutex);
      if (!g_run_dir.empty() && job.map == g_run_map) {
        RunCase c{job.id, job.bot, job.kind, place, RunTime(), job.x, job.z, 0, 0, place >= 1, !job.eye_png.empty(),
                  job.prompt, why};
        if (c.picked) c.px = job.places_at[size_t(place - 1)][0], c.pz = job.places_at[size_t(place - 1)][1];
        g_run_cases.push_back(std::move(c));
        // Its pictures in the run's own folder too: the replay shows them from
        // there (a viewer may not reach ../../cases), and the folder can be
        // copied or zipped whole.
        std::error_code ec;
        std::filesystem::create_directories(g_run_dir / "cases", ec);
        std::string id = std::to_string(job.id);
        std::ofstream(g_run_dir / "cases" / (id + ".png"), std::ios::binary)
            .write(reinterpret_cast<const char*>(job.png.data()), std::streamsize(job.png.size()));
        if (!job.eye_png.empty())
          std::ofstream(g_run_dir / "cases" / (id + "_eye.png"), std::ios::binary)
              .write(reinterpret_cast<const char*>(job.eye_png.data()), std::streamsize(job.eye_png.size()));
      }
    }
  }
}

// ---- the map ---------------------------------------------------------------------------
struct Canvas {
  int n;  // pixels each way
  std::vector<uint8_t> px;
  explicit Canvas(int size) : n(size), px(size_t(size) * size * 3) {}
  void Put(int x, int y, uint32_t rgb, float a = 1.0f) {
    if (x < 0 || y < 0 || x >= n || y >= n) return;
    uint8_t* p = &px[(y * n + x) * 3];
    // (signed: a darker colour over a lighter pixel wrapped round as unsigned,
    // black or garbage where it should have shaded)
    for (int c = 0; c < 3; ++c) {
      int to = int(rgb >> (16 - 8 * c) & 255);
      p[c] = uint8_t(std::clamp(int(std::lround(p[c] + (to - int(p[c])) * a)), 0, 255));
    }
  }
  void Disc(float cx, float cy, float r, uint32_t rgb) {
    for (int y = int(cy - r - 1); y <= int(cy + r + 1); ++y)
      for (int x = int(cx - r - 1); x <= int(cx + r + 1); ++x) {
        float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy);
        if (d < r) Put(x, y, rgb, std::min(1.0f, r - d));
      }
  }
  void Box(float cx, float cy, float r, uint32_t rgb) {
    for (int y = int(cy - r); y <= int(cy + r); ++y)
      for (int x = int(cx - r); x <= int(cx + r); ++x) Put(x, y, rgb);
  }
  void Line(float x0, float y0, float x1, float y1, float w, uint32_t rgb) {
    int n = int(std::hypot(x1 - x0, y1 - y0) * 2) + 1;
    for (int i = 0; i <= n; ++i) Disc(x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, w, rgb);
  }
  void Digit(float cx, float cy, int d, uint32_t rgb) {  // 3x5, 2 px a dot
    static const char* k[10] = {"111101101101111", "010110010010111", "111001111100111", "111001111001111",
                                "101101111001001", "111100111001111", "111100111101111", "111001001001001",
                                "111101111101111", "111101111001111"};
    for (int r = 0; r < 5; ++r)
      for (int c = 0; c < 3; ++c)
        if (k[d % 10][r * 3 + c] == '1') Box(cx - 2.5f + c * 2, cy - 4.5f + r * 2, 0.5f, rgb);
  }
};

// size: pixels each way (the bots' questions 256; a whole level's, the
// training viewer's, more).
std::vector<uint8_t> Draw(const MapView& m, int n = kSize) {
  Canvas cv(n);
  const float mpp = 2 * m.half / n;  // metres a pixel
  auto toX = [&](float x) { return (x - (m.cx - m.half)) / mpp; };
  auto toY = [&](float z) { return ((m.cz + m.half) - z) / mpp; };  // (north up)
  // Ground: height-tinted, lit from the north-west.
  std::vector<float> h(n * n, NAN);
  if (m.ground)
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x) h[y * n + x] = m.ground(m.cx - m.half + (x + 0.5f) * mpp, m.cz + m.half - (y + 0.5f) * mpp);
  // Where soldiers walk: cells of 4 x 4 pixels, each asked once.
  const int kCell = 4, kCells = n / kCell;
  std::vector<char> walk;
  float sea = -1e9f;  // the sea's level: as low as the ground soldiers walk on goes (none known: no water drawn)
  if (m.walkable) {
    walk.assign(size_t(kCells) * kCells, 0);
    std::vector<float> walked;
    for (int cy = 0; cy < kCells; ++cy)
      for (int cx = 0; cx < kCells; ++cx) {
        float wx = m.cx - m.half + (cx + 0.5f) * kCell * mpp, wz = m.cz + m.half - (cy + 0.5f) * kCell * mpp;
        float gy = h[(cy * kCell + kCell / 2) * n + cx * kCell + kCell / 2];
        bool ok = std::isfinite(gy) && gy > -1e8f;
        walk[size_t(cy) * kCells + cx] = m.walkable(wx, ok ? gy : m.y, wz);
        if (ok && walk[size_t(cy) * kCells + cx]) walked.push_back(gy);
      }
    // (a whole level's only: its beaches go down to the water; the bots' own
    // small maps stay as the advisor's prompt describes them)
    if (n >= 1024 && walked.size() > 200) {
      std::nth_element(walked.begin(), walked.begin() + walked.size() / 100, walked.end());
      sea = walked[walked.size() / 100];
    }
  }
  // The structures (below), and the sea's surface among them: a flat
  // collision plane over the sea bed, the commonest height of what lies over
  // the ground (over a tenth of the picture). Its level is the sea's.
  std::vector<float> top;
  const bool structures = m.tops && m.tops(m.cx - m.half, m.cz + m.half, mpp, n, top);
  std::vector<float> waters;  // the water surfaces' levels
  if (structures) {
    std::map<int, int> heights;  // (half metres)
    for (size_t i = 0; i < top.size(); ++i)
      if (std::isfinite(top[i]) && (!std::isfinite(h[i]) || top[i] > h[i] + 1.0f)) ++heights[int(std::lround(top[i] * 2))];
    auto most = std::max_element(heights.begin(), heights.end(), [](auto& a, auto& b) { return a.second < b.second; });
    if (most != heights.end() && most->second > int(top.size() / 10)) waters.push_back(most->first / 2.0f);
    // Less of the picture (a 200th each) for rivers, lakes and harbours (Bad
    // Company's levels; Valley Run has two: the reservoir above its dam, the
    // river below), up to three: no roof is that large and flat.
    else if (n >= 1024) {
      std::vector<std::pair<int, int>> by(heights.begin(), heights.end());
      std::sort(by.begin(), by.end(), [](auto& a, auto& b) { return a.second > b.second; });
      for (size_t i = 0; i < by.size() && waters.size() < 3; ++i)
        if (by[i].second > int(top.size() / 200)) waters.push_back(by[i].first / 2.0f);
    }
    if (!waters.empty()) sea = *std::min_element(waters.begin(), waters.end());
    if (n >= 1024) {  // (the whole level's: what it found)
      std::vector<std::pair<int, int>> by(heights.begin(), heights.end());
      std::sort(by.begin(), by.end(), [](auto& a, auto& b) { return a.second > b.second; });
      std::string common;
      for (size_t i = 0; i < by.size() && i < 4; ++i)
        common += " " + std::to_string(by[i].first / 2.0f).substr(0, 6) + " m (" + std::to_string(by[i].second) + ")";
      std::string found;
      for (float w : waters) found += " " + std::to_string(w).substr(0, 6) + " m";
      RT_INFO("bot llm: the level's map: water%s; the commonest levels over the ground:%s",
              found.empty() ? " none" : found.c_str(), common.c_str());
    }
  }
  // The water surface at pixel i (its level), or NAN.
  auto water_at = [&](size_t i) {
    if (std::isfinite(top[i]))
      for (float w : waters)
        if (std::fabs(top[i] - w) < 0.6f) return w;
    return NAN;
  };
  auto is_water = [&](size_t i) { return std::isfinite(water_at(i)); };
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) {
      float v = h[y * n + x];
      if (!std::isfinite(v) || v < -1e8f) {
        cv.Put(x, y, 0x5A6E78);
        continue;
      }
      // Water: under a water surface, or (no surfaces found) below the sea's
      // level; lighter in the shallows.
      const float w = structures ? water_at(size_t(y) * n + x) : NAN;
      if ((std::isfinite(w) && v < w) || (waters.empty() && v < sea - 0.3f)) {
        float d = std::clamp(((std::isfinite(w) ? w : sea) - v) / 15.0f, 0.0f, 1.0f);
        cv.Put(x, y, uint32_t(70 - 40 * d) << 16 | uint32_t(140 - 70 * d) << 8 | uint32_t(170 - 60 * d));
        continue;
      }
      float t = std::clamp((v - m.y) / 30.0f * 0.5f + 0.5f, 0.0f, 1.0f);  // (30 m below to 30 m above it)
      float gx = x > 0 && x + 1 < n ? h[y * n + x + 1] - h[y * n + x - 1] : 0;
      float gy = y > 0 && y + 1 < n ? h[(y + 1) * n + x] - h[(y - 1) * n + x] : 0;
      float lit = std::isfinite(gx + gy) ? std::clamp(1.0f + (-gx - gy) / (4 * mpp) * 0.6f, 0.6f, 1.4f) : 1.0f;
      float r = (90 + 80 * t) * lit, g = (120 + 60 * t) * lit, b = (60 + 40 * t) * lit;
      cv.Put(x, y, uint32_t(std::min(r, 255.f)) << 16 | uint32_t(std::min(g, 255.f)) << 8 | uint32_t(std::min(b, 255.f)));
    }
  // Structures (buildings, walls, bunkers, rocks, ships): what stands over a
  // metre above the ground, roofs lighter the taller, lit as the ground, their
  // outlines dark. (Not the sea's surface: drawn as water above.)
  if (structures) {
    auto above = [&](int x, int y) {
      if (x < 0 || y < 0 || x >= n || y >= n) return -1.0f;
      float t = top[size_t(y) * n + x], g = h[size_t(y) * n + x];
      if (!std::isfinite(t) || is_water(size_t(y) * n + x)) return -1.0f;
      return std::isfinite(g) && g > -1e8f ? t - g : 99.0f;  // (off the terrain: ships, piers)
    };
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x) {
        float a = above(x, y);
        if (a < 1.0f) continue;
        const float* t = &top[size_t(y) * n + x];
        float gx = x > 0 && x + 1 < n ? t[1] - t[-1] : 0, gy = y > 0 && y + 1 < n ? t[n] - t[-n] : 0;
        float lit = std::isfinite(gx + gy) ? std::clamp(1.0f + (-gx - gy) / (4 * mpp) * 0.5f, 0.55f, 1.35f) : 1.0f;
        bool edge = false;
        for (auto [dx, dy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
          float b = above(x + dx, y + dy);
          if (b < 1.0f || std::fabs(top[size_t(y + dy) * n + x + dx] - *t) > 2.5f) edge = true;
        }
        float k = std::clamp(a / 15.0f, 0.0f, 1.0f) * 50 + 150;
        float r = k * lit, g = (k - 8) * lit, b = (k - 22) * lit;
        if (edge) r *= 0.45f, g *= 0.45f, b *= 0.45f;
        cv.Put(x, y, uint32_t(std::min(r, 255.f)) << 16 | uint32_t(std::min(g, 255.f)) << 8 | uint32_t(std::min(b, 255.f)));
      }
  }
  // Where nobody walks, darker (on land: the water shows it; with the
  // structures drawn, lighter: they show it already).
  if (!walk.empty()) {
    for (int cy = 0; cy < kCells; ++cy)
      for (int cx = 0; cx < kCells; ++cx) {
        if (walk[size_t(cy) * kCells + cx]) continue;
        float gy = h[(cy * kCell + kCell / 2) * n + cx * kCell + kCell / 2];
        if (std::isfinite(gy) && gy < sea - 0.3f) continue;
        for (int y = 0; y < kCell; ++y)
          for (int x = 0; x < kCell; ++x) cv.Put(cx * kCell + x, cy * kCell + y, 0x2A2A2A, structures ? 0.35f : 0.8f);
      }
  }
  // A 10 m scale bar, bottom left.
  cv.Line(6, n - 6, 6 + 10 / mpp, n - 6, 1, 0xFFFFFF);
  for (const Marker& k : m.markers) {
    float x = toX(k.x), y = toY(k.z);
    switch (k.kind) {
      case Marker::kFriend: cv.Disc(x, y, 3.5f, 0x103070), cv.Disc(x, y, 2.5f, 0x40A0FF); break;
      case Marker::kEnemy: cv.Disc(x, y, 4.5f, 0x400000), cv.Disc(x, y, 3.5f, 0xFF2020); break;
      case Marker::kObjective: cv.Box(x, y, 5, 0x000000), cv.Box(x, y, 4, 0xFFE020); break;
      case Marker::kObjectiveEnemy: cv.Box(x, y, 5, 0x000000), cv.Box(x, y, 4, 0xFF8000); break;
      case Marker::kVehicle: cv.Box(x, y, 4, 0x000000), cv.Box(x, y, 3, 0xA0A0A0); break;
      case Marker::kPlace: cv.Disc(x, y, 8.5f, 0x000000), cv.Disc(x, y, 7.5f, 0xFFFFFF), cv.Digit(x, y, k.label, 0x000000); break;
      case Marker::kSelf: break;
    }
  }
  for (const Marker& k : m.markers)  // (on top)
    if (k.kind == Marker::kSelf) {
      float x = toX(k.x), y = toY(k.z);
      cv.Line(x, y, x + 12 * std::sin(k.yaw), y - 12 * std::cos(k.yaw), 1.2f, 0xFFFFFF);
      cv.Disc(x, y, 5.5f, 0x000000), cv.Disc(x, y, 4.5f, 0xFFFFFF);
    }
  std::vector<uint8_t> png;
  stbi_write_png_to_func([](void* ctx, void* data, int size) {
    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
    v->insert(v->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
  }, &png, n, n, 3, cv.px.data(), n * 3);
  return png;
}

// The eye view: 48 x 16 rays over 90 degrees across and 34 up and down from
// the eye, each one's distance to what it hits (to 80 m) found by halving
// with sight; drawn 4 pixels a ray (192 x 64): near light, far dark, the sky
// blue, open ground beyond 80 m dark green. Teammates and enemies in sight
// in it: blue and red boxes, bigger nearer.
std::vector<uint8_t> DrawEye(const MapView& m) {
  constexpr int kCols = 48, kRows = 16, kCell = 4, kW = kCols * kCell, kH = kRows * kCell;
  constexpr float kFov = 1.5708f, kPitch = 0.6f, kFar = 80.0f;
  std::vector<uint8_t> px(size_t(kW) * kH * 3);
  auto put = [&](int x, int y, uint32_t rgb) {
    if (x < 0 || y < 0 || x >= kW || y >= kH) return;
    uint8_t* p = &px[(size_t(y) * kW + x) * 3];
    p[0] = uint8_t(rgb >> 16), p[1] = uint8_t(rgb >> 8), p[2] = uint8_t(rgb);
  };
  for (int r = 0; r < kRows; ++r)
    for (int c = 0; c < kCols; ++c) {
      float yaw = m.eye_yaw + (float(c) / (kCols - 1) - 0.5f) * kFov;
      float pitch = (0.5f - float(r) / (kRows - 1)) * kPitch;
      float d[3] = {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
      auto at = [&](float t, float* out) {
        for (int k = 0; k < 3; ++k) out[k] = m.eye[k] + d[k] * t;
      };
      float end[3];
      at(kFar, end);
      uint32_t rgb;
      if (m.sight(m.eye, end)) {
        rgb = pitch > 0 ? 0x78A8E0 : 0x304028;
      } else {
        float lo = 0, hi = kFar, p[3];
        for (int k = 0; k < 6; ++k) {
          float mid = (lo + hi) / 2;
          at(mid, p);
          (m.sight(m.eye, p) ? lo : hi) = mid;
        }
        int v = int(230 * std::pow(1.0f - std::min(hi / kFar, 1.0f), 0.8f)) + 25;
        rgb = uint32_t(v) << 16 | uint32_t(v) << 8 | uint32_t(v);
      }
      for (int y = 0; y < kCell; ++y)
        for (int x = 0; x < kCell; ++x) put(c * kCell + x, r * kCell + y, rgb);
    }
  for (const Marker& k : m.markers) {
    if (k.kind != Marker::kFriend && k.kind != Marker::kEnemy) continue;
    float dx = k.x - m.eye[0], dz = k.z - m.eye[2], dist = std::hypot(dx, dz);
    float off = std::atan2(dx, dz) - m.eye_yaw;
    while (off > 3.14159265f) off -= 6.2831853f;
    while (off < -3.14159265f) off += 6.2831853f;
    if (dist < 1 || dist > kFar || std::fabs(off) > kFov / 2) continue;
    float gy = m.ground ? m.ground(k.x, k.z) : NAN;
    float chest[3] = {k.x, (std::isfinite(gy) && gy > -1e8f ? gy : m.y) + 1.2f, k.z};
    if (!m.sight(m.eye, chest)) continue;
    float pitch = std::atan2(chest[1] - m.eye[1], dist);
    float sx = (off / kFov + 0.5f) * kW, sy = (0.5f - pitch / kPitch) * kH;
    float half = std::clamp(60.0f / dist, 2.0f, 12.0f);
    uint32_t rgb = k.kind == Marker::kEnemy ? 0xFF2020 : 0x40A0FF;
    for (int y = int(sy - half * 1.6f); y <= int(sy + half * 1.6f); ++y)
      for (int x = int(sx - half * 0.6f); x <= int(sx + half * 0.6f); ++x) put(x, y, rgb);
  }
  std::vector<uint8_t> png;
  stbi_write_png_to_func([](void* ctx, void* data, int size) {
    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
    v->insert(v->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
  }, &png, kW, kH, 3, px.data(), kW * 3);
  return png;
}

// "12 m north-east" from the bot.
std::string Where(const MapView& m, float x, float z) {
  static const char* dirs[8] = {"north", "north-east", "east", "south-east", "south", "south-west", "west", "north-west"};
  float dx = x - m.cx, dz = z - m.cz;
  int o = int(std::lround(std::atan2(dx, dz) / (3.14159265f / 4))) & 7;
  char buf[48];
  snprintf(buf, sizeof(buf), "%.0f m %s", std::hypot(dx, dz), dirs[o]);
  return buf;
}

}  // namespace

void Configure(const std::string& where) {
  if (g_enabled.exchange(true)) return;
  g_port = kPort + g_port_offset;
  bool url = where.rfind("http://", 0) == 0;
  if (url) {
    std::string hp = where.substr(7);
    hp = hp.substr(0, hp.find('/'));
    size_t c = hp.rfind(':');
    g_host = hp.substr(0, c);
    g_port = c == std::string::npos ? 80 : atoi(hp.c_str() + c + 1);
  }
  std::thread(Worker).detach();
  std::thread([url, where] {
    // One already up (another process's): used as it is.
    if (!Http("GET", "/health", "", nullptr, 2000)) {
      if (url) RT_INFO("bot llm: waiting for %s", where.c_str());
      else if (!StartServer(where.empty() ? std::filesystem::path("llm") : std::filesystem::path(where))) return;
    }
    // (Loading the model takes a while: up to 5 minutes.)
    for (int i = 0; i < 300; ++i) {
      if (Http("GET", "/health", "", nullptr, 2000)) {
        // A first question (the GPU's shaders, the caches): the bots' first
        // one would take 20 s otherwise.
        Http("POST", "/v1/chat/completions", "{\"max_tokens\":1,\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}",
             nullptr, 120000);
        g_ready = true;
        RT_INFO("bot llm: ready on %s:%d", g_host.c_str(), g_port);
        return;
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    RT_WARN("bot llm: the server did not come up: bots without it");
  }).detach();
}

bool Enabled() { return g_ready; }

void StopServer() {
  g_ready = false;
#ifdef _WIN32
  if (g_server_process) ::TerminateProcess(g_server_process, 0), g_server_process = nullptr;
#else
  if (g_server_pid > 0) ::kill(g_server_pid, SIGKILL), g_server_pid = 0;
#endif
}

bool CanAsk(int bot, int cooldown_ms) {
  if (!g_ready) return false;
  if (g_training) cooldown_ms /= 3;  // (training: asked about more)
  std::lock_guard lock(g_mutex);
  const BotState& b = g_bots[bot];
  return !b.busy && Now() - b.last >= cooldown_ms && g_jobs.size() < 4;
}

bool Ask(int bot, const std::string& situation, const MapView& map, int places, int cooldown_ms) {
  if (!g_ready) return false;
  if (g_training) cooldown_ms /= 3;
  int64_t now = Now();
  {
    std::lock_guard lock(g_mutex);
    BotState& b = g_bots[bot];
    if (b.busy || now - b.last < cooldown_ms || g_jobs.size() >= 4) return false;
    b.busy = true, b.answered = false, b.last = now;
  }
  static std::atomic<int64_t> next_id{0};
  int64_t id = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() *
                   1000 + next_id++ % 1000;
  std::string map_name;
  {
    std::lock_guard lock(g_mutex);
    map_name = g_map;
  }
  Job job{bot, {}, Draw(map), places, now, id, map_name, {}, map.kind, map.cx, map.cz, {}};
  job.places_at.resize(size_t(std::max(places, 0)));
  for (const Marker& k : map.markers)
    if (k.kind == Marker::kPlace && k.label >= 1 && k.label <= places) job.places_at[size_t(k.label - 1)] = {k.x, k.z};
  if (map.sight) {
    int64_t t0 = Now();
    job.eye_png = DrawEye(map);
    static int timed = 0;
    if (timed++ < 3) RT_INFO("bot llm: eye view in %lld ms", (long long)(Now() - t0));
  }
  // What the map shows, in words as well (a small model reads both).
  std::string text = "Situation: " + situation + "\nThe map is " + std::to_string(int(2 * map.half)) +
                     " m across (the white bar is 10 m).";
  for (const Marker& k : map.markers) {
    if (k.kind == Marker::kPlace)
      text += "\nPlace " + std::to_string(k.label) + ": " + Where(map, k.x, k.z) + (k.note.empty() ? "" : ", " + k.note) + ".";
    else if (k.kind == Marker::kEnemy) text += "\nEnemy: " + Where(map, k.x, k.z) + ".";
    else if (k.kind == Marker::kObjective) text += "\nIts objective: " + Where(map, k.x, k.z) + ".";
  }
  if (map.sight)
    text += "\nThe second picture is the bot's own view ahead, 90 degrees wide: near things light, far ones dark, "
            "the sky blue; red boxes are enemies it can see, blue ones teammates.";
  text += "\nWhich place (1-" + std::to_string(places) + ", or 0 for none)?";
  job.prompt = text;
  static const bool dump = getenv("BF_BOT_LLM_DUMP") != nullptr;  // (testing: what it was shown)
  if (dump) {
    std::filesystem::create_directories("llm_dump");
    std::string base = "llm_dump/bot" + std::to_string(bot) + "_" + std::to_string(now);
    std::ofstream(base + ".png", std::ios::binary).write(reinterpret_cast<const char*>(job.png.data()), std::streamsize(job.png.size()));
    std::ofstream(base + ".txt") << text << "\n";
  }
  {
    std::lock_guard lock(g_mutex);
    g_jobs.push_back(std::move(job));
  }
  g_cv.notify_one();
  return true;
}

bool Answer(int bot, int* place, std::string* why, int64_t* case_id) {
  std::lock_guard lock(g_mutex);
  BotState& b = g_bots[bot];
  if (!b.answered) return false;
  b.answered = false;
  *place = b.place;
  if (why) *why = b.why;
  if (case_id) *case_id = b.id;
  return true;
}

void TopsOfTriangles(const std::vector<std::array<float, 9>>& tris, float x0, float z0, float mpp, int n,
                     std::vector<float>& top) {
  top.assign(size_t(n) * n, NAN);
  auto put = [&](int x, int y, float h) {
    if (x < 0 || y < 0 || x >= n || y >= n) return;
    float& t = top[size_t(y) * n + x];
    if (!(t >= h)) t = h;
  };
  for (const auto& t : tris) {
    float px[3], py[3], h[3];
    for (int k = 0; k < 3; ++k) px[k] = (t[3 * k] - x0) / mpp, py[k] = (z0 - t[3 * k + 2]) / mpp, h[k] = t[3 * k + 1];
    float lx = std::min({px[0], px[1], px[2]}), hx = std::max({px[0], px[1], px[2]});
    float ly = std::min({py[0], py[1], py[2]}), hy = std::max({py[0], py[1], py[2]});
    if (hx < 0 || hy < 0 || lx >= n || ly >= n) continue;
    // The edges (every half pixel).
    for (int e = 0; e < 3; ++e) {
      int a = e, b = (e + 1) % 3;
      int steps = int(std::hypot(px[b] - px[a], py[b] - py[a]) * 2) + 1;
      for (int s = 0; s <= steps; ++s) {
        float f = float(s) / steps;
        put(int(px[a] + (px[b] - px[a]) * f), int(py[a] + (py[b] - py[a]) * f), h[a] + (h[b] - h[a]) * f);
      }
    }
    // The face: the pixels whose centres are inside, at its height there.
    float area = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
    if (std::fabs(area) < 0.25f) continue;
    for (int y = std::max(0, int(ly)); y <= std::min(n - 1, int(hy)); ++y)
      for (int x = std::max(0, int(lx)); x <= std::min(n - 1, int(hx)); ++x) {
        float cx = x + 0.5f, cy = y + 0.5f;
        float w0 = ((px[1] - cx) * (py[2] - cy) - (px[2] - cx) * (py[1] - cy)) / area;
        float w1 = ((px[2] - cx) * (py[0] - cy) - (px[0] - cx) * (py[2] - cy)) / area;
        float w2 = 1.0f - w0 - w1;
        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
        put(x, y, w0 * h[0] + w1 * h[1] + w2 * h[2]);
      }
  }
}

void SetLevelMap(const std::string& map, const MapView& level) {
  if (!g_training) return;
  {
    std::lock_guard lock(g_run_mutex);
    if (map == g_run_map) return;
    WriteRun();  // (the last map's, as it ended)
    g_tracks.clear(), g_run_cases.clear(), g_run_learned.clear(), g_run_shots.clear(), g_run_kills.clear();
    g_run_map = map;
    g_run_t0 = GuestSeconds();
    g_run_bounds[0] = level.cx, g_run_bounds[1] = level.cz, g_run_bounds[2] = level.half;
    char stamp[32];
    time_t now = time(nullptr);
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", localtime(&now));
    g_run_dir = AdviceDir() / "runs" / (map + "_" + stamp);
    std::error_code ec;
    std::filesystem::create_directories(g_run_dir, ec);
    std::ofstream(g_run_dir / "index.html", std::ios::binary) << kViewer;
  }
  // The whole level, 1024 pixels (outside the lock: it takes a moment).
  std::vector<uint8_t> png = Draw(level, 2048);  // (about 0.8 m a pixel on Wake Island)
  std::lock_guard lock(g_run_mutex);
  std::ofstream(g_run_dir / "map.png", std::ios::binary).write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
  WriteRun();
  RT_INFO("training: the run's viewer is %s", (g_run_dir / "index.html").string().c_str());
  static std::once_flag writer;
  std::call_once(writer, [] {
    std::thread([] {
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(20));
        std::lock_guard lock(g_run_mutex);
        WriteRun();
      }
    }).detach();
  });
}

void Track(int bot, int team, float x, float z) {
  if (!g_training) return;
  std::lock_guard lock(g_run_mutex);
  if (g_run_dir.empty()) return;
  auto& tr = g_tracks[bot];
  double t = RunTime();
  if (!tr.second.empty() && t - tr.second.back()[0] < 1.0) return;  // (once a game second)
  tr.first = team;
  tr.second.push_back({float(t), x, z});
}

void Shot(int bot, const float from[3], const float to[3], bool hit) {
  if (!g_training) return;
  std::lock_guard lock(g_run_mutex);
  if (g_run_dir.empty()) return;
  // Misses at most twice a game second per bot (it fires several times a
  // second); every hit.
  static std::map<int, double> last;
  double t = RunTime();
  if (!hit && t - last[bot] < 0.5) return;
  last[bot] = t;
  g_run_shots.push_back({float(t), from[0], from[2], to[0], to[2], bot, hit});
}

void Killed(float x, float z, int team, int killer) {
  if (!g_training) return;
  std::lock_guard lock(g_run_mutex);
  if (g_run_dir.empty()) return;
  g_run_kills.push_back({float(RunTime()), x, z, team, killer});
}

void FinishRun() {
  std::lock_guard lock(g_run_mutex);
  WriteRun();
  if (!g_run_dir.empty()) RT_INFO("training finished: %s", (g_run_dir / "index.html").string().c_str());
}

void UseMap(const std::string& map) {
  {
    std::lock_guard lock(g_mutex);
    g_map = map;
  }
  std::lock_guard lock(g_learn_mutex);
  if (map == g_learn_map) return;
  g_learn_map = map;
  g_learned.clear();
  if (FILE* f = fopen((AdviceDir() / (map + ".txt")).string().c_str(), "r")) {
    char line[256];
    while (fgets(line, sizeof(line), f)) {
      Learned l{};
      if (sscanf(line, "%d %f %f %f %f %f %f %f %d %d", &l.kind, &l.at[0], &l.at[1], &l.at[2], &l.dir, &l.place[0],
                 &l.place[1], &l.place[2], &l.wins, &l.losses) == 10)
        g_learned.push_back(l);
    }
    fclose(f);
  }
  if (!g_learned.empty()) RT_INFO("bot llm: %zu learned places on %s", g_learned.size(), map.c_str());
}

bool Recall(int kind, const float at[3], float dir, float place[3]) {
  std::lock_guard lock(g_learn_mutex);
  const Learned* best = nullptr;
  float best_score = 0;
  for (const Learned& l : g_learned) {
    float d = std::hypot(l.at[0] - at[0], l.at[2] - at[2]);
    if (l.kind != kind || d > 6.0f || std::fabs(l.at[1] - at[1]) > 3.0f || AngleOff(l.dir, dir) > 1.05f ||
        l.wins <= l.losses)
      continue;
    float score = float(l.wins - l.losses) - d * 0.1f;
    if (score > best_score) best_score = score, best = &l;
  }
  if (!best) return false;
  memcpy(place, best->place, sizeof(best->place));
  return true;
}

void Learn(int kind, const float at[3], float dir, const float place[3], bool worked, int64_t case_id) {
  if (g_training) {
    std::lock_guard run_lock(g_run_mutex);
    bool found = false;
    for (RunCase& c : g_run_cases)
      if (case_id && c.id == case_id) c.worked = worked, found = true;
    if (!case_id && !g_run_dir.empty()) g_run_learned.push_back({RunTime(), kind, at[0], at[2], worked});
    (void)found;
  }
  if (case_id)
    AppendCase("{\"id\":" + std::to_string(case_id) + ",\"worked\":" + (worked ? "true" : "false") + "}");
  std::lock_guard lock(g_learn_mutex);
  if (g_learn_map.empty()) return;
  Learned* same = nullptr;
  for (Learned& l : g_learned)
    if (l.kind == kind && std::hypot(l.at[0] - at[0], l.at[2] - at[2]) < 3.0f && std::fabs(l.at[1] - at[1]) < 3.0f &&
        AngleOff(l.dir, dir) < 0.8f && std::hypot(l.place[0] - place[0], l.place[2] - place[2]) < 3.0f)
      same = &l;
  if (!same) {
    g_learned.push_back(Learned{kind, {at[0], at[1], at[2]}, dir, {place[0], place[1], place[2]}, 0, 0});
    same = &g_learned.back();
  }
  (worked ? same->wins : same->losses)++;
  static const char* kinds[] = {"?", "a way round", "cover", "a way round driving", "moving on", "a place to fight from"};
  RT_INFO("bot llm: learned on %s: %s at %.0f,%.0f %s (%d:%d)", g_learn_map.c_str(), kinds[kind >= 1 && kind <= 5 ? kind : 0],
          at[0], at[2], worked ? "worked" : "failed", same->wins, same->losses);
  SaveLearned();
}

}  // namespace rt::botllm
