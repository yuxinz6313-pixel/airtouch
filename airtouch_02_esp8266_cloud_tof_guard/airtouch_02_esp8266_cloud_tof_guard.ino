#include <ESP8266WiFi.h>
#include <SoftwareSerial.h>
#include <Wire.h>
#include <VL53L0X.h>

#define WIFI_SSID "H"
#define WIFI_PASS "12345678"

#define WORKER_HOST "airtouch-cloud-worker.zyx-airtouch-cloud.workers.dev"
#define WORKER_PATH "/api/airtouch/records"
#define DEVICE_TOKEN "airtouch_demo_token_2026"

#define P4_RX_PIN D5
#define P4_TX_PIN D6
#define P4_BAUD 9600
#define USB_BAUD 115200

SoftwareSerial p4Serial(P4_RX_PIN, P4_TX_PIN);

// ================= AirTouch ESP8266 ToF Guard Merge v1c =================
// Original cloud gateway logic remains unchanged.
// Extra function: VL53L0X distance guard on ESP8266 D1/D2.
// Event-only UART protocol to P4:
//   GUARD,<seq>,<guard_on>,<distance_mm>,<reason>

#ifndef D1
#define D1 5
#endif

#ifndef D2
#define D2 4
#endif

#define AIRTOUCH_TOF_SDA_PIN D2
#define AIRTOUCH_TOF_SCL_PIN D1

#define AIRTOUCH_TOF_MIN_VALID_MM 30
#define AIRTOUCH_TOF_MAX_VALID_MM 1800

#define AIRTOUCH_GUARD_ON_THRESHOLD_MM 250
#define AIRTOUCH_GUARD_OFF_THRESHOLD_MM 320
#define AIRTOUCH_GUARD_STABLE_COUNT_REQUIRED 3
#define AIRTOUCH_GUARD_SAMPLE_INTERVAL_MS 120

static VL53L0X s_airtouch_tof_v1c;

static bool s_airtouch_tof_ready_v1c = false;
static bool s_airtouch_guard_on_v1c = false;
static bool s_airtouch_guard_state_initialized_v1c = false;

static int s_airtouch_guard_candidate_state_v1c = -1;
static int s_airtouch_guard_candidate_count_v1c = 0;

static uint32_t s_airtouch_guard_seq_v1c = 0;
static uint32_t s_airtouch_guard_last_sample_ms_v1c = 0;

static bool airtouch_tof_guard_is_valid_mm_v1c(uint16_t mm)
{
    // For AirTouch distance protection, very large VL53L0X values such as 8191
    // mean "far enough / safe", not invalid.
    // Only zero is treated as unusable here. Timeout is checked outside.
    if (mm == 0) return false;
    return true;
}

static void airtouch_tof_guard_send_event_v1c(bool guard_on, uint16_t mm, const char *reason)
{
    s_airtouch_guard_seq_v1c++;

    char line[96];
    snprintf(line,
             sizeof(line),
             "GUARD,%lu,%d,%u,%s",
             (unsigned long)s_airtouch_guard_seq_v1c,
             guard_on ? 1 : 0,
             (unsigned int)mm,
             reason ? reason : "NA");

    p4Serial.println(line);

    Serial.print("[TOF GUARD v1c] TX to P4: ");
    Serial.println(line);
}

static void airtouch_tof_guard_update_state_v1c(uint16_t mm, bool valid)
{
    if (!valid) {
        return;
    }

    int desired = -1;

    if (mm < AIRTOUCH_GUARD_ON_THRESHOLD_MM) {
        desired = 1;
    } else if (mm > AIRTOUCH_GUARD_OFF_THRESHOLD_MM) {
        desired = 0;
    } else {
        return;
    }

    if (!s_airtouch_guard_state_initialized_v1c) {
        s_airtouch_guard_on_v1c = (desired == 1);
        s_airtouch_guard_state_initialized_v1c = true;
        s_airtouch_guard_candidate_state_v1c = -1;
        s_airtouch_guard_candidate_count_v1c = 0;

        airtouch_tof_guard_send_event_v1c(
            s_airtouch_guard_on_v1c,
            mm,
            s_airtouch_guard_on_v1c ? "INIT_TOO_CLOSE" : "INIT_SAFE"
        );
        return;
    }

    if ((desired == 1) == s_airtouch_guard_on_v1c) {
        s_airtouch_guard_candidate_state_v1c = -1;
        s_airtouch_guard_candidate_count_v1c = 0;
        return;
    }

    if (s_airtouch_guard_candidate_state_v1c != desired) {
        s_airtouch_guard_candidate_state_v1c = desired;
        s_airtouch_guard_candidate_count_v1c = 1;
    } else {
        s_airtouch_guard_candidate_count_v1c++;
    }

    if (s_airtouch_guard_candidate_count_v1c >= AIRTOUCH_GUARD_STABLE_COUNT_REQUIRED) {
        s_airtouch_guard_on_v1c = (desired == 1);
        s_airtouch_guard_candidate_state_v1c = -1;
        s_airtouch_guard_candidate_count_v1c = 0;

        airtouch_tof_guard_send_event_v1c(
            s_airtouch_guard_on_v1c,
            mm,
            s_airtouch_guard_on_v1c ? "TOO_CLOSE" : "SAFE_AGAIN"
        );
    }
}

