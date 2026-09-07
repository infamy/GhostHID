#pragma once
#include <Arduino.h>
#include <map>
#include <string>
#include <stdint.h>
// In-memory stand-in for the ESP32 NVS Preferences API (subset Config uses).
class Preferences {
    std::map<std::string,std::string> s_;   // strings
    std::map<std::string,uint32_t>    n_;   // bool/ushort
public:
    bool begin(const char*, bool = false) { return true; }
    void end() {}
    bool clear() { s_.clear(); n_.clear(); return true; }
    bool remove(const char* k) { s_.erase(k); n_.erase(k); return true; }
    size_t putString(const char* k, const char* v) { s_[k] = v ? v : ""; return s_[k].size(); }
    String getString(const char* k, const String& def = String()) {
        auto it = s_.find(k); return it==s_.end() ? def : String(it->second.c_str());
    }
    bool putBool(const char* k, bool v) { n_[k] = v?1:0; return true; }
    bool getBool(const char* k, bool def=false) { auto it=n_.find(k); return it==n_.end()?def:it->second!=0; }
    uint16_t putUShort(const char* k, uint16_t v) { n_[k]=v; return v; }
    uint16_t getUShort(const char* k, uint16_t def=0) { auto it=n_.find(k); return it==n_.end()?def:(uint16_t)it->second; }
    bool isKey(const char* k) { return s_.count(k)||n_.count(k); }
};
