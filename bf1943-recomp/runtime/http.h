// A plain HTTP(S) GET (http.cpp), for the master server list.
#pragma once

#include <string>

namespace rt::http {
// The body of url's answer; false on no answer or a status that isn't 2xx.
// Blocks up to about timeout_ms per step: call it from a background thread.
bool Get(const std::string& url, std::string* body, int timeout_ms = 8000);
}  // namespace rt::http