static void airtouch_tof_guard_init_v1c(void)
{
    Serial.println("[TOF GUARD v1c] init begin");
    Serial.println("[TOF GUARD v1c] Wiring: VL53L0X SDA=D2/GPIO4, SCL=D1/GPIO5, VCC=3V3, GND=GND, XSHUT=3V3");

    Wire.begin(AIRTOUCH_TOF_SDA_PIN, AIRTOUCH_TOF_SCL_PIN);
    Wire.setClock(100000);

    s_airtouch_tof_v1c.setTimeout(120);

    if (!s_airtouch_tof_v1c.init()) {
        s_airtouch_tof_ready_v1c = false;
        Serial.println("[TOF GUARD v1c] VL53L0X init failed");
        airtouch_tof_guard_send_event_v1c(true, 0, "INIT_FAIL");
        return;
    }

    s_airtouch_tof_v1c.startContinuous(AIRTOUCH_GUARD_SAMPLE_INTERVAL_MS);

    s_airtouch_tof_ready_v1c = true;
    s_airtouch_guard_last_sample_ms_v1c = millis();

    Serial.println("[TOF GUARD v1c] VL53L0X init OK");
    Serial.println("[TOF GUARD v1c] event-only guard enabled");
}

static void airtouch_tof_guard_tick_v1c(void)
{
    if (!s_airtouch_tof_ready_v1c) {
        return;
    }

    uint32_t now = millis();
    if ((uint32_t)(now - s_airtouch_guard_last_sample_ms_v1c) < AIRTOUCH_GUARD_SAMPLE_INTERVAL_MS) {
        return;
    }
    s_airtouch_guard_last_sample_ms_v1c = now;

    uint16_t mm = s_airtouch_tof_v1c.readRangeContinuousMillimeters();
    bool timeout = s_airtouch_tof_v1c.timeoutOccurred();
    bool valid = (!timeout) && airtouch_tof_guard_is_valid_mm_v1c(mm);

    airtouch_tof_guard_update_state_v1c(mm, valid);
}
// ================= AirTouch ESP8266 ToF Guard Merge v1c.2 END =================



String p4Line;
String usbLine;

const char *CF_IPS[] = {
  "104.16.0.1",
  "104.18.0.1",
  "104.21.0.1",
  "188.114.96.1",
  "162.159.140.1"
};

static void connectWiFi()
{
  Serial.print("Connecting WiFi: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("Gateway: ");
    Serial.println(WiFi.gatewayIP());
    Serial.print("DNS: ");
    Serial.println(WiFi.dnsIP());
    Serial.print("RSSI: ");
    Serial.println(WiFi.RSSI());
    Serial.print("Free heap after WiFi: ");
    Serial.println(ESP.getFreeHeap());
  } else {
    Serial.println("WiFi connect timeout");
  }
}

static bool ensureWiFi()
{
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  Serial.println("WiFi disconnected, reconnecting...");
  connectWiFi();

  return WiFi.status() == WL_CONNECTED;
}

static void sendAckToP4(const String &seq)
{
  p4Serial.print("ACK:");
  p4Serial.println(seq);

  Serial.print("TX to P4: ACK:");
  Serial.println(seq);
}

static void sendNackToP4(const String &seq)
{
  p4Serial.print("NACK:");
  p4Serial.println(seq);

  Serial.print("TX to P4: NACK:");
  Serial.println(seq);
}

