#include "Config.h"

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

#include "board_config.h"

namespace ghosthid {
namespace {

// NVS namespace and keys. NVS keys are limited to 15 characters.
constexpr char kNamespace[] = "ghosthid";
constexpr char kKeySsid[]   = "sta_ssid";
constexpr char kKeyStaPw[]  = "sta_pass";
constexpr char kKeyApPw[]   = "ap_pass";
constexpr char kKeyToken[]  = "token";
constexpr char kKeyName[]   = "name";
constexpr char kKeyApAlways[] = "ap_always";
constexpr char kKeyDfOn[]     = "df_on";
constexpr char kKeyDfHost[]   = "df_host";
constexpr char kKeyDfPort[]   = "df_port";
constexpr char kKeyDfScreen[] = "df_screen";
constexpr char kKeyDfW[]      = "df_w";
constexpr char kKeyDfH[]      = "df_h";
constexpr char kKeyDfTls[]    = "df_tls";

Preferences g_prefs;

void loadInto(const char *key, const char *fallback, char *out, size_t outSize) {
    String v = g_prefs.getString(key, String(fallback));
    snprintf(out, outSize, "%s", v.c_str());
}

bool store(const char *key, const char *value) {
    return g_prefs.putString(key, value) > 0 || value[0] == '\0';
}

}  // namespace

bool Config::validApPassword(const char *p) {
    if (p == nullptr) return false;
    const size_t n = strlen(p);
    return n >= 8 && n <= 63;
}

void Config::begin() {
    g_prefs.begin(kNamespace, /*readOnly=*/false);

    loadInto(kKeySsid,  GHOSTHID_STA_SSID,     staSsid_, sizeof(staSsid_));
    loadInto(kKeyStaPw, GHOSTHID_STA_PASSWORD, staPass_, sizeof(staPass_));
    loadInto(kKeyApPw,  GHOSTHID_AP_PASSWORD,  apPass_,  sizeof(apPass_));
    loadInto(kKeyToken, GHOSTHID_AUTH_TOKEN,   token_,   sizeof(token_));
    loadInto(kKeyName,  GHOSTHID_MDNS_NAME,    name_,    sizeof(name_));

    apAlways_ = g_prefs.getBool(kKeyApAlways, true);

    dfEnabled_ = g_prefs.getBool(kKeyDfOn, false);
    loadInto(kKeyDfHost, "", dfHost_, sizeof(dfHost_));
    dfPort_ = g_prefs.getUShort(kKeyDfPort, 24800);
    // Default the screen name to the device name so it is recognisable in a
    // server's layout without extra configuration.
    loadInto(kKeyDfScreen, name_, dfScreen_, sizeof(dfScreen_));
    if (dfScreen_[0] == '\0') snprintf(dfScreen_, sizeof(dfScreen_), "%s", name_);
    dfWidth_  = g_prefs.getUShort(kKeyDfW, 1920);
    dfHeight_ = g_prefs.getUShort(kKeyDfH, 1080);
    dfTls_    = g_prefs.getBool(kKeyDfTls, true);

    // A stored AP password that fails validation would bring the AP up open.
    // Fall back rather than do that.
    if (!validApPassword(apPass_)) {
        snprintf(apPass_, sizeof(apPass_), "%s", GHOSTHID_AP_PASSWORD);
    }
}

bool Config::setStation(const char *ssid, const char *password) {
    if (ssid == nullptr || password == nullptr) return false;
    if (strlen(ssid) > kSsidMax || strlen(password) > kPassMax) return false;
    // An empty SSID is legitimate: it means "disable station mode".
    snprintf(staSsid_, sizeof(staSsid_), "%s", ssid);
    snprintf(staPass_, sizeof(staPass_), "%s", password);
    store(kKeySsid,  staSsid_);
    store(kKeyStaPw, staPass_);
    rebootPending_ = true;
    return true;
}

bool Config::setApPassword(const char *password) {
    if (!validApPassword(password)) return false;
    snprintf(apPass_, sizeof(apPass_), "%s", password);
    rebootPending_ = true;
    return store(kKeyApPw, apPass_);
}

bool Config::setAuthToken(const char *token) {
    if (token == nullptr || strlen(token) > kTokenMax) return false;
    snprintf(token_, sizeof(token_), "%s", token);
    return store(kKeyToken, token_);
}

bool Config::setDeviceName(const char *name) {
    if (name == nullptr || name[0] == '\0' || strlen(name) > kNameMax) return false;
    // Used as an mDNS hostname, so keep it to a DNS-safe alphabet.
    for (const char *p = name; *p; ++p) {
        const bool ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                        (*p >= '0' && *p <= '9') || *p == '-';
        if (!ok) return false;
    }
    snprintf(name_, sizeof(name_), "%s", name);
    rebootPending_ = true;
    return store(kKeyName, name_);
}

bool Config::setApAlways(bool always) {
    apAlways_ = always;
    g_prefs.putBool(kKeyApAlways, always);
    rebootPending_ = true;
    return true;
}

bool Config::setDeskflowServer(const char *host, uint16_t port) {
    if (host == nullptr || strlen(host) > kHostMax) return false;
    if (port == 0) return false;
    snprintf(dfHost_, sizeof(dfHost_), "%s", host);
    dfPort_ = port;
    store(kKeyDfHost, dfHost_);
    g_prefs.putUShort(kKeyDfPort, dfPort_);
    return true;
}

bool Config::setDeskflowScreen(const char *screenName) {
    if (screenName == nullptr || screenName[0] == '\0') return false;
    if (strlen(screenName) > sizeof(dfScreen_) - 1) return false;
    snprintf(dfScreen_, sizeof(dfScreen_), "%s", screenName);
    return store(kKeyDfScreen, dfScreen_);
}

bool Config::setDeskflowScreenSize(uint16_t w, uint16_t h) {
    // Guard against nonsense that would make every pointer position wrong.
    if (w < 320 || h < 240 || w > 16384 || h > 16384) return false;
    dfWidth_ = w; dfHeight_ = h;
    g_prefs.putUShort(kKeyDfW, w);
    g_prefs.putUShort(kKeyDfH, h);
    return true;
}

bool Config::setDeskflowTls(bool on) {
    dfTls_ = on;
    g_prefs.putBool(kKeyDfTls, on);
    return true;
}

bool Config::setDeskflowEnabled(bool on) {
    dfEnabled_ = on;
    g_prefs.putBool(kKeyDfOn, on);
    return true;
}

void Config::factoryReset() {
    g_prefs.clear();
}

}  // namespace ghosthid
