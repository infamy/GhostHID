// Host-test shim for Arduino.h — just enough for the logic layers to compile.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <string>

// Test-controllable clock (see test_main.cpp).
extern uint32_t g_test_millis;
inline uint32_t millis() { return g_test_millis; }
inline void delay(uint32_t) {}

// Minimal Arduino String (std::string-backed) — Config uses getString()/length()/c_str().
class String {
    std::string s_;
public:
    String() {}
    String(const char* p) : s_(p ? p : "") {}
    String(const std::string& s) : s_(s) {}
    const char* c_str() const { return s_.c_str(); }
    size_t length() const { return s_.size(); }
    bool isEmpty() const { return s_.empty(); }
    String& operator=(const char* p) { s_ = p ? p : ""; return *this; }
    bool operator==(const char* p) const { return s_ == (p ? p : ""); }
};

// No-op Serial for host tests.
struct SerialStub {
    void begin(unsigned long) {}
    void flush() {}
    template <class... A> void print(A...) {}
    template <class... A> void println(A...) {}
    template <class... A> int  printf(A...) { return 0; }
};
inline SerialStub Serial;

// No-op ESP heap accessors used by CommandProcessor's status report.
struct EspStub {
    uint32_t getFreeHeap() { return 100000; }
    uint32_t getMaxAllocHeap() { return 60000; }
};
inline EspStub ESP;