static int parseHttpStatus(const String &statusLine)
{
  if (!statusLine.startsWith("HTTP/1.1 ")) {
    return -1;
  }

  String codeText = statusLine.substring(9, 12);
  return codeText.toInt();
}

static bool postJsonViaCloudflareIp(const char *ipText, const String &json)
{
  IPAddress ip;
  if (!ip.fromString(ipText)) {
    Serial.print("Bad Cloudflare IP: ");
    Serial.println(ipText);
    return false;
  }

  WiFiClient client;
  client.setTimeout(12000);

  Serial.println();
  Serial.println("==== Cloudflare raw IP HTTP POST ====");
  Serial.print("Cloudflare IP: ");
  Serial.println(ipText);
  Serial.print("Host: ");
  Serial.println(WORKER_HOST);
  Serial.print("Path: ");
  Serial.println(WORKER_PATH);
  Serial.print("JSON length: ");
  Serial.println(json.length());
  Serial.print("Free heap before POST: ");
  Serial.println(ESP.getFreeHeap());

  unsigned long start = millis();
  bool connected = client.connect(ip, 80);
  unsigned long used = millis() - start;

  Serial.print("TCP connect result: ");
  Serial.println(connected ? "OK" : "FAIL");
  Serial.print("TCP connect ms: ");
  Serial.println(used);

  if (!connected) {
    client.stop();
    return false;
  }

  client.print("POST ");
  client.print(WORKER_PATH);
  client.print(" HTTP/1.1\r\n");

  client.print("Host: ");
  client.print(WORKER_HOST);
  client.print("\r\n");

  client.print("User-Agent: AirTouch-ESP8266-v1f\r\n");
  client.print("Content-Type: application/json\r\n");

  client.print("X-AirTouch-Token: ");
  client.print(DEVICE_TOKEN);
  client.print("\r\n");

  client.print("Content-Length: ");
  client.print(json.length());
  client.print("\r\n");

  client.print("Connection: close\r\n");
  client.print("\r\n");

  client.print(json);

  Serial.println("HTTP POST request sent.");

  unsigned long waitStart = millis();
  while (!client.available() && millis() - waitStart < 12000) {
    delay(10);
  }

  if (!client.available()) {
    Serial.println("No HTTP response");
    client.stop();
    return false;
  }

  String statusLine = client.readStringUntil('\n');
  statusLine.trim();

  Serial.print("HTTP status line: ");
  Serial.println(statusLine);

  int status = parseHttpStatus(statusLine);

  String responsePreview;
  unsigned long readStart = millis();
  while (client.connected() && millis() - readStart < 5000) {
    while (client.available()) {
      char c = (char)client.read();
      if (responsePreview.length() < 900) {
        responsePreview += c;
      }
    }
    delay(5);
  }

  while (client.available()) {
    char c = (char)client.read();
    if (responsePreview.length() < 900) {
      responsePreview += c;
    }
  }

  client.stop();

  Serial.print("HTTP status code: ");
  Serial.println(status);

  if (responsePreview.length() > 0) {
    Serial.println("Response preview:");
    Serial.println(responsePreview);
    Serial.println("---- END RESPONSE PREVIEW ----");
  }

  Serial.print("Free heap after POST: ");
  Serial.println(ESP.getFreeHeap());

  return status >= 200 && status < 300;
}

static bool postJsonToCloudflare(const String &json)
{
  if (!ensureWiFi()) {
    Serial.println("POST failed: WiFi not connected");
    return false;
  }

  for (unsigned int i = 0; i < sizeof(CF_IPS) / sizeof(CF_IPS[0]); ++i) {
    bool ok = postJsonViaCloudflareIp(CF_IPS[i], json);
    if (ok) {
      Serial.print("Cloudflare POST OK via IP: ");
      Serial.println(CF_IPS[i]);
      return true;
    }

    Serial.print("Cloudflare POST failed via IP: ");
    Serial.println(CF_IPS[i]);
    delay(300);
  }

  Serial.println("All Cloudflare IP POST attempts failed.");
  return false;
}


// -----------------------------------------------------------------------------
// Cloud-SD v2i CFGACK cloud ack
//
// P4 sends:
//   CFGACK,version=1,ok=1
//
// ESP8266 posts:
//   POST /api/airtouch/config/ack
// -----------------------------------------------------------------------------

