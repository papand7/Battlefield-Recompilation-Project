// A plain HTTP(S) GET for the master server list (lan.cpp): WinHTTP on
// Windows, curl on Linux (the console server). Blocking: call it from a
// background thread.
#include "http.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#else
#include <cstdio>
#endif

#include <string>

namespace rt::http {

#ifdef _WIN32
bool Get(const std::string& url, std::string* body, int timeout_ms) {
  if (body) body->clear();
  std::wstring wide(url.begin(), url.end());
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  wchar_t host[256] = {}, path[2048] = {};
  parts.lpszHostName = host, parts.dwHostNameLength = 255;
  parts.lpszUrlPath = path, parts.dwUrlPathLength = 2047;
  wchar_t extra[2048] = {};
  parts.lpszExtraInfo = extra, parts.dwExtraInfoLength = 2047;
  if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts)) return false;
  std::wstring target = std::wstring(path) + extra;
  bool ok = false;
  HINTERNET session = WinHttpOpen(L"BF1943-PC", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return false;
  WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
  HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
  HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                      WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                      parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                                 : nullptr;
  if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
      WinHttpReceiveResponse(request, nullptr)) {
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    std::string data;
    for (DWORD available = 0; WinHttpQueryDataAvailable(request, &available) && available;) {
      std::string chunk(available, '\0');
      DWORD read = 0;
      if (!WinHttpReadData(request, chunk.data(), available, &read) || !read) break;
      data.append(chunk.data(), read);
      if (data.size() > (1u << 20)) break;  // a list is a few kB
    }
    if (body) *body = std::move(data);
    ok = status >= 200 && status < 300;
  }
  if (request) WinHttpCloseHandle(request);
  if (connection) WinHttpCloseHandle(connection);
  WinHttpCloseHandle(session);
  return ok;
}
#else
bool Get(const std::string& url, std::string* body, int timeout_ms) {
  if (body) body->clear();
  // (The URL is ours, from the command line or the default: still, no quotes.)
  if (url.find_first_of("'\"`$\\") != std::string::npos) return false;
  std::string command = "curl -sf --max-time " + std::to_string((timeout_ms + 999) / 1000) + " '" + url + "'";
  FILE* p = popen(command.c_str(), "r");
  if (!p) return false;
  std::string data;
  char buf[4096];
  for (size_t n; (n = fread(buf, 1, sizeof(buf), p)) > 0;) data.append(buf, n);
  bool ok = pclose(p) == 0;
  if (body) *body = std::move(data);
  return ok;
}
#endif

}  // namespace rt::http
