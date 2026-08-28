/*
 * Shop Door Motion Notifier - v3.0
 * ESP32-CAM + pot-less PIR (AM312 / HC-SR505) -> Telegram
 * ---------------------------------------------------------------
 * Alerts staff when someone enters the shop, without the false-positive
 * storm and reboot loop that plagued earlier revisions.
 *
 * KEY IDEA - adaptive duration discrimination
 *   These PIR modules hold their output high for a fixed period after ANY
 *   trigger, so for the first couple of seconds a thermal-drift false alarm
 *   and a real person look identical. But the module retriggers: a person
 *   who keeps moving keeps re-arming the timer, so their activation runs
 *   LONGER than the fixed floor, while drift and RF blips stop exactly at
 *   it. The sketch learns the floor at runtime and only alerts on
 *   activations that clearly exceed it. No pots, no calibration step.
 *
 * ALSO INCLUDED
 *   - Single reused TLS connection (per-call WiFiClientSecure allocation
 *     fragments the heap and eventually panics the board).
 *   - Thermal mitigation: 80MHz CPU, modem sleep, Bluetooth off. Self-
 *     heating warms the sensor's background, which simultaneously causes
 *     missed detections and drift-induced false alarms.
 *   - Radio blanking so WiFi bursts cannot contaminate a measurement.
 *   - Opening-hours filter, NTP clock, nightly restart.
 *   - Per-user mute, runtime status, automatic fallback if the module
 *     turns out not to retrigger.
 *
 * HARDWARE
 *   ESP32-CAM (or any ESP32), PIR signal -> GPIO13.
 *   GPIO13 carries an external pull-up on the CAM board, so use plain
 *   INPUT, not INPUT_PULLDOWN.
 *   Power the PIR from 5V with local decoupling; keep it as far from the
 *   antenna as the enclosure allows.
 *
 * BUILD
 *   Arduino IDE, board "ESP32 Dev Module", Flash DIO @ 40MHz,
 *   Upload Speed 115200, Partition "Huge APP". No external libraries.
 *
 * SETUP
 *   Fill in the placeholders in the CONFIG section below. Get a chat ID by
 *   messaging your bot, then opening
 *   https://api.telegram.org/bot<TOKEN>/getUpdates in a browser.
 *
 * MIT licensed.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <esp_system.h>
#include <esp_bt.h>

// ===================== CONFIG =====================

const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

const char* BOT_TOKEN = "YOUR_TELEGRAM_BOT_TOKEN";

// ---- Recipients ----
// To add coworkers: raise NUM_USERS, add their chat ID to CHAT_IDS, and add
// a matching true/false to alertsEnabled. Nothing else needs changing.
// Each person can mute themselves with the Off button without affecting
// anyone else. Get a chat ID by having them message the bot, then visit
// https://api.telegram.org/bot<TOKEN>/getUpdates in a browser.
#define NUM_USERS 1
const char* CHAT_IDS[NUM_USERS] = {
  "YOUR_TELEGRAM_CHAT_ID"
};
bool alertsEnabled[NUM_USERS] = {
  true
};

// ---- Google Sheets ----
// OFF by default. Each log costs TWO TLS handshakes (Apps Script redirects
// to script.googleusercontent.com), ~45KB heap, and 1.5-3s of blocked loop.
// Toggle at runtime with /log.
bool        logToSheetsEnabled = false;
const char* SCRIPT_URL = "YOUR_GOOGLE_APPS_SCRIPT_WEB_APP_URL";

// ---- Sensor ----
const int  PIR_PIN         = 13;    // GPIO13: plain INPUT, external pull-up present
const bool PIR_ACTIVE_HIGH = true;
const unsigned long PIR_WARMUP_MS = 90000;
const unsigned long PIR_SAMPLE_MS = 25;

// ---- Duration discrimination (your only sensitivity control) ----
// Alert once an activation lasts longer than (learned floor * MULT + PAD).
// Too many false alarms  -> raise DURATION_MULT toward 1.8
// Missing real customers -> lower DURATION_MULT toward 1.2
const float         DURATION_MULT    = 1.45f;
const unsigned long DURATION_PAD_MS  = 500;
const unsigned long MIN_HOLD_MS      = 2000;
const unsigned long MAX_HOLD_MS      = 9000;
const unsigned long RF_EXTRA_HOLD_MS = 1500;
const unsigned long RF_BLANK_MS      = 1200;

const float FLOOR_MIN_MS = 800.0f;
const float FLOOR_MAX_MS = 12000.0f;
const float FLOOR_START  = 2000.0f;

// ---- Fallback safety net ----
// Only trips on a LONG run of rejections spread over real time, so a testing
// session of quick hand-waves cannot trigger it. Re-arms automatically.
const int           DEAF_LIMIT        = 10;
const unsigned long DEAF_MIN_SPAN_MS  = 900000;    // 15 minutes
const unsigned long DEAF_REARM_MS     = 7200000;   // retry adaptive after 2h
const unsigned long FALLBACK_HOLD_MS  = 1200;

// ---- Alerting ----
const unsigned long ALERT_COOLDOWN_MS = 20000;

// ---- Opening hours: 09:30-18:00, Mon Tue Thu Fri Sat ----
bool      enforceHours  = true;
const int OPEN_MINUTES  = 9 * 60 + 30;
const int CLOSE_MINUTES = 18 * 60;
// index: Sun=0, Mon=1, Tue=2, Wed=3, Thu=4, Fri=5, Sat=6
const bool OPEN_DAYS[7] = { false, true, true, false, true, true, true };
const int PRE_OPEN_GRACE_MIN = 30;   // start alerting this early

// ---- Noise damping ----
const uint32_t      NOISE_PER_HOUR     = 8;
const unsigned long NOISE_PENALTY_STEP = 1000;
const unsigned long NOISE_PENALTY_MAX  = 5000;

// ---- Housekeeping ----
const unsigned long TELEGRAM_POLL_MS = 30000;
const unsigned long WIFI_CHECK_MS    = 10000;
const unsigned long WIFI_RETRY_MS    = 30000;
const unsigned long WIFI_GIVEUP_MS   = 300000;
const unsigned long HEALTH_LOG_MS    = 120000;
const uint32_t      HEAP_PANIC_BYTES = 15000;
const int           NIGHTLY_REBOOT_HOUR = 3;   // -1 to disable

const char* TZ_INFO = "GMT0IST,M3.5.0/1,M10.5.0/2";

// ===================== STATE =====================

RTC_DATA_ATTR uint32_t rtcBootCount    = 0;
RTC_DATA_ATTR uint32_t rtcCrashNotices = 0;
RTC_DATA_ATTR long     rtcLastUpdateId = 0;

WiFiClientSecure tgClient;
HTTPClient       tgHttp;

unsigned long bootMillis = 0;
unsigned long lastPirSample = 0, lastTelegramPoll = 0, lastWifiCheck = 0;
unsigned long lastHealthLog = 0, wifiDownSince = 0, lastWifiRetry = 0;
unsigned long lastRadioMs = 0;

bool          sigHigh = false;
unsigned long highSince = 0;
bool          alertedThisHigh = false;
bool          rfSuspect = false;
float         txFloorMs = FLOOR_START;
int           deafCounter = 0;
unsigned long deafRunStart = 0;
unsigned long fallbackSince = 0;
bool          discriminate = true;
bool          deafWarned = false;

unsigned long lastAlertAt = 0;
uint32_t alertCount = 0, suppressedCooldown = 0, suppressedHours = 0;
uint32_t rejectedShort = 0, activationCount = 0;
unsigned long noisePenaltyMs = 0;
uint32_t alertsThisHour = 0;
int      currentHourBucket = -1;
int      lastRebootDay = -1;

// ===================== HELPERS =====================

const char* resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_EXT:      return "external reset";
    case ESP_RST_SW:       return "software restart";
    case ESP_RST_PANIC:    return "crash (panic)";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout (power supply)";
    default:               return "unknown";
  }
}

String urlEncode(const String& src) {
  static const char* hex = "0123456789ABCDEF";
  String out; out.reserve(src.length() * 2);
  for (size_t i = 0; i < src.length(); i++) {
    uint8_t c = (uint8_t)src[i];
    bool safe = (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||
                c=='-'||c=='_'||c=='.'||c=='~';
    if (safe) out += (char)c;
    else { out += '%'; out += hex[c>>4]; out += hex[c&0x0F]; }
  }
  return out;
}

bool localNow(struct tm* out) { return getLocalTime(out, 200); }

String timestampNow() {
  struct tm t;
  if (!localNow(&t)) return String("time-unsynced");
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
  return String(buf);
}

String uptimeText() {
  unsigned long s = (millis() - bootMillis) / 1000UL;
  char buf[40];
  snprintf(buf, sizeof(buf), "%lud %luh %lum", s/86400UL, (s%86400UL)/3600UL, (s%3600UL)/60UL);
  return String(buf);
}

bool withinOpeningHours() {
  if (!enforceHours) return true;
  struct tm t;
  if (!localNow(&t)) return true;             // clock unknown: fail open
  if (!OPEN_DAYS[t.tm_wday]) return false;
  int mins = t.tm_hour * 60 + t.tm_min;
  return (mins >= (OPEN_MINUTES - PRE_OPEN_GRACE_MIN)) && (mins < CLOSE_MINUTES);
}

unsigned long requiredHoldMs() {
  if (!discriminate) return FALLBACK_HOLD_MS + noisePenaltyMs;
  float need = txFloorMs * DURATION_MULT + DURATION_PAD_MS + noisePenaltyMs;
  if (need < MIN_HOLD_MS) need = MIN_HOLD_MS;
  if (need > MAX_HOLD_MS) need = MAX_HOLD_MS;
  return (unsigned long)need;
}

// ===================== TELEGRAM =====================

bool telegramCall(const char* method, const String& body, String* response) {
  if (WiFi.status() != WL_CONNECTED) return false;
  lastRadioMs = millis();

  String url = String("https://api.telegram.org/bot") + BOT_TOKEN + "/" + method;
  if (!tgHttp.begin(tgClient, url)) return false;
  tgHttp.addHeader("Content-Type", "application/x-www-form-urlencoded");

  int code = tgHttp.POST(body);
  bool ok = (code == 200);
  if (!ok) {
    Serial.printf("[TG] %s -> HTTP %d\n", method, code);
    if (response) *response = "";
    tgHttp.end(); tgClient.stop();
    lastRadioMs = millis();
    return false;
  }
  if (response) *response = tgHttp.getString();
  tgHttp.end();
  lastRadioMs = millis();
  return true;
}

void sendMessage(const char* chatId, const String& text, bool withButtons) {
  String body = "chat_id=" + String(chatId) + "&text=" + urlEncode(text) +
                "&parse_mode=HTML&disable_web_page_preview=true";
  if (withButtons) {
    String kb = "{\"keyboard\":[[{\"text\":\"On\"},{\"text\":\"Off\"}],"
                "[{\"text\":\"/status\"},{\"text\":\"/test\"}]],"
                "\"resize_keyboard\":true,\"is_persistent\":true}";
    body += "&reply_markup=" + urlEncode(kb);
  }
  telegramCall("sendMessage", body, nullptr);
}

void broadcast(const String& text, bool onlyIfAlertsOn) {
  for (int i = 0; i < NUM_USERS; i++) {
    if (onlyIfAlertsOn && !alertsEnabled[i]) continue;
    sendMessage(CHAT_IDS[i], text, false);
  }
}

void drainTelegramBacklog() {
  String resp;
  if (!telegramCall("getUpdates", "offset=-1&timeout=0", &resp)) return;
  int idx = resp.lastIndexOf("\"update_id\":");
  if (idx == -1) return;
  int start = idx + 12, end = start;
  while (end < (int)resp.length() && isDigit(resp[end])) end++;
  rtcLastUpdateId = resp.substring(start, end).toInt();
}

String extractAfter(const String& block, const char* key, bool quoted) {
  int i = block.indexOf(key);
  if (i == -1) return "";
  int start = i + strlen(key);
  if (quoted) {
    int end = block.indexOf('"', start);
    return (end == -1) ? String("") : block.substring(start, end);
  }
  int end = start;
  while (end < (int)block.length() && (isDigit(block[end]) || block[end]=='-')) end++;
  return block.substring(start, end);
}

String hoursText() {
  char buf[64];
  snprintf(buf, sizeof(buf), "%02d:%02d-%02d:%02d Mon Tue Thu Fri Sat",
           OPEN_MINUTES/60, OPEN_MINUTES%60, CLOSE_MINUTES/60, CLOSE_MINUTES%60);
  return String(buf);
}

int enabledUserCount() {
  int n = 0;
  for (int i = 0; i < NUM_USERS; i++) if (alertsEnabled[i]) n++;
  return n;
}

void handleCommand(int u, const String& text) {
  if (text == "Off") {
    alertsEnabled[u] = false;
    sendMessage(CHAT_IDS[u], "\xE2\x9D\x8C Alerts <b>OFF</b> for you. "
                + String(enabledUserCount()) + "/" + String(NUM_USERS)
                + " still receiving.", true);
  } else if (text == "On") {
    alertsEnabled[u] = true;
    sendMessage(CHAT_IDS[u], "\xE2\x9C\x85 Alerts <b>ON</b> for you. "
                + String(enabledUserCount()) + "/" + String(NUM_USERS)
                + " receiving.", true);
  } else if (text == "/hours") {
    enforceHours = !enforceHours;
    sendMessage(CHAT_IDS[u], enforceHours
      ? "\xF0\x9F\x95\x92 Hours filter <b>ON</b>\n" + hoursText()
      : "\xF0\x9F\x95\x92 Hours filter <b>OFF</b> - alerting 24/7.", true);
  } else if (text == "/log") {
    logToSheetsEnabled = !logToSheetsEnabled;
    sendMessage(CHAT_IDS[u], logToSheetsEnabled
      ? "\xF0\x9F\x93\x8A Sheets logging <b>ON</b> (slower alerts, more heat)."
      : "\xF0\x9F\x93\x8A Sheets logging <b>OFF</b>.", true);
  } else if (text == "/status" || text == "status") {
    String m = "<b>Shop door sensor</b>\n";
    m += "Your alerts: " + String(alertsEnabled[u] ? "ON" : "OFF");
    m += withinOpeningHours() ? " (open now)\n" : " (closed now)\n";
    m += "Receiving: " + String(enabledUserCount()) + "/" + String(NUM_USERS) + "\n";
    m += "Hours: " + String(enforceHours ? hoursText() : String("24/7")) + "\n";
    m += "Time: " + timestampNow() + "\n";
    m += "Uptime: " + uptimeText() + "\n";
    m += "Mode: " + String(discriminate ? "adaptive" : "FALLBACK") + "\n";
    m += "Learned floor: " + String((int)txFloorMs) + " ms\n";
    m += "Alert threshold: " + String(requiredHoldMs()) + " ms\n";
    if (noisePenaltyMs) m += "Noise penalty: +" + String(noisePenaltyMs) + " ms\n";
    m += "Activations: " + String(activationCount) + "\n";
    m += "Rejected (too short): " + String(rejectedShort) + "\n";
    m += "Alerts sent: " + String(alertCount) + "\n";
    m += "Suppressed (closed): " + String(suppressedHours) + "\n";
    m += "Sheets log: " + String(logToSheetsEnabled ? "on" : "off") + "\n";
    m += "WiFi " + String(WiFi.RSSI()) + " dBm, heap " + String(ESP.getFreeHeap());
    sendMessage(CHAT_IDS[u], m, true);
  } else if (text == "/test" || text == "test") {
    sendMessage(CHAT_IDS[u], "\xF0\x9F\x9B\x8E\xEF\xB8\x8F Test alert - path OK.", true);
  } else if (text == "/reset") {
    txFloorMs = FLOOR_START; discriminate = true; deafCounter = 0;
    deafRunStart = 0; fallbackSince = 0; deafWarned = false; noisePenaltyMs = 0;
    sendMessage(CHAT_IDS[u], "\xF0\x9F\x94\x84 Learning reset, adaptive mode on.", true);
  }
}

void pollTelegram() {
  String resp;
  String body = "offset=" + String(rtcLastUpdateId + 1) +
                "&timeout=0&allowed_updates=%5B%22message%22%5D";
  if (!telegramCall("getUpdates", body, &resp)) return;
  if (resp.indexOf("\"result\":[]") != -1) return;

  int pos = 0;
  while (true) {
    int here = resp.indexOf("\"update_id\":", pos);
    if (here == -1) break;
    int next = resp.indexOf("\"update_id\":", here + 12);
    String block = (next == -1) ? resp.substring(here) : resp.substring(here, next);
    pos = (next == -1) ? resp.length() : next;

    long id = extractAfter(block, "\"update_id\":", false).toInt();
    if (id > rtcLastUpdateId) rtcLastUpdateId = id;

    String sender = extractAfter(block, "\"chat\":{\"id\":", false);
    String text   = extractAfter(block, "\"text\":\"", true);
    if (!sender.length() || !text.length()) continue;
    for (int i = 0; i < NUM_USERS; i++) if (sender == CHAT_IDS[i]) handleCommand(i, text);
  }
}

// ===================== SHEETS =====================

void logToSheets(const String& timestamp, const String& status) {
  if (!logToSheetsEnabled || WiFi.status() != WL_CONNECTED) return;
  lastRadioMs = millis();
  WiFiClientSecure c; c.setInsecure();
  HTTPClient h; h.setTimeout(8000);
  h.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!h.begin(c, SCRIPT_URL)) return;
  h.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int code = h.POST("timestamp=" + urlEncode(timestamp) + "&status=" + urlEncode(status));
  Serial.printf("[Sheets] HTTP %d\n", code);
  h.end(); c.stop();
  lastRadioMs = millis();
}

// ===================== MOTION =====================

bool pirIsActive() {
  int v = digitalRead(PIR_PIN);
  return PIR_ACTIVE_HIGH ? (v == HIGH) : (v == LOW);
}

void learnTxFloor(unsigned long dur) {
  if (dur < 500) return;                    // wiring glitch, not a real cycle
  if (dur < txFloorMs) txFloorMs = txFloorMs * 0.35f + (float)dur * 0.65f;
  else                 txFloorMs += 15.0f;
  if (txFloorMs < FLOOR_MIN_MS) txFloorMs = FLOOR_MIN_MS;
  if (txFloorMs > FLOOR_MAX_MS) txFloorMs = FLOOR_MAX_MS;
}

void noteHourlyAlert() {
  struct tm t;
  int hour = localNow(&t) ? t.tm_hour : (int)((millis() / 3600000UL) % 24);
  if (hour != currentHourBucket) {
    if (currentHourBucket != -1 && alertsThisHour < NOISE_PER_HOUR && noisePenaltyMs > 0) {
      noisePenaltyMs = (noisePenaltyMs > NOISE_PENALTY_STEP)
                       ? noisePenaltyMs - NOISE_PENALTY_STEP : 0;
    }
    currentHourBucket = hour;
    alertsThisHour = 0;
  }
  alertsThisHour++;
  if (alertsThisHour == NOISE_PER_HOUR && noisePenaltyMs < NOISE_PENALTY_MAX) {
    noisePenaltyMs += NOISE_PENALTY_STEP;
    Serial.printf("[Noise] penalty now %lu\n", noisePenaltyMs);
    broadcast("\xE2\x9A\xA0\xEF\xB8\x8F Sensor triggering often. Threshold raised to "
              + String(requiredHoldMs()/1000) + "s.", false);
  }
}

void fireAlert(unsigned long heldMs) {
  unsigned long now = millis();

  if (!withinOpeningHours()) {
    suppressedHours++;
    Serial.println("[PIR] confirmed but shop closed, suppressed");
    return;
  }
  if (lastAlertAt != 0 && now - lastAlertAt < ALERT_COOLDOWN_MS) {
    suppressedCooldown++;
    return;
  }

  lastAlertAt = now;
  alertCount++;
  noteHourlyAlert();

  String stamp = timestampNow();
  Serial.printf("[PIR] ALERT #%u held %lums (thr %lu, floor %d)\n",
                alertCount, heldMs, requiredHoldMs(), (int)txFloorMs);

  broadcast("\xF0\x9F\x9A\xAA Someone at the shop door\n<i>" + stamp + "</i>", true);
  logToSheets(stamp, "Motion Detected");
}

void samplePir() {
  unsigned long now = millis();
  if (now - bootMillis < PIR_WARMUP_MS) return;

  bool active = pirIsActive();

  if (active && !sigHigh) {
    sigHigh = true;
    highSince = now;
    alertedThisHigh = false;
    rfSuspect = (lastRadioMs != 0) && (now - lastRadioMs < RF_BLANK_MS);
    activationCount++;
    if (rfSuspect) Serial.println("[PIR] began during radio TX");
    return;
  }

  if (active && sigHigh && !alertedThisHigh) {
    unsigned long need = requiredHoldMs() + (rfSuspect ? RF_EXTRA_HOLD_MS : 0);
    unsigned long held = now - highSince;
    if (held >= need) {
      alertedThisHigh = true;
      deafCounter = 0;
      deafRunStart = 0;
      fireAlert(held);
    }
    return;
  }

  if (!active && sigHigh) {
    unsigned long held = now - highSince;
    sigHigh = false;
    learnTxFloor(held);

    if (!alertedThisHigh) {
      rejectedShort++;
      if (deafCounter == 0) deafRunStart = now;
      deafCounter++;
      Serial.printf("[PIR] rejected: held %lums, needed %lu (floor %d, run %d)\n",
                    held, requiredHoldMs(), (int)txFloorMs, deafCounter);

      // Only fall back on a long run of rejections spread over real time.
      // A testing session of quick hand-waves cannot trip this.
      if (discriminate && deafCounter >= DEAF_LIMIT &&
          (now - deafRunStart) >= DEAF_MIN_SPAN_MS) {
        discriminate = false;
        fallbackSince = now;
        Serial.println("[PIR] discriminator disabled, falling back");
        if (!deafWarned) {
          deafWarned = true;
          broadcast("\xE2\x9A\xA0\xEF\xB8\x8F Tuning check: nothing has exceeded the "
                    "learned threshold for 15+ minutes of activity, so this module may "
                    "not be retriggering. Switching to simple detection for 2 hours, "
                    "then retrying automatically.\n\nIf this repeats during real trading, "
                    "lower DURATION_MULT toward 1.2 and reflash.", false);
        }
      }
    }
  }
}

// Periodically give adaptive mode another go rather than staying degraded.
void maybeRearmDiscriminator() {
  if (discriminate || fallbackSince == 0) return;
  if (millis() - fallbackSince < DEAF_REARM_MS) return;
  discriminate = true;
  deafCounter = 0;
  deafRunStart = 0;
  fallbackSince = 0;
  txFloorMs = FLOOR_START;
  Serial.println("[PIR] re-arming adaptive discrimination");
}

// ===================== WIFI =====================

void startWifi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname("shop-door");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void maintainWifi() {
  unsigned long now = millis();
  if (now - lastWifiCheck < WIFI_CHECK_MS) return;
  lastWifiCheck = now;

  if (WiFi.status() == WL_CONNECTED) {
    if (wifiDownSince) { Serial.println("[WiFi] reconnected"); wifiDownSince = 0; }
    return;
  }
  if (!wifiDownSince) {
    wifiDownSince = now; lastWifiRetry = now;
    Serial.println("[WiFi] lost"); tgClient.stop();
  }
  if (now - lastWifiRetry > WIFI_RETRY_MS) {
    lastWifiRetry = now;
    WiFi.disconnect(); WiFi.begin(WIFI_SSID, WIFI_PASS);
    lastRadioMs = now;
  }
  if (now - wifiDownSince > WIFI_GIVEUP_MS) { Serial.println("[WiFi] restart"); ESP.restart(); }
}

void maybeNightlyReboot() {
  if (NIGHTLY_REBOOT_HOUR < 0) return;
  struct tm t;
  if (!localNow(&t)) return;
  if (lastRebootDay == -1) { lastRebootDay = t.tm_mday; return; }
  if (t.tm_hour == NIGHTLY_REBOOT_HOUR && t.tm_mday != lastRebootDay
      && millis() - bootMillis > 3600000UL) {
    Serial.println("[Sys] nightly restart");
    ESP.restart();
  }
}

// ===================== SETUP / LOOP =====================

void setup() {
  setCpuFrequencyMhz(80);          // cooler than 240MHz, still ample

  Serial.begin(115200);
  delay(200);
  bootMillis = millis();

  btStop();
  esp_bt_controller_disable();

  pinMode(PIR_PIN, INPUT);

  rtcBootCount++;
  const char* reason = resetReasonText();
  Serial.printf("\n=== Shop door sensor v3.0 ===\nBoot #%u (%s), CPU %uMHz, users %d\n",
                rtcBootCount, reason, getCpuFrequencyMhz(), NUM_USERS);

  startWifi();
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000) { delay(250); Serial.print("."); }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] %s  RSSI %d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  }

  tgClient.setInsecure();
  tgClient.setTimeout(10000);
  tgHttp.setReuse(true);
  tgHttp.setTimeout(10000);
  tgHttp.setConnectTimeout(10000);

  drainTelegramBacklog();

  if (rtcBootCount == 1) {
    String m = "\xF0\x9F\xA4\x96 Shop door sensor online.\n";
    m += "Warming up " + String(PIR_WARMUP_MS/1000) + "s.\n";
    m += "Hours: " + hoursText();
    for (int i = 0; i < NUM_USERS; i++) sendMessage(CHAT_IDS[i], m, true);
  } else if (rtcCrashNotices < 3) {
    rtcCrashNotices++;
    String m = "\xE2\x9A\xA0\xEF\xB8\x8F Restarted (" + String(reason) + "), boot #" +
               String(rtcBootCount) + ".";
    if (rtcCrashNotices == 3) m += "\nFurther notices muted.";
    for (int i = 0; i < NUM_USERS; i++) sendMessage(CHAT_IDS[i], m, false);
  }

  Serial.printf("[Heap] %u free after setup\n", ESP.getFreeHeap());
}

void loop() {
  unsigned long now = millis();

  if (now - lastPirSample >= PIR_SAMPLE_MS) {
    lastPirSample = now;
    samplePir();
  }

  maintainWifi();

  // Never transmit while measuring an activation: radio bursts contaminate
  // the very measurement the filter depends on.
  if (now - lastTelegramPoll >= TELEGRAM_POLL_MS && !sigHigh) {
    lastTelegramPoll = now;
    if (WiFi.status() == WL_CONNECTED) pollTelegram();
  }

  if (now - lastHealthLog >= HEALTH_LOG_MS) {
    lastHealthLog = now;
    uint32_t heap = ESP.getFreeHeap();
    Serial.printf("[Health] up %s | heap %u | RSSI %d | floor %dms | thr %lums | "
                  "act %u | rej %u | alerts %u | %s | %s\n",
                  uptimeText().c_str(), heap, WiFi.RSSI(), (int)txFloorMs,
                  requiredHoldMs(), activationCount, rejectedShort, alertCount,
                  withinOpeningHours() ? "OPEN" : "closed",
                  discriminate ? "adaptive" : "fallback");
    if (heap < HEAP_PANIC_BYTES) { Serial.println("[Health] low heap, restart"); ESP.restart(); }
    maybeRearmDiscriminator();
    maybeNightlyReboot();
  }

  delay(10);
}