static const char *AIRTOUCH_CFG_ACK_HOST_V2I = "airtouch-cloud-worker.zyx-airtouch-cloud.workers.dev";
static const char *AIRTOUCH_CFG_ACK_PATH_V2I = "/api/airtouch/config/ack";
static const char *AIRTOUCH_CFG_ACK_TOKEN_V2I = "airtouch_demo_token_2026";

static int s_airtouch_cfg_ack_posted_version_v2i = -1;

static int airtouchLineIntV2I(const String &line, const String &key, int fallback)
{
  int k = line.indexOf(key + "=");

  if (k < 0) {
    return fallback;
  }

  int i = k + key.length() + 1;

  bool neg = false;

  if (i < (int)line.length() && line[i] == '-') {
    neg = true;
    i++;
  }

  long value = 0;
  bool hasDigit = false;

  while (i < (int)line.length() && line[i] >= '0' && line[i] <= '9') {
    hasDigit = true;
    value = value * 10 + (line[i] - '0');
    i++;
  }

  if (!hasDigit) {
    return fallback;
  }

  return neg ? -value : value;
}

static bool airtouchPostConfigAckViaIpV2I(const char *ipText, int version)
{
  if (!ipText || !ipText[0] || version <= 0) {
    return false;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[CFG v2i] WiFi not connected, cannot POST config ack");
    return false;
  }

  WiFiClient client;
  client.setTimeout(8000);

  Serial.print("[CFG v2i] POST config/ack via IP ");
  Serial.println(ipText);

  if (!client.connect(ipText, 80)) {
    Serial.print("[CFG v2i] Connect failed: ");
    Serial.println(ipText);
    return false;
  }

  String body = "{";
  body += "\"device_id\":\"airtouch_001\",";
  body += "\"user_id\":\"child_001\",";
  body += "\"applied_version\":" + String(version) + ",";
  body += "\"config_version\":" + String(version);
  body += "}";

  client.print(String("POST ") + AIRTOUCH_CFG_ACK_PATH_V2I + " HTTP/1.1\r\n" +
               "Host: " + AIRTOUCH_CFG_ACK_HOST_V2I + "\r\n" +
               "User-Agent: AirTouch-ESP8266-v2i\r\n" +
               "Content-Type: application/json\r\n" +
               "Accept: application/json\r\n" +
               "X-AirTouch-Token: " + AIRTOUCH_CFG_ACK_TOKEN_V2I + "\r\n" +
               "Content-Length: " + String(body.length()) + "\r\n" +
               "Connection: close\r\n\r\n" +
               body);

  String statusLine = client.readStringUntil('\n');
  statusLine.trim();

  Serial.print("[CFG v2i] HTTP status: ");
  Serial.println(statusLine);

  if (statusLine.indexOf("200") < 0 && statusLine.indexOf("201") < 0) {
    Serial.println("[CFG v2i] HTTP status not success");
    client.stop();
    return false;
  }

  bool headerDone = false;
  unsigned long startMs = millis();

  while ((client.connected() || client.available()) && millis() - startMs < 8000) {
    String line = client.readStringUntil('\n');
    line.trim();

    if (line.length() == 0) {
      headerDone = true;
      break;
    }

    delay(1);
  }

  String resp;
  startMs = millis();

  while ((client.connected() || client.available()) && millis() - startMs < 8000) {
    while (client.available()) {
      char ch = (char)client.read();
      if (resp.length() < 512) {
        resp += ch;
      }
    }
    delay(1);
  }

  client.stop();

  Serial.print("[CFG v2i] Response length: ");
  Serial.println(resp.length());

  if (resp.length() > 0) {
    Serial.print("[CFG v2i] Response head: ");
    Serial.println(resp.substring(0, 180));
  }

  if (resp.indexOf("\"ok\":true") >= 0 || resp.indexOf("\"ok\": true") >= 0 || headerDone) {
    Serial.print("[CFG v2i] Cloud config ack posted, applied_version=");
    Serial.println(version);
    return true;
  }

  Serial.println("[CFG v2i] Cloud config ack response not OK");
  return false;
}

static bool airtouchPostConfigAckV2I(int version)
{
  if (version <= 0) {
    Serial.println("[CFG v2i] Invalid version, skip cloud ack");
    return false;
  }

  if (s_airtouch_cfg_ack_posted_version_v2i == version) {
    Serial.print("[CFG v2i] Cloud ack already posted for version v");
    Serial.println(version);
    return true;
  }

  const size_t ipCount = sizeof(CF_IPS) / sizeof(CF_IPS[0]);

  for (size_t i = 0; i < ipCount; ++i) {
    if (airtouchPostConfigAckViaIpV2I(CF_IPS[i], version)) {
      s_airtouch_cfg_ack_posted_version_v2i = version;
      return true;
    }

    delay(300);
  }

  Serial.print("[CFG v2i] All config/ack attempts failed for version v");
  Serial.println(version);
  return false;
}

static void airtouchHandleCfgAckV2I(String line)
{
  line.trim();

  Serial.print("[CFG v2i] RX from P4: ");
  Serial.println(line);

  if (!line.startsWith("CFGACK,")) {
    return;
  }

  const int version = airtouchLineIntV2I(line, "version", 0);
  const int ok = airtouchLineIntV2I(line, "ok", 0);

  Serial.print("[CFG v2i] Parsed CFGACK version=");
  Serial.print(version);
  Serial.print(" ok=");
  Serial.println(ok);

  if (version <= 0 || ok != 1) {
    Serial.println("[CFG v2i] CFGACK not OK, skip cloud ack");
    return;
  }

  airtouchPostConfigAckV2I(version);
}

static void processAtqLine(String line, bool fromP4)
{
  // Cloud-SD v2i: handle CFGACK before ATQ
  line.trim();

  if (fromP4 && line.startsWith("CFGACK,")) {
    airtouchHandleCfgAckV2I(line);
    return;
  }
  // Cloud-SD v2i.1: normalize P4 comma-style ATQ to legacy colon-style parser.
  // P4 current format:
  //   ATQ,200007,{...}
  // Legacy ESP8266 parser may expect:
  //   ATQ:200007:{...}
  if (fromP4 && line.startsWith("ATQ,")) {
    int firstComma = line.indexOf(',');
    int secondComma = line.indexOf(',', firstComma + 1);

    if (firstComma > 0 && secondComma > firstComma) {
      String seqText = line.substring(firstComma + 1, secondComma);
      String jsonText = line.substring(secondComma + 1);

      seqText.trim();
      jsonText.trim();

      line = "ATQ:" + seqText + ":" + jsonText;

      Serial.print("[ATQ v2i.1] normalized comma ATQ to legacy parser, seq=");
      Serial.println(seqText);
    } else {
      Serial.print("[ATQ v2i.1] bad comma ATQ line: ");
      Serial.println(line);
    }
  }

  line.trim();

  if (line.length() == 0) {
    return;
  }

  Serial.print(fromP4 ? "RX from P4: " : "RX from USB: ");
  Serial.println(line);

  if (!line.startsWith("ATQ:")) {
    Serial.println("Bad line: missing ATQ prefix");
    if (fromP4) {
      sendNackToP4("0");
    }
    return;
  }

  int secondColon = line.indexOf(':', 4);
  if (secondColon < 0) {
    Serial.println("Bad line: missing second colon");
    if (fromP4) {
      sendNackToP4("0");
    }
    return;
  }

  String seq = line.substring(4, secondColon);
  String payload = line.substring(secondColon + 1);
  payload.trim();

  if (seq.length() == 0 || !payload.startsWith("{")) {
    Serial.println("Bad line: invalid seq or json");
    if (fromP4) {
      sendNackToP4(seq.length() ? seq : "0");
    }
    return;
  }

  bool ok = postJsonToCloudflare(payload);

  if (ok) {
    Serial.println("ATQ_POST_PASS");

    if (fromP4) {
      sendAckToP4(seq);
    } else {
      Serial.print("ACK:");
      Serial.println(seq);
    }
  } else {
    Serial.println("ATQ_POST_FAIL");

    if (fromP4) {
      sendNackToP4(seq);
    } else {
      Serial.print("NACK:");
      Serial.println(seq);
    }
  }
}

static void readLineFromStream(Stream &stream, String &buffer, bool fromP4)
{
  while (stream.available() > 0) {
    char c = (char)stream.read();

    if (c == '\r') {
      continue;
    }

    if (c == '\n') {
      processAtqLine(buffer, fromP4);
      buffer = "";
      continue;
    }

    if (buffer.length() < 1800) {
      buffer += c;
    } else {
      Serial.println("Line buffer overflow, clearing");
      buffer = "";
    }
  }
}

void setup()
{
  Serial.begin(USB_BAUD);
  delay(500);

  Serial.println();
  Serial.println("AirTouch ESP8266 Cloud v1f boot");
  Serial.println("Mode: P4 UART -> ESP8266 WiFi -> Cloudflare fixed IP HTTP -> Worker -> D1");
  Serial.print("Worker host: ");
  Serial.println(WORKER_HOST);
  Serial.print("Worker path: ");
  Serial.println(WORKER_PATH);
  Serial.println("P4 UART: D5/GPIO14=RX, D6/GPIO12=TX, baud=9600");
  Serial.print("Free heap at boot: ");
  Serial.println(ESP.getFreeHeap());

  p4Line.reserve(1800);
  usbLine.reserve(1800);

  p4Serial.begin(P4_BAUD);

  connectWiFi();

  Serial.println();
  Serial.println("Ready for ATQ lines from P4.");

  // AirTouch ToF Guard v1c init: keep cloud logic, add distance guard
  airtouch_tof_guard_init_v1c();
}


// -----------------------------------------------------------------------------
// Cloud-SD v2g-a config pull
//
// Purpose:
//   ESP8266 pulls latest cloud config from Worker and prints parsed values.
//   This version does NOT send config to P4 yet.
// -----------------------------------------------------------------------------

static const char *AIRTOUCH_CFG_HOST_V2G = "airtouch-cloud-worker.zyx-airtouch-cloud.workers.dev";
static const char *AIRTOUCH_CFG_PATH_V2G =
  "/api/airtouch/config/latest?device_id=airtouch_001&user_id=child_001";

static int airtouchJsonIntAfterV2G(const String &s,
                                   const String &section,
                                   const String &key,
                                   int fallback)
{
  int start = 0;

  if (section.length() > 0) {
    start = s.indexOf(section);
    if (start < 0) {
      return fallback;
    }
  }

  int k = s.indexOf(key, start);
  if (k < 0) {
    return fallback;
  }

  int colon = s.indexOf(':', k);
  if (colon < 0) {
    return fallback;
  }

  int i = colon + 1;
  while (i < (int)s.length() && (s[i] == ' ' || s[i] == '"' || s[i] == '\r' || s[i] == '\n')) {
    i++;
  }

  bool neg = false;
  if (i < (int)s.length() && s[i] == '-') {
    neg = true;
    i++;
  }

  long value = 0;
  bool hasDigit = false;

  while (i < (int)s.length() && s[i] >= '0' && s[i] <= '9') {
    hasDigit = true;
    value = value * 10 + (s[i] - '0');
    i++;
  }

  if (!hasDigit) {
    return fallback;
  }

  return neg ? -value : value;
}

static bool airtouchJsonBoolAfterV2G(const String &s,
                                     const String &section,
                                     const String &key,
                                     bool fallback)
{
  int start = 0;

  if (section.length() > 0) {
    start = s.indexOf(section);
    if (start < 0) {
      return fallback;
    }
  }

  int k = s.indexOf(key, start);
  if (k < 0) {
    return fallback;
  }

  int colon = s.indexOf(':', k);
  if (colon < 0) {
    return fallback;
  }

  int i = colon + 1;
  while (i < (int)s.length() && (s[i] == ' ' || s[i] == '"' || s[i] == '\r' || s[i] == '\n')) {
    i++;
  }

  if (s.startsWith("true", i)) {
    return true;
  }

  if (s.startsWith("false", i)) {
    return false;
  }

  if (i < (int)s.length() && s[i] == '1') {
    return true;
  }

  if (i < (int)s.length() && s[i] == '0') {
    return false;
  }

  return fallback;
}


// -----------------------------------------------------------------------------
// Cloud-SD v2g-b CFG UART downlink
//
// Purpose:
//   Send parsed cloud config to P4 as a simple CFG key=value line.
//   P4 will only print it in v2g-b; it will not apply/write CONFIG.TXT yet.
// -----------------------------------------------------------------------------

static int s_airtouch_cfg_sent_version_v2gb = -1;

static void airtouchSendConfigToP4V2GB(int configVersion,
                                       int starTarget,
                                       int starDwell,
                                       int starDuration,
                                       int starDifficulty,
                                       bool starAdaptive,
                                       int colorTarget,
                                       int colorDwell,
                                       int colorBubble,
                                       int colorNogo,
                                       int colorDuration,
                                       int colorDifficulty,
                                       bool colorAdaptive)
{
  if (configVersion <= 0) {
    Serial.println("[CFG v2g-b] Skip CFG downlink: invalid config_version");
    return;
  }

  if (s_airtouch_cfg_sent_version_v2gb == configVersion) {
    Serial.print("[CFG v2g-b] CFG version already sent, skip duplicate: v");
    Serial.println(configVersion);
    return;
  }

  String line = "CFG";
  line += ",version=" + String(configVersion);
  line += ",star_target=" + String(starTarget);
  line += ",star_dwell=" + String(starDwell);
  line += ",star_duration=" + String(starDuration);
  line += ",star_difficulty=" + String(starDifficulty);
  line += ",star_adaptive=" + String(starAdaptive ? 1 : 0);
  line += ",color_target=" + String(colorTarget);
  line += ",color_dwell=" + String(colorDwell);
  line += ",color_bubble=" + String(colorBubble);
  line += ",color_nogo=" + String(colorNogo);
  line += ",color_duration=" + String(colorDuration);
  line += ",color_difficulty=" + String(colorDifficulty);
  line += ",color_adaptive=" + String(colorAdaptive ? 1 : 0);

  p4Serial.println(line);

  Serial.print("[CFG v2g-b] TX to P4: ");
  Serial.println(line);

  s_airtouch_cfg_sent_version_v2gb = configVersion;
}

static bool airtouchFetchConfigLatestFromIpV2G(const char *ipText)
{
  if (!ipText || !ipText[0]) {
    return false;
  }

  WiFiClient client;
  client.setTimeout(8000);

  Serial.print("[CFG v2g-a] Connecting Worker config/latest via IP ");
  Serial.println(ipText);

  if (!client.connect(ipText, 80)) {
    Serial.print("[CFG v2g-a] Connect failed: ");
    Serial.println(ipText);
    return false;
  }

  client.print(String("GET ") + AIRTOUCH_CFG_PATH_V2G + " HTTP/1.1\r\n" +
               "Host: " + AIRTOUCH_CFG_HOST_V2G + "\r\n" +
               "User-Agent: AirTouch-ESP8266-v2g-a\r\n" +
               "Accept: application/json\r\n" +
               "Connection: close\r\n\r\n");

  String statusLine = client.readStringUntil('\n');
  statusLine.trim();

  Serial.print("[CFG v2g-a] HTTP status: ");
  Serial.println(statusLine);

  if (statusLine.indexOf("200") < 0) {
    Serial.println("[CFG v2g-a] HTTP not 200");
    client.stop();
    return false;
  }

  bool headerDone = false;
  unsigned long startMs = millis();

  while (client.connected() && millis() - startMs < 8000) {
    String line = client.readStringUntil('\n');
    line.trim();

    if (line.length() == 0) {
      headerDone = true;
      break;
    }
  }

  if (!headerDone) {
    Serial.println("[CFG v2g-a] Header timeout");
    client.stop();
    return false;
  }

  String body;
  startMs = millis();

  while ((client.connected() || client.available()) && millis() - startMs < 10000) {
    while (client.available()) {
      char ch = (char)client.read();
      if (body.length() < 8192) {
        body += ch;
      }
    }
    delay(1);
  }

  client.stop();

  Serial.print("[CFG v2g-a] Body length: ");
  Serial.println(body.length());

  if (body.indexOf("\"ok\":true") < 0 && body.indexOf("\"ok\": true") < 0) {
    Serial.println("[CFG v2g-a] Config response not OK");
    Serial.println(body.substring(0, 300));
    return false;
  }

  const int configVersion = airtouchJsonIntAfterV2G(body, "", "\"config_version\"", -1);
  const int appliedVersion = airtouchJsonIntAfterV2G(body, "", "\"applied_version\"", 0);

  const int starTarget = airtouchJsonIntAfterV2G(body, "\"star\"", "\"target_radius\"", 56);
  const int starDwell = airtouchJsonIntAfterV2G(body, "\"star\"", "\"dwell_ms\"", 336);
  const int starDuration = airtouchJsonIntAfterV2G(body, "\"star\"", "\"round_duration_s\"", 45);
  const int starDifficulty = airtouchJsonIntAfterV2G(body, "\"star\"", "\"difficulty\"", 2);
  const bool starAdaptive = airtouchJsonBoolAfterV2G(body, "\"star\"", "\"adaptive_enabled\"", true);

  const int colorTarget = airtouchJsonIntAfterV2G(body, "\"color_go\"", "\"target_radius\"", 54);
  const int colorDwell = airtouchJsonIntAfterV2G(body, "\"color_go\"", "\"dwell_ms\"", 520);
  const int colorBubble = airtouchJsonIntAfterV2G(body, "\"color_go\"", "\"bubble_count\"", 4);
  const int colorNogo = airtouchJsonIntAfterV2G(body, "\"color_go\"", "\"nogo_ratio\"", 25);
  const int colorDuration = airtouchJsonIntAfterV2G(body, "\"color_go\"", "\"round_duration_s\"", 45);
  const int colorDifficulty = airtouchJsonIntAfterV2G(body, "\"color_go\"", "\"difficulty\"", 2);
  const bool colorAdaptive = airtouchJsonBoolAfterV2G(body, "\"color_go\"", "\"adaptive_enabled\"", true);

  Serial.println("[CFG v2g-a] ===== Cloud config latest =====");
  Serial.print("[CFG v2g-a] config_version=");
  Serial.print(configVersion);
  Serial.print(" applied_version=");
  Serial.println(appliedVersion);

  Serial.print("[CFG v2g-a] STAR target=");
  Serial.print(starTarget);
  Serial.print(" dwell=");
  Serial.print(starDwell);
  Serial.print(" duration=");
  Serial.print(starDuration);
  Serial.print(" difficulty=");
  Serial.print(starDifficulty);
  Serial.print(" adaptive=");
  Serial.println(starAdaptive ? 1 : 0);

  Serial.print("[CFG v2g-a] COLOR target=");
  Serial.print(colorTarget);
  Serial.print(" dwell=");
  Serial.print(colorDwell);
  Serial.print(" bubble=");
  Serial.print(colorBubble);
  Serial.print(" nogo=");
  Serial.print(colorNogo);
  Serial.print(" duration=");
  Serial.print(colorDuration);
  Serial.print(" difficulty=");
  Serial.print(colorDifficulty);
  Serial.print(" adaptive=");
  Serial.println(colorAdaptive ? 1 : 0);

  airtouchSendConfigToP4V2GB(configVersion,
                             starTarget,
                             starDwell,
                             starDuration,
                             starDifficulty,
                             starAdaptive,
                             colorTarget,
                             colorDwell,
                             colorBubble,
                             colorNogo,
                             colorDuration,
                             colorDifficulty,
                             colorAdaptive);

  Serial.println("[CFG v2g-b] =================================");

  return true;
}

static bool airtouchFetchConfigLatestV2G()
{
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[CFG v2g-a] WiFi not connected, skip config pull");
    return false;
  }

  const size_t ipCount = sizeof(CF_IPS) / sizeof(CF_IPS[0]);

  for (size_t i = 0; i < ipCount; ++i) {
    if (airtouchFetchConfigLatestFromIpV2G(CF_IPS[i])) {
      return true;
    }

    delay(300);
  }

  Serial.println("[CFG v2g-a] All Cloudflare IP config/latest attempts failed");
  return false;
}
static void airtouchConfigPollTickV2G()
{
  static bool fetchedOnce = false;
  static unsigned long lastAttemptMs = 0;

  const unsigned long now = millis();
  const unsigned long intervalMs = fetchedOnce ? 60000UL : 5000UL;

  if (lastAttemptMs != 0 && now - lastAttemptMs < intervalMs) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  lastAttemptMs = now;

  if (airtouchFetchConfigLatestV2G()) {
    fetchedOnce = true;
  }
}

void loop()
{
  // AirTouch ToF Guard v1c tick: event-only, non-repeating
  airtouch_tof_guard_tick_v1c();

  airtouchConfigPollTickV2G();
  readLineFromStream(p4Serial, p4Line, true);
  readLineFromStream(Serial, usbLine, false);

  delay(5);
}








