#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include <LittleFS.h>

// ================== microSD PIN - ESP32-S3 ==================
#define SD_CS 10
#define SD_SCK 12
#define SD_MOSI 11
#define SD_MISO 13

#define SD_FORMAT_IF_EMPTY false

SPIClass spiSD(FSPI);

uint32_t mountedSDFreq = 0;

// ================== LED PIN - ESP32-S3 ==================
#define LED_RED 21
#define LED_GREEN 47
#define LED_WHITE 48

// ================== WIFI AP CONFIG ==================
const char *AP_SSID = "1A3bcba8b69c7aea0c5942b5ce87f102";
const char *AP_PASS = "d1b07167d06adf161a21d4096041d40b";

IPAddress local_IP(10, 42, 0, 1);
IPAddress gateway(10, 42, 0, 1);
IPAddress subnet(255, 255, 255, 0);

#define SERVER_PORT 8765
WebServer server(SERVER_PORT);

// ================== HANDSHAKE SSID LIST (BINARY FILE O(1) SEEK) ==================
#define SSID_RECORD_SIZE 64
#define SSID_LIST_MAX 100000UL
uint32_t g_ssidCount = 0;
String g_currentApSsid = "";
String g_currentApPass = "";
int32_t g_lockedIndex = -1;        // -1 = đang ở SSID handshake mặc định
uint32_t g_lockStartMs = 0;
bool g_handshakeDone = false;     // true khi đã có R3c STA connect vào SSID động
#define HANDSHAKE_LOCK_TIMEOUT_MS 30000UL

// ================== TRUSTED WIFI CONFIG ==================
// AP nay la WiFi an: beacon khong mang SSID nen scanNetworks() tra ve ten rong.
// Khong duoc dung ket qua scan de kiem tra ton tai; phai WiFi.begin() truc tiep.
#define TRUSTED_SSID "1A00a60d349d091982c62e3b7561c38f"
#define TRUSTED_PASS "afd4fa706f49d71cc98d2a7a87284a7a"
#define SCAN_INTERVAL_MS 30000UL
#define STA_CONNECT_TIMEOUT_MS 10000UL
#define AP_CHANNEL 1
#define AP_HIDDEN true
#define AP_MAX_CONN 2

// ================== LED STATE ==================
// Quy ước LED: mỗi thời điểm chỉ có đúng 1 trạng thái LED.
// IDLE      = Tắt hết: không có kết nối
// CONNECTED = Xanh: có WiFi client / USB CDC / STA connected
// TRANSFER  = Đỏ: đang upload / download file
// ERROR     = Đỏ nháy: lỗi SD / lỗi WiFi AP
enum LedState
{
  LED_STATE_IDLE,      // Tắt hết
  LED_STATE_CONNECTED, // Xanh
  LED_STATE_TRANSFER,  // Đỏ liên tục
  LED_STATE_ERROR      // Đỏ nháy, dùng trong vòng lặp lỗi
};

volatile LedState g_ledState = LED_STATE_IDLE;
bool transferLedActive = false;
uint8_t lastClientCount = 0;

// ================== WIFI MODE STATE ==================
enum WifiMode
{
  MODE_AP,
  MODE_STA
};
WifiMode currentMode = MODE_AP;
unsigned long lastScanMs = 0;

// ================== UPLOAD STATE ==================
File uploadFile;
String uploadFileName = "";
String uploadFilePath = "";
bool uploadOK = false;
String uploadError = "";
uint64_t uploadBytes = 0;
uint32_t uploadStartMs = 0;

// ── serial chunked upload state ──────────────────────────────────────────────
File _serialUpFile;
String _serialUpName = "";
size_t _serialUpTotal = 0;
size_t _serialUpWritten = 0;
uint32_t _serialUpStartMs = 0;

// Serial upload: decode từng chunk nhỏ rồi ghi thẳng xuống SD.
// Không cấp phát RAM theo kích thước file/chunk nữa.
#define SERIAL_B64_MAX_LEN 12288    // base64 tối đa mỗi dòng JSON (~9KB dữ liệu gốc)
#define SERIAL_DECODE_BUF_SIZE 9216 // buffer decode cố định
static uint8_t _serialDecodeBuf[SERIAL_DECODE_BUF_SIZE];

// ── serial chunked download state ────────────────────────────────────────────
File _serialDownFile;
String _serialDownName = "";
size_t _serialDownSize = 0;
size_t _serialDownRead = 0;
uint32_t _serialDownChunks = 0;
uint32_t _serialDownStartMs = 0;
#define SERIAL_DOWN_CHUNK 12288 // bytes per chunk → base64 ~16384 chars

// ================== UPLOAD ALL STATE ==================
bool uploadAllActive = false;
uint32_t uploadAllOK = 0;
uint32_t uploadAllFailed = 0;
uint64_t uploadAllBytes = 0;
String uploadAllErrors = "";

// ================== UTILS ==================
String humanSize(uint64_t bytes)
{
  if (bytes < 1024)
    return String((unsigned long long)bytes) + " B";
  if (bytes < 1024ULL * 1024ULL)
    return String((double)bytes / 1024.0, 1) + " KB";
  return String((double)bytes / 1024.0 / 1024.0, 2) + " MB";
}

String jsonEscape(const String &s)
{
  String out = "";
  for (size_t i = 0; i < s.length(); i++)
  {
    char c = s[i];
    if (c == '"')
      out += "\\\"";
    else if (c == '\\')
      out += "\\\\";
    else if (c == '\n')
      out += "\\n";
    else if (c == '\r')
      out += "\\r";
    else
      out += c;
  }
  return out;
}

// ================== TRANSFER LOG ==================
void logTransfer(const char *type, const String &name, uint64_t bytes, float sec, float kbs, const char *status)
{
  bool writeHeader = !SD.exists("/transfer_log.csv");
  File f = SD.open("/transfer_log.csv", FILE_APPEND);
  if (!f)
    return;
  if (writeHeader)
    f.println("uptime_ms,type,filename,bytes,duration_s,speed_kbs,status");
  f.printf("%lu,%s,%s,%llu,%.2f,%.1f,%s\n",
           (unsigned long)millis(), type, name.c_str(),
           (unsigned long long)bytes, sec, kbs, status);
  f.close();
}

// ================== DINH DANH ==================
struct DinhDanh
{
  String ma_id = "BN-0001";
  String ho_ten = "Nguyen Ngoc Hai";
  int nam_sinh = 1990;
  String dia_chi = "Ha Noi";
  String ngay_cap_dinh_danh = "2026-05-30";
  String ten_thiet_bi = "PhantomR3b";
  String id_thiet_bi = "001";
  String dia_chi_thiet_bi = "Ha Noi";
};

DinhDanh dinhDanh;

static String _jStr(const String &j, const String &key)
{
  int p = j.indexOf("\"" + key + "\"");
  if (p < 0)
    return "";
  p = j.indexOf(':', p + key.length() + 2);
  if (p < 0)
    return "";
  p++;
  while (p < (int)j.length() && (j[p] == ' ' || j[p] == '\t'))
    p++;
  if (j[p] != '"')
    return "";
  p++;
  String v = "";
  while (p < (int)j.length() && j[p] != '"')
  {
    if (j[p] == '\\' && p + 1 < (int)j.length())
    {
      p++;
      v += j[p];
    }
    else
      v += j[p];
    p++;
  }
  return v;
}

static int _jInt(const String &j, const String &key)
{
  int p = j.indexOf("\"" + key + "\"");
  if (p < 0)
    return -1;
  p = j.indexOf(':', p + key.length() + 2);
  if (p < 0)
    return -1;
  p++;
  while (p < (int)j.length() && (j[p] == ' ' || j[p] == '\t'))
    p++;
  String v = "";
  while (p < (int)j.length() && j[p] >= '0' && j[p] <= '9')
    v += j[p++];
  return v.length() ? v.toInt() : -1;
}

void loadDinhDanhFromSD()
{
  File f = SD.open("/dinh_danh.json", FILE_READ);
  if (!f)
  {
    Serial.println("[DD] dinh_danh.json not found, using defaults");
    return;
  }
  String c = "";
  while (f.available())
    c += (char)f.read();
  f.close();
  String v;
  int n;
  if ((v = _jStr(c, "ma_id")).length())
    dinhDanh.ma_id = v;
  if ((v = _jStr(c, "ho_ten")).length())
    dinhDanh.ho_ten = v;
  if ((n = _jInt(c, "nam_sinh")) >= 0)
    dinhDanh.nam_sinh = n;
  if ((v = _jStr(c, "dia_chi")).length())
    dinhDanh.dia_chi = v;
  if ((v = _jStr(c, "ngay_cap_dinh_danh")).length())
    dinhDanh.ngay_cap_dinh_danh = v;
  if ((v = _jStr(c, "ten_thiet_bi")).length())
    dinhDanh.ten_thiet_bi = v;
  if ((v = _jStr(c, "id_thiet_bi")).length())
    dinhDanh.id_thiet_bi = v;
  if ((v = _jStr(c, "dia_chi_thiet_bi")).length())
    dinhDanh.dia_chi_thiet_bi = v;
  Serial.println("[DD] Loaded dinh_danh.json");
}

bool saveDinhDanhToSD()
{
  File f = SD.open("/dinh_danh.json", FILE_WRITE);
  if (!f)
    return false;
  f.printf("{\n  \"ma_id\":\"%s\",\n  \"ho_ten\":\"%s\",\n  \"nam_sinh\":%d,\n"
           "  \"dia_chi\":\"%s\",\n  \"ngay_cap_dinh_danh\":\"%s\",\n"
           "  \"ten_thiet_bi\":\"%s\",\n  \"id_thiet_bi\":\"%s\",\n"
           "  \"dia_chi_thiet_bi\":\"%s\"\n}",
           dinhDanh.ma_id.c_str(), dinhDanh.ho_ten.c_str(), dinhDanh.nam_sinh,
           dinhDanh.dia_chi.c_str(), dinhDanh.ngay_cap_dinh_danh.c_str(),
           dinhDanh.ten_thiet_bi.c_str(), dinhDanh.id_thiet_bi.c_str(),
           dinhDanh.dia_chi_thiet_bi.c_str());
  f.close();
  return true;
}

// ================== SSID LIST BINARY LOAD & SEEK ==================
// Đọc (ssid, pass) tại index bất kỳ từ Flash LittleFS hoặc /ssid_list.bin trên SD. O(1) seek, 0 bytes RAM.
bool g_ssidSourceFlash = false;

bool getSsidEntry(uint32_t idx, String &outSsid, String &outPass)
{
  if (idx == 0)
  {
    outSsid = String(AP_SSID);
    outPass = String(AP_PASS);
    return true;
  }

  File f;
  if (g_ssidSourceFlash)
  {
    f = LittleFS.open("/ssid_list.bin", "r");
  }
  else
  {
    f = SD.open("/ssid_list.bin", FILE_READ);
  }

  if (!f)
  {
    outSsid = String(AP_SSID);
    outPass = String(AP_PASS);
    return false;
  }
  if (!f.seek((uint32_t)idx * SSID_RECORD_SIZE))
  {
    f.close();
    outSsid = String(AP_SSID);
    outPass = String(AP_PASS);
    return false;
  }
  char buf[SSID_RECORD_SIZE + 1];
  memset(buf, 0, sizeof(buf));
  int n = f.read((uint8_t *)buf, SSID_RECORD_SIZE);
  f.close();
  if (n != SSID_RECORD_SIZE)
  {
    outSsid = String(AP_SSID);
    outPass = String(AP_PASS);
    return false;
  }
  char sBuf[33];
  char pBuf[33];
  memcpy(sBuf, buf, 32);
  sBuf[32] = '\0';
  memcpy(pBuf, buf + 32, 32);
  pBuf[32] = '\0';
  outSsid = String(sBuf);
  outPass = String(pBuf);
  return true;
}

void loadSsidList()
{
  g_ssidCount = 0;
  g_ssidSourceFlash = false;

  // 1. Ưu tiên đọc từ bộ nhớ Flash nội bộ (LittleFS) - an toàn, không thể tháo rời
  if (LittleFS.exists("/ssid_list.bin"))
  {
    File f = LittleFS.open("/ssid_list.bin", "r");
    if (f)
    {
      size_t sz = f.size();
      f.close();
      g_ssidCount = sz / SSID_RECORD_SIZE;
      if (g_ssidCount > SSID_LIST_MAX)
        g_ssidCount = SSID_LIST_MAX;
      if (g_ssidCount > 0)
      {
        g_ssidSourceFlash = true;
        String firstSsid, firstPass, lastSsid, lastPass;
        getSsidEntry(0, firstSsid, firstPass);
        getSsidEntry(g_ssidCount - 1, lastSsid, lastPass);
        Serial.printf("[SSID] Loaded %u (ssid,pass) from internal Flash LittleFS (first=%s, last=%s)\n",
                      g_ssidCount, firstSsid.c_str(), lastSsid.c_str());
        return;
      }
    }
  }

  // 2. Dự phòng: nếu Flash chưa nạp, đọc từ thẻ nhớ MicroSD
  File f = SD.open("/ssid_list.bin", FILE_READ);
  if (f)
  {
    size_t sz = f.size();
    f.close();
    g_ssidCount = sz / SSID_RECORD_SIZE;
    if (g_ssidCount > SSID_LIST_MAX)
      g_ssidCount = SSID_LIST_MAX;
    if (g_ssidCount > 0)
    {
      g_ssidSourceFlash = false;
      String firstSsid, firstPass, lastSsid, lastPass;
      getSsidEntry(0, firstSsid, firstPass);
      getSsidEntry(g_ssidCount - 1, lastSsid, lastPass);
      Serial.printf("[SSID] Loaded %u (ssid,pass) from MicroSD card (first=%s, last=%s)\n",
                    g_ssidCount, firstSsid.c_str(), lastSsid.c_str());
      return;
    }
  }

  // 3. Fallback: chỉ 1 SSID khởi điểm AP_SSID + pass AP_PASS.
  g_ssidCount = 1;
  Serial.printf("[SSID] Fallback single AP: %s\n", AP_SSID);
}

String safeFileName(String name)
{
  name.replace("\\", "/");

  int slash = name.lastIndexOf('/');
  if (slash >= 0)
    name = name.substring(slash + 1);

  String safe = "";
  for (size_t i = 0; i < name.length(); i++)
  {
    char c = name[i];
    if ((c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        c == '.' || c == '_' || c == '-')
      safe += c;
    else
      safe += "_";
  }

  if (safe.length() == 0)
    safe = "upload.bin";

  if (safe.length() > 80)
    safe = safe.substring(safe.length() - 80);

  return safe;
}

bool initSDWithFallback()
{
  const uint32_t freqs[] = {20000000, 16000000, 10000000, 8000000, 4000000, 1000000, 400000};

  for (size_t i = 0; i < (sizeof(freqs) / sizeof(freqs[0])); i++)
  {
    uint32_t hz = freqs[i];
    SD.end();
    Serial.printf("[SD] Thu SD.begin o %lu Hz...\n", (unsigned long)hz);
    if (SD.begin(SD_CS, spiSD, hz, "/sd", 8, SD_FORMAT_IF_EMPTY))
    {
      mountedSDFreq = hz;
      Serial.printf("[SD] OK: Mount SD thanh cong o %lu Hz\n", (unsigned long)hz);
      return true;
    }
  }
  return false;
}

bool ensureUploadDir()
{
  if (!SD.exists("/uploads"))
    return SD.mkdir("/uploads");
  return true;
}

uint64_t getUsedBytes() { return SD.usedBytes(); }
uint64_t getTotalBytes() { return SD.totalBytes(); }

// ================== LED FUNCTIONS ==================
// Theo hiện tượng bạn báo: để im mà sáng cả 3 đèn => mạch LED đang là HIGH = sáng, LOW = tắt.
// Nếu mạch của bạn là loại ngược lại thì chỉ cần đổi LED_ON/LED_OFF.
#define LED_ON HIGH
#define LED_OFF LOW

void ledAllOff()
{
  digitalWrite(LED_RED, LED_OFF);
  digitalWrite(LED_GREEN, LED_OFF);
  digitalWrite(LED_WHITE, LED_OFF);
}

void ledRed(bool on) { digitalWrite(LED_RED, on ? LED_ON : LED_OFF); }
void ledGreen(bool on) { digitalWrite(LED_GREEN, on ? LED_ON : LED_OFF); }

void setLedState(LedState st)
{
  g_ledState = st;

  // Luôn tắt hết trước để đảm bảo mỗi thời điểm chỉ đúng 1 đèn sáng.
  ledAllOff();

  switch (st)
  {
  case LED_STATE_IDLE:
    // IDLE = không có kết nối => tắt hết
    break;

  case LED_STATE_CONNECTED:
    // CONNECTED = xanh
    ledGreen(true);
    break;

  case LED_STATE_TRANSFER:
    // TRANSFER = đỏ liên tục
    ledRed(true);
    break;

  case LED_STATE_ERROR:
  default:
    // ERROR = đỏ, nháy xử lý bằng vòng lặp delay bên ngoài
    ledRed(true);
    break;
  }
}

void setTransferLed(bool active)
{
  transferLedActive = active;
  if (active)
  {
    setLedState(LED_STATE_TRANSFER);
  }
}

bool isUsbSerialConnected()
{
#if ARDUINO_USB_CDC_ON_BOOT
  return (bool)Serial; // true chỉ khi USB host đang thật sự kết nối
#else
  return false;
#endif
}

uint8_t getActiveConnectionCount()
{
  uint8_t count = 0;

  // WiFi AP: số client đang nối vào ESP32.
  if (currentMode == MODE_AP)
  {
    count += WiFi.softAPgetStationNum();
  }

  // WiFi STA: ESP32 đang nối vào trusted WiFi cũng tính là 1 kết nối.
  if (currentMode == MODE_STA && WiFi.status() == WL_CONNECTED)
  {
    count++;
  }

  return count;
}

void updateConnectionLed()
{
  // Ưu tiên trạng thái: ERROR > TRANSFER > CONNECTED > IDLE
  if (g_ledState == LED_STATE_ERROR)
    return;

  if (transferLedActive)
    return;

  uint8_t totalConnections = getActiveConnectionCount();

  // USB CDC cũng được tính là một kết nối nếu host đang mở cổng Serial.
  if (isUsbSerialConnected())
  {
    totalConnections++;
  }

  lastClientCount = (currentMode == MODE_AP) ? WiFi.softAPgetStationNum() : 0;

  if (totalConnections == 0)
    setLedState(LED_STATE_IDLE); // Tắt hết: chưa có kết nối
  else
    setLedState(LED_STATE_CONNECTED); // Xanh: có ít nhất 1 kết nối
}

void endTransferLed()
{
  transferLedActive = false;
  updateConnectionLed();
}

void startTransferLed()
{
  setTransferLed(true);
}

void setWifiLedByClient()
{
  updateConnectionLed();
}

void updateStatusLed()
{
  updateConnectionLed();
}

// ================== API HANDLERS ==================
void handleRoot()
{
  String ip = (currentMode == MODE_STA) ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  String msg = "ESP32-S3 microSD WiFi Server\n";
  msg += "Mode: ";
  msg += (currentMode == MODE_STA) ? "STA" : "AP";
  msg += "\nIP: " + ip + "\nPort: " + String(SERVER_PORT) + "\n\n";
  msg += "API:\n";
  msg += "GET    /api/status\n";
  msg += "GET    /api/filelist\n";
  msg += "POST   /api/upload     form-data key=file\n";
  msg += "POST   /api/upload-all form-data key=files\n";
  msg += "GET    /api/download?name=filename.bin\n";
  msg += "DELETE /api/delete?name=filename.bin\n";
  msg += "DELETE /api/delete-all\n";
  server.send(200, "text/plain", msg);
}

// Pass tương ứng với SSID đang phát (g_currentApSsid), fallback AP_PASS.
String passForCurrentAp()
{
  if (g_currentApPass.length() > 0)
    return g_currentApPass;
  return String(AP_PASS);
}

void handleStatus()
{
  uint8_t cardType = SD.cardType();
  String card = "UNKNOWN";
  if (cardType == CARD_NONE)
    card = "NONE";
  else if (cardType == CARD_MMC)
    card = "MMC";
  else if (cardType == CARD_SD)
    card = "SDSC";
  else if (cardType == CARD_SDHC)
    card = "SDHC/SDXC";

  String ip = (currentMode == MODE_STA) ? WiFi.localIP().toString() : WiFi.softAPIP().toString();

  String json = "{";
  json += "\"ok\":true,";
  json += "\"board\":\"ESP32-S3\",";
  json += "\"wifi_mode\":\"" + String(currentMode == MODE_STA ? "STA" : "AP") + "\",";
  json += "\"ip\":\"" + ip + "\",";
  json += "\"port\":" + String(SERVER_PORT) + ",";
  json += "\"ap_ssid\":\"" + jsonEscape(g_currentApSsid.length() ? g_currentApSsid : String(AP_SSID)) + "\",";
  json += "\"ap_pass\":\"" + jsonEscape(passForCurrentAp()) + "\",";
  json += "\"sd_card_type\":\"" + card + "\",";
  json += "\"sd_spi_hz\":" + String((unsigned long)mountedSDFreq) + ",";
  json += "\"sd_total\":" + String((unsigned long long)getTotalBytes()) + ",";
  json += "\"sd_used\":" + String((unsigned long long)getUsedBytes()) + ",";
  json += "\"sd_total_human\":\"" + humanSize(getTotalBytes()) + "\",";
  json += "\"sd_used_human\":\"" + humanSize(getUsedBytes()) + "\",";
  json += "\"dinh_danh\":{";
  json += "\"ma_id\":\"" + jsonEscape(String(dinhDanh.ma_id)) + "\",";
  json += "\"ho_ten\":\"" + jsonEscape(String(dinhDanh.ho_ten)) + "\",";
  json += "\"nam_sinh\":" + String(dinhDanh.nam_sinh) + ",";
  json += "\"dia_chi\":\"" + jsonEscape(String(dinhDanh.dia_chi)) + "\",";
  json += "\"ngay_cap_dinh_danh\":\"" + jsonEscape(String(dinhDanh.ngay_cap_dinh_danh)) + "\",";
  json += "\"ten_thiet_bi\":\"" + jsonEscape(String(dinhDanh.ten_thiet_bi)) + "\",";
  json += "\"id_thiet_bi\":\"" + jsonEscape(String(dinhDanh.id_thiet_bi)) + "\",";
  json += "\"dia_chi_thiet_bi\":\"" + jsonEscape(String(dinhDanh.dia_chi_thiet_bi)) + "\"";
  json += "}}";

  server.send(200, "application/json", json);
}

void handleGetDinhDanh()
{
  String json = "{";
  json += "\"ok\":true,";
  json += "\"ma_id\":\"" + jsonEscape(dinhDanh.ma_id) + "\",";
  json += "\"ho_ten\":\"" + jsonEscape(dinhDanh.ho_ten) + "\",";
  json += "\"nam_sinh\":" + String(dinhDanh.nam_sinh) + ",";
  json += "\"dia_chi\":\"" + jsonEscape(dinhDanh.dia_chi) + "\",";
  json += "\"ngay_cap_dinh_danh\":\"" + jsonEscape(dinhDanh.ngay_cap_dinh_danh) + "\",";
  json += "\"ten_thiet_bi\":\"" + jsonEscape(dinhDanh.ten_thiet_bi) + "\",";
  json += "\"id_thiet_bi\":\"" + jsonEscape(dinhDanh.id_thiet_bi) + "\",";
  json += "\"dia_chi_thiet_bi\":\"" + jsonEscape(dinhDanh.dia_chi_thiet_bi) + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handlePostDinhDanh()
{
  String body = server.arg("plain");
  if (body.length() == 0)
  {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"empty body\"}");
    return;
  }
  String v;
  int n;
  if ((v = _jStr(body, "ma_id")).length())
    dinhDanh.ma_id = v;
  if ((v = _jStr(body, "ho_ten")).length())
    dinhDanh.ho_ten = v;
  if ((n = _jInt(body, "nam_sinh")) >= 0)
    dinhDanh.nam_sinh = n;
  if ((v = _jStr(body, "dia_chi")).length())
    dinhDanh.dia_chi = v;
  if ((v = _jStr(body, "ngay_cap_dinh_danh")).length())
    dinhDanh.ngay_cap_dinh_danh = v;
  if ((v = _jStr(body, "ten_thiet_bi")).length())
    dinhDanh.ten_thiet_bi = v;
  if ((v = _jStr(body, "id_thiet_bi")).length())
    dinhDanh.id_thiet_bi = v;
  if ((v = _jStr(body, "dia_chi_thiet_bi")).length())
    dinhDanh.dia_chi_thiet_bi = v;

  if (!saveDinhDanhToSD())
  {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"sd_write_failed\"}");
    return;
  }
  handleGetDinhDanh();
}

void handleFileList()
{
  File dir = SD.open("/uploads");
  if (!dir || !dir.isDirectory())
  {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"cannot_open_uploads_dir\"}");
    return;
  }

  String json = "{\"ok\":true,\"dir\":\"/uploads\",\"files\":[";
  bool first = true;

  while (true)
  {
    File file = dir.openNextFile();
    if (!file)
      break;

    if (!file.isDirectory())
    {
      String fullName = String(file.name());
      int slash = fullName.lastIndexOf('/');
      String name = (slash >= 0) ? fullName.substring(slash + 1) : fullName;
      uint64_t size = file.size();

      if (!first)
        json += ",";
      first = false;

      json += "{";
      json += "\"name\":\"" + jsonEscape(name) + "\",";
      json += "\"size\":" + String((unsigned long long)size) + ",";
      json += "\"size_human\":\"" + humanSize(size) + "\",";
      json += "\"download\":\"/api/download?name=" + jsonEscape(name) + "\"";
      json += "}";
    }
    file.close();
  }
  dir.close();

  json += "]}";
  server.send(200, "application/json", json);
}

String getNameArg()
{
  if (!server.hasArg("name"))
    return "";
  return safeFileName(server.arg("name"));
}

void handleDownload()
{
  String name = getNameArg();
  if (name.length() == 0)
  {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing_name\"}");
    return;
  }

  String path = "/uploads/" + name;
  if (!SD.exists(path))
  {
    server.send(404, "application/json", "{\"ok\":false,\"error\":\"file_not_found\"}");
    return;
  }

  File file = SD.open(path, FILE_READ);
  if (!file)
  {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"cannot_open_file\"}");
    return;
  }

  uint64_t fileSize = file.size();
  setTransferLed(true);
  uint32_t dlStart = millis();
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
  server.streamFile(file, "application/octet-stream");
  uint32_t dlMs = millis() - dlStart;
  file.close();
  endTransferLed();

  float dlSec = dlMs / 1000.0f;
  float dlKBs = (dlSec > 0) ? (fileSize / 1024.0f / dlSec) : 0;
  Serial.printf("[DOWNLOAD] %s | %llu bytes | %.2f s | %.1f KB/s\n",
                name.c_str(), (unsigned long long)fileSize, dlSec, dlKBs);
  logTransfer("HTTP_DOWN", name, fileSize, dlSec, dlKBs, "ok");

  SD.remove(path);
  Serial.printf("[DOWNLOAD] Xoa file sau download: %s\n", name.c_str());
}

void handleDelete()
{
  String name = getNameArg();
  if (name.length() == 0)
  {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing_name\"}");
    return;
  }

  String path = "/uploads/" + name;
  if (!SD.exists(path))
  {
    server.send(404, "application/json", "{\"ok\":false,\"error\":\"file_not_found\"}");
    return;
  }

  if (SD.remove(path))
    server.send(200, "application/json", "{\"ok\":true,\"deleted\":\"" + jsonEscape(name) + "\"}");
  else
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"delete_failed\"}");
}

void handleDeleteAll()
{
  File dir = SD.open("/uploads");
  if (!dir || !dir.isDirectory())
  {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"cannot_open_uploads_dir\"}");
    return;
  }

  uint32_t deleted = 0, failed = 0;

  while (true)
  {
    File file = dir.openNextFile();
    if (!file)
      break;

    if (!file.isDirectory())
    {
      String fullName = String(file.name());
      int slash = fullName.lastIndexOf('/');
      String name = (slash >= 0) ? fullName.substring(slash + 1) : fullName;
      file.close();

      if (SD.remove("/uploads/" + name))
        deleted++;
      else
        failed++;
    }
    else
    {
      file.close();
    }
  }
  dir.close();

  String json = "{\"ok\":";
  json += (failed == 0 ? "true" : "false");
  json += ",\"deleted\":" + String(deleted) + ",\"failed\":" + String(failed) + "}";
  server.send(failed == 0 ? 200 : 500, "application/json", json);
}

void handleUploadResponse()
{
  if (uploadOK)
  {
    String json = "{\"ok\":true,";
    json += "\"name\":\"" + jsonEscape(uploadFileName) + "\",";
    json += "\"size\":" + String((unsigned long long)uploadBytes) + ",";
    json += "\"size_human\":\"" + humanSize(uploadBytes) + "\"}";
    server.send(200, "application/json", json);
  }
  else
  {
    server.send(500, "application/json",
                "{\"ok\":false,\"error\":\"" + jsonEscape(uploadError) + "\"}");
  }
}

void handleFileUpload()
{
  HTTPUpload &upload = server.upload();

  if (upload.status == UPLOAD_FILE_START)
  {
    uploadOK = false;
    uploadError = "";
    uploadBytes = 0;
    uploadStartMs = millis();
    uploadFileName = safeFileName(upload.filename);
    uploadFilePath = "/uploads/" + uploadFileName;

    Serial.printf("[UPLOAD] START %s\n", uploadFileName.c_str());
    setTransferLed(true);

    if (!ensureUploadDir())
    {
      uploadError = "cannot_create_uploads_dir";
      endTransferLed();
      return;
    }
    if (SD.exists(uploadFilePath))
      SD.remove(uploadFilePath);

    uploadFile = SD.open(uploadFilePath, FILE_WRITE);
    if (!uploadFile)
    {
      uploadError = "cannot_open_file_for_write";
      endTransferLed();
      return;
    }
  }
  else if (upload.status == UPLOAD_FILE_WRITE)
  {
    if (uploadFile)
    {
      size_t written = uploadFile.write(upload.buf, upload.currentSize);
      uploadBytes += written;
      if (written != upload.currentSize)
        uploadError = "sd_write_failed";
    }
    else
    {
      uploadError = "upload_file_not_open";
    }
  }
  else if (upload.status == UPLOAD_FILE_END)
  {
    if (uploadFile)
    {
      uploadFile.flush();
      uploadFile.close();
    }

    if (uploadError.length() == 0 && uploadBytes > 0)
    {
      uploadOK = true;
      uint32_t ulMs = millis() - uploadStartMs;
      float ulSec = ulMs / 1000.0f;
      float ulKBs = (ulSec > 0) ? (uploadBytes / 1024.0f / ulSec) : 0;
      startTransferLed();
      Serial.printf("[UPLOAD] OK %s | %llu bytes | %.2f s | %.1f KB/s\n",
                    uploadFileName.c_str(), (unsigned long long)uploadBytes, ulSec, ulKBs);
      logTransfer("HTTP_UP", uploadFileName, uploadBytes, ulSec, ulKBs, "ok");
      endTransferLed();
    }
    else
    {
      if (uploadError.length() == 0)
        uploadError = "empty_upload";
      if (SD.exists(uploadFilePath))
        SD.remove(uploadFilePath);
      Serial.printf("[UPLOAD] FAIL %s err=%s\n", uploadFileName.c_str(), uploadError.c_str());
      endTransferLed();
    }
  }
  else if (upload.status == UPLOAD_FILE_ABORTED)
  {
    if (uploadFile)
      uploadFile.close();
    if (SD.exists(uploadFilePath))
      SD.remove(uploadFilePath);
    uploadOK = false;
    uploadError = "upload_aborted";
    endTransferLed();
  }
}

// ================== UPLOAD ALL HANDLERS ==================
void resetUploadAllState()
{
  uploadAllActive = true;
  uploadAllOK = 0;
  uploadAllFailed = 0;
  uploadAllBytes = 0;
  uploadAllErrors = "";
}

void handleUploadAllResponse()
{
  if (!uploadAllActive)
  {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"no_files_uploaded\"}");
    return;
  }

  String json = "{\"ok\":";
  json += (uploadAllFailed == 0 ? "true" : "false");
  json += ",\"uploaded\":" + String(uploadAllOK);
  json += ",\"failed\":" + String(uploadAllFailed);
  json += ",\"total_bytes\":" + String((unsigned long long)uploadAllBytes);
  json += ",\"total_human\":\"" + humanSize(uploadAllBytes) + "\"";
  if (uploadAllErrors.length() > 0)
    json += ",\"errors\":\"" + jsonEscape(uploadAllErrors) + "\"";
  json += "}";

  uploadAllActive = false;
  server.send(uploadAllFailed == 0 ? 200 : 500, "application/json", json);
}

void handleFileUploadAll()
{
  HTTPUpload &upload = server.upload();

  if (upload.status == UPLOAD_FILE_START)
  {
    if (!uploadAllActive)
      resetUploadAllState();

    uploadOK = false;
    uploadError = "";
    uploadBytes = 0;
    uploadStartMs = millis();
    uploadFileName = safeFileName(upload.filename);
    uploadFilePath = "/uploads/" + uploadFileName;

    Serial.printf("[UPLOAD-ALL] START %s\n", uploadFileName.c_str());
    setTransferLed(true);

    if (!ensureUploadDir())
    {
      uploadError = "cannot_create_uploads_dir";
      uploadAllFailed++;
      uploadAllErrors += uploadFileName + ": cannot_create_uploads_dir; ";
      endTransferLed();
      return;
    }

    if (SD.exists(uploadFilePath))
      SD.remove(uploadFilePath);

    uploadFile = SD.open(uploadFilePath, FILE_WRITE);
    if (!uploadFile)
    {
      uploadError = "cannot_open_file_for_write";
      uploadAllFailed++;
      uploadAllErrors += uploadFileName + ": cannot_open_file_for_write; ";
      endTransferLed();
      return;
    }
  }
  else if (upload.status == UPLOAD_FILE_WRITE)
  {
    if (uploadFile)
    {
      size_t written = uploadFile.write(upload.buf, upload.currentSize);
      uploadBytes += written;
      if (written != upload.currentSize)
        uploadError = "sd_write_failed";
    }
    else
    {
      uploadError = "upload_file_not_open";
    }
  }
  else if (upload.status == UPLOAD_FILE_END)
  {
    if (uploadFile)
    {
      uploadFile.flush();
      uploadFile.close();
    }

    if (uploadError.length() == 0 && uploadBytes > 0)
    {
      uploadOK = true;
      uploadAllOK++;
      uploadAllBytes += uploadBytes;
      uint32_t ulMs = millis() - uploadStartMs;
      float ulSec = ulMs / 1000.0f;
      float ulKBs = (ulSec > 0) ? (uploadBytes / 1024.0f / ulSec) : 0;
      startTransferLed();
      Serial.printf("[UPLOAD-ALL] OK %s | %llu bytes | %.2f s | %.1f KB/s\n",
                    uploadFileName.c_str(), (unsigned long long)uploadBytes, ulSec, ulKBs);
      logTransfer("HTTP_UP_ALL", uploadFileName, uploadBytes, ulSec, ulKBs, "ok");
      endTransferLed();
    }
    else
    {
      if (uploadError.length() == 0)
        uploadError = "empty_upload";
      uploadAllFailed++;
      uploadAllErrors += uploadFileName + ": " + uploadError + "; ";
      if (SD.exists(uploadFilePath))
        SD.remove(uploadFilePath);
      Serial.printf("[UPLOAD-ALL] FAIL %s err=%s\n", uploadFileName.c_str(), uploadError.c_str());
      endTransferLed();
    }
  }
  else if (upload.status == UPLOAD_FILE_ABORTED)
  {
    if (uploadFile)
      uploadFile.close();
    if (SD.exists(uploadFilePath))
      SD.remove(uploadFilePath);
    uploadOK = false;
    uploadError = "upload_aborted";
    uploadAllFailed++;
    uploadAllErrors += uploadFileName + ": upload_aborted; ";
    endTransferLed();
  }
}

// ================== WIFI SWITCHING ==================
void handleDeviceInfo()
{
  String json = "{\"ok\":true,";
  json += "\"id_thiet_bi\":\"" + jsonEscape(dinhDanh.id_thiet_bi) + "\",";
  json += "\"mac\":\"" + WiFi.macAddress() + "\"}";
  server.send(200, "application/json", json);
}

void notifyTrustedHost()
{
  String ip = WiFi.localIP().toString();
  String gw = WiFi.gatewayIP().toString();
  String payload = "{\"ip\":\"" + ip + "\",\"type\":\"PhantomR3b\"}";
  String url = "http://" + gw + ":" + String(SERVER_PORT) + "/api/device/connection";

  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(payload);
  Serial.printf("[NOTIFY] POST %s -> %d\n", url.c_str(), code);
  http.end();
}

void startAP(const String &ssid, const String &pass)
{
  server.stop();
  // Tắt AP/STA an toàn trước khi đổi mode. Đặt mode OFF rồi mới gọi softAPdisconnect/disconnect
  // để tránh lỗi "STA disconnect failed! 0x3001: ESP_ERR_WIFI_NOT_INIT".
  WiFi.mode(WIFI_OFF);
  delay(200);
  ledAllOff();

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  WiFi.softAPConfig(local_IP, gateway, subnet);
  WiFi.softAP(ssid.c_str(), pass.c_str(), AP_CHANNEL, AP_HIDDEN, AP_MAX_CONN);

  g_currentApSsid = ssid;
  g_currentApPass = pass;
  currentMode = MODE_AP;
  lastClientCount = 0;
  Serial.printf("[WIFI] AP: %s  IP: %s\n", ssid.c_str(), WiFi.softAPIP().toString().c_str());
  server.begin();
}

void startAP()
{
  // Phát SSID khởi điểm (phần tử đầu list) với pass riêng của nó.
  String defSsid, defPass;
  getSsidEntry(0, defSsid, defPass);
  startAP(defSsid, defPass);
}

void revertToHandshakeAP()
{
  Serial.println("[HANDSHAKE] timeout -> revert to default AP");
  g_lockedIndex = -1;
  g_handshakeDone = false;
  startAP();
}

void handleHandshake()
{
  String body = server.arg("plain");
  if (body.length() == 0)
  {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"empty_body\"}");
    return;
  }
  long idxVal = _jInt(body, "index");
  if (idxVal < 0 || (uint32_t)idxVal >= g_ssidCount)
  {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_index\"}");
    return;
  }
  uint32_t idx = (uint32_t)idxVal;

  // Nếu đã có client đang transfer trên SSID động hiện tại → reject để tránh gián đoạn.
  if (g_lockedIndex >= 0 && g_handshakeDone)
  {
    String r = "{\"ok\":false,\"error\":\"busy\",\"locked_index\":" + String(g_lockedIndex) + "}";
    server.send(409, "application/json", r);
    return;
  }

  String newSsid, newPass;
  if (!getSsidEntry(idx, newSsid, newPass))
  {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"read_ssid_fail\"}");
    return;
  }

  Serial.printf("[HANDSHAKE] index=%u -> switch AP to %s (g_ssidCount=%u)\n", idx, newSsid.c_str(), g_ssidCount);
  g_lockedIndex = (int32_t)idx;
  g_lockStartMs = millis();
  g_handshakeDone = false;

  // Gửi response TRƯỚC khi đổi SSID, vì sau khi softAP đổi SSID, client cũ (đang kết nối 0002)
  // sẽ bị mất kết nối và không nhận được response → gây lỗi "Host is unreachable" ở R3c/R3a.
  String r = "{\"ok\":true,\"index\":" + String(idx) + ",\"ssid\":\"" + jsonEscape(newSsid) + "\"}";
  server.send(200, "application/json", r);
  delay(100); // Đợi client nhận xong response trước khi AP đổi SSID.

  startAP(newSsid, newPass);
}

bool connectToTrusted()
{
  server.stop();
  // Core 3.x: softAPdisconnect(true) -> AP.end() deinit ca WiFi stack, nen goi
  // tiep disconnect() khi stack da chet se tra ESP_ERR_WIFI_NOT_INIT (0x3001).
  if (WiFi.getMode() != WIFI_MODE_NULL)
  {
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true);
    delay(300);
  }

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("PhantomR3b");
  WiFi.setSleep(false);
  WiFi.begin(TRUSTED_SSID, TRUSTED_PASS);

  Serial.printf("[WIFI] Dang ket noi %s...\n", TRUSTED_SSID);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED)
  {
    if (millis() - t > STA_CONNECT_TIMEOUT_MS)
    {
      Serial.println("[WIFI] Ket noi that bai, quay ve AP.");
      startAP();
      return false;
    }
    delay(200);
  }

  currentMode = MODE_STA;
  Serial.printf("[WIFI] STA connected. IP: %s\n", WiFi.localIP().toString().c_str());
  notifyTrustedHost();
  server.begin();
  return true;
}

void doWifiScan()
{
  // TRUSTED_SSID chi phat khi may PC phat hien USB serial cua thiet bi nay.
  // Chua cam USB -> AP do khong ton tai, thu ket noi chi lam mat AP + timeout vo ich.
  if (!isUsbSerialConnected())
    return;

  // TRUSTED_SSID la AP an -> khong the xac nhan bang scan (WiFi.SSID(i) rong).
  // Thu ket noi truc tiep: WiFi.begin() gui probe request co ten nen tim duoc AP an.
  // connectToTrusted() tu quay ve startAP() khi timeout.
  Serial.println("[SCAN] USB da cam -> thu ket noi trusted AP an...");
  connectToTrusted();
}

void checkSTAConnection()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("[WIFI] Mat ket noi STA, quay ve AP.");
    startAP();
  }
}

void handleNotFound()
{
  String json = "{\"ok\":false,\"error\":\"not_found\",\"uri\":\"" + jsonEscape(server.uri()) + "\"}";
  server.send(404, "application/json", json);
}

// ================== SERIAL (USB CDC) COMMAND HANDLER ==================
static String _serialBuf = "";

static String base64Encode(const uint8_t *data, size_t len)
{
  const char *b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String out = "";
  out.reserve(((len + 2) / 3) * 4 + 4);
  for (size_t i = 0; i < len; i += 3)
  {
    uint32_t v = (uint32_t)data[i] << 16;
    if (i + 1 < len)
      v |= (uint32_t)data[i + 1] << 8;
    if (i + 2 < len)
      v |= data[i + 2];
    out += b64[(v >> 18) & 0x3F];
    out += b64[(v >> 12) & 0x3F];
    out += (i + 1 < len) ? b64[(v >> 6) & 0x3F] : '=';
    out += (i + 2 < len) ? b64[v & 0x3F] : '=';
  }
  return out;
}

static int _b64Val(char c)
{
  if (c >= 'A' && c <= 'Z')
    return c - 'A';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 26;
  if (c >= '0' && c <= '9')
    return c - '0' + 52;
  if (c == '+')
    return 62;
  if (c == '/')
    return 63;
  return -1;
}

static bool base64DecodeToBuffer(const String &b64, uint8_t *out, size_t outMax, size_t *outLen)
{
  *outLen = 0;
  size_t len = b64.length();

  if (len == 0 || (len % 4) != 0)
    return false;

  for (size_t i = 0; i + 3 < len; i += 4)
  {
    int v0 = _b64Val(b64[i]);
    int v1 = _b64Val(b64[i + 1]);
    int v2 = (b64[i + 2] != '=') ? _b64Val(b64[i + 2]) : -1;
    int v3 = (b64[i + 3] != '=') ? _b64Val(b64[i + 3]) : -1;

    if (v0 < 0 || v1 < 0)
      return false;
    if (b64[i + 2] != '=' && v2 < 0)
      return false;
    if (b64[i + 3] != '=' && v3 < 0)
      return false;

    if (*outLen + 1 > outMax)
      return false;
    out[(*outLen)++] = (uint8_t)((v0 << 2) | (v1 >> 4));

    if (b64[i + 2] != '=')
    {
      if (*outLen + 1 > outMax)
        return false;
      out[(*outLen)++] = (uint8_t)((v1 << 4) | (v2 >> 2));
    }

    if (b64[i + 3] != '=')
    {
      if (*outLen + 1 > outMax)
        return false;
      out[(*outLen)++] = (uint8_t)((v2 << 6) | v3);
    }
  }

  return true;
}

static String _jCmdStr(const String &j, const String &key) { return _jStr(j, key); }

void processSerialCommand(const String &line)
{
  String cmd = _jCmdStr(line, "cmd");

  // ── ping / identify ──────────────────────────────────────────────
  if (cmd == "ping")
  {
    Serial.println("{\"ok\":true,\"type\":\"PHANTOMR3B\",\"board\":\"ESP32-S3\",\"version\":\"1.0\"}");
    return;
  }

  // ── status ───────────────────────────────────────────────────────
  if (cmd == "status")
  {
    uint8_t ct = SD.cardType();
    String card = ct == CARD_MMC ? "MMC" : ct == CARD_SD ? "SDSC"
                                       : ct == CARD_SDHC ? "SDHC/SDXC"
                                                         : "UNKNOWN";
    String ip = (currentMode == MODE_STA) ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
    String r = "{\"ok\":true,\"board\":\"ESP32-S3\",\"type\":\"PHANTOMR3B\",";
    r += "\"wifi_mode\":\"" + String(currentMode == MODE_STA ? "STA" : "AP") + "\",";
    r += "\"ip\":\"" + ip + "\",";
    r += "\"sd_card_type\":\"" + card + "\",";
    r += "\"sd_total\":" + String((unsigned long long)getTotalBytes()) + ",";
    r += "\"sd_used\":" + String((unsigned long long)getUsedBytes()) + ",";
    r += "\"sd_total_human\":\"" + humanSize(getTotalBytes()) + "\",";
    r += "\"sd_used_human\":\"" + humanSize(getUsedBytes()) + "\"}";
    Serial.println(r);
    return;
  }

  // ── dinh_danh GET ────────────────────────────────────────────────
  if (cmd == "dinh_danh")
  {
    String r = "{\"ok\":true,";
    r += "\"ma_id\":\"" + jsonEscape(dinhDanh.ma_id) + "\",";
    r += "\"ho_ten\":\"" + jsonEscape(dinhDanh.ho_ten) + "\",";
    r += "\"nam_sinh\":" + String(dinhDanh.nam_sinh) + ",";
    r += "\"dia_chi\":\"" + jsonEscape(dinhDanh.dia_chi) + "\",";
    r += "\"ngay_cap_dinh_danh\":\"" + jsonEscape(dinhDanh.ngay_cap_dinh_danh) + "\",";
    r += "\"ten_thiet_bi\":\"" + jsonEscape(dinhDanh.ten_thiet_bi) + "\",";
    r += "\"id_thiet_bi\":\"" + jsonEscape(dinhDanh.id_thiet_bi) + "\",";
    r += "\"dia_chi_thiet_bi\":\"" + jsonEscape(dinhDanh.dia_chi_thiet_bi) + "\"}";
    Serial.println(r);
    return;
  }

  // ── dinh_danh SET ────────────────────────────────────────────────
  if (cmd == "set_dinh_danh")
  {
    String v;
    int n;
    if ((v = _jStr(line, "ma_id")).length())
      dinhDanh.ma_id = v;
    if ((v = _jStr(line, "ho_ten")).length())
      dinhDanh.ho_ten = v;
    if ((n = _jInt(line, "nam_sinh")) >= 0)
      dinhDanh.nam_sinh = n;
    if ((v = _jStr(line, "dia_chi")).length())
      dinhDanh.dia_chi = v;
    if ((v = _jStr(line, "ngay_cap_dinh_danh")).length())
      dinhDanh.ngay_cap_dinh_danh = v;
    if ((v = _jStr(line, "ten_thiet_bi")).length())
      dinhDanh.ten_thiet_bi = v;
    if ((v = _jStr(line, "id_thiet_bi")).length())
      dinhDanh.id_thiet_bi = v;
    if ((v = _jStr(line, "dia_chi_thiet_bi")).length())
      dinhDanh.dia_chi_thiet_bi = v;
    if (saveDinhDanhToSD())
      Serial.println("{\"ok\":true}");
    else
      Serial.println("{\"ok\":false,\"error\":\"sd_write_failed\"}");
    return;
  }

  // ── filelist ─────────────────────────────────────────────────────
  if (cmd == "filelist")
  {
    File dir = SD.open("/uploads");
    if (!dir || !dir.isDirectory())
    {
      Serial.println("{\"ok\":false,\"error\":\"cannot_open_uploads_dir\"}");
      return;
    }
    String r = "{\"ok\":true,\"files\":[";
    bool first = true;
    while (true)
    {
      File f = dir.openNextFile();
      if (!f)
        break;
      if (!f.isDirectory())
      {
        String fn = String(f.name());
        int sl = fn.lastIndexOf('/');
        if (sl >= 0)
          fn = fn.substring(sl + 1);
        uint64_t sz = f.size();
        if (!first)
          r += ",";
        first = false;
        r += "{\"name\":\"" + jsonEscape(fn) + "\",\"size\":" + String((unsigned long long)sz) + ",\"size_human\":\"" + humanSize(sz) + "\"}";
      }
      f.close();
    }
    dir.close();
    r += "]}";
    Serial.println(r);
    return;
  }

  // ── download ─────────────────────────────────────────────────────
  // Legacy: chỉ dùng cho file nhỏ (< SERIAL_DECODE_BUF_SIZE bytes).
  // File lớn phải dùng download_begin / download_chunk.
  if (cmd == "download")
  {
    String name = safeFileName(_jStr(line, "name"));
    if (!name.length())
    {
      Serial.println("{\"ok\":false,\"error\":\"missing_name\"}");
      return;
    }
    String path = "/uploads/" + name;
    if (!SD.exists(path))
    {
      Serial.println("{\"ok\":false,\"error\":\"file_not_found\"}");
      return;
    }
    File f = SD.open(path, FILE_READ);
    if (!f)
    {
      Serial.println("{\"ok\":false,\"error\":\"cannot_open_file\"}");
      return;
    }
    size_t sz = f.size();
    if (sz > SERIAL_DECODE_BUF_SIZE)
    {
      f.close();
      // Redirect client to use chunked protocol
      Serial.println("{\"ok\":false,\"error\":\"file_too_large_use_download_chunk\",\"size\":" + String((unsigned long)sz) + "}");
      return;
    }
    setTransferLed(true);
    f.read(_serialDecodeBuf, sz);
    f.close();
    String b64 = base64Encode(_serialDecodeBuf, sz);
    String r = "{\"ok\":true,\"name\":\"" + jsonEscape(name) + "\",\"size\":" + String((unsigned long)sz) + ",\"data_b64\":\"" + b64 + "\"}";
    Serial.println(r);
    SD.remove(path);
    endTransferLed();
    return;
  }

  // ── download_begin ────────────────────────────────────────────────
  if (cmd == "download_begin")
  {
    String name = safeFileName(_jStr(line, "name"));
    if (!name.length())
    {
      Serial.println("{\"ok\":false,\"error\":\"missing_name\"}");
      return;
    }
    String path = "/uploads/" + name;
    if (!SD.exists(path))
    {
      Serial.println("{\"ok\":false,\"error\":\"file_not_found\"}");
      return;
    }
    if (_serialDownFile)
      _serialDownFile.close();
    _serialDownFile = SD.open(path, FILE_READ);
    if (!_serialDownFile)
    {
      Serial.println("{\"ok\":false,\"error\":\"cannot_open_file\"}");
      return;
    }
    _serialDownName = name;
    _serialDownSize = _serialDownFile.size();
    _serialDownRead = 0;
    _serialDownChunks = (_serialDownSize + SERIAL_DOWN_CHUNK - 1) / SERIAL_DOWN_CHUNK;
    _serialDownStartMs = millis();
    setTransferLed(true);
    Serial.println("{\"ok\":true,\"name\":\"" + jsonEscape(name) + "\",\"size\":" + String((unsigned long)_serialDownSize) + ",\"chunks\":" + String(_serialDownChunks) + "}");
    return;
  }

  // ── download_chunk ────────────────────────────────────────────────
  if (cmd == "download_chunk")
  {
    if (!_serialDownFile)
    {
      Serial.println("{\"ok\":false,\"error\":\"no_download_in_progress\"}");
      return;
    }
    size_t toRead = min((size_t)SERIAL_DOWN_CHUNK, _serialDownSize - _serialDownRead);
    size_t got = _serialDownFile.read(_serialDecodeBuf, toRead);
    _serialDownRead += got;
    bool done = (_serialDownRead >= _serialDownSize);
    String b64 = base64Encode(_serialDecodeBuf, got);
    String r = "{\"ok\":true,\"data_b64\":\"" + b64 + "\",\"done\":" + String(done ? "true" : "false") + "}";
    Serial.println(r);
    if (done)
    {
      _serialDownFile.close();
      SD.remove("/uploads/" + _serialDownName);
      uint32_t dlMs = millis() - _serialDownStartMs;
      float dlSec = dlMs / 1000.0f;
      float dlKBs = (dlSec > 0) ? (_serialDownSize / 1024.0f / dlSec) : 0;
      Serial.printf("[SERIAL-DOWN] OK %s | %lu bytes | %.2f s | %.1f KB/s\n",
                    _serialDownName.c_str(), (unsigned long)_serialDownSize, dlSec, dlKBs);
      logTransfer("SERIAL_DOWN", _serialDownName, _serialDownSize, dlSec, dlKBs, "ok");
      _serialDownName = "";
      _serialDownSize = 0;
      _serialDownRead = 0;
      _serialDownStartMs = 0;
      endTransferLed();
    }
    return;
  }

  // ── delete ───────────────────────────────────────────────────────
  if (cmd == "delete")
  {
    String name = safeFileName(_jStr(line, "name"));
    if (!name.length())
    {
      Serial.println("{\"ok\":false,\"error\":\"missing_name\"}");
      return;
    }
    String path = "/uploads/" + name;
    if (!SD.exists(path))
    {
      Serial.println("{\"ok\":false,\"error\":\"file_not_found\"}");
      return;
    }
    Serial.println(SD.remove(path) ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"delete_failed\"}");
    return;
  }

  // ── upload_bin ───────────────────────────────────────────────────
  // Chỉ dùng cho file rất nhỏ. File lớn phải dùng upload_begin/upload_chunk/upload_end.
  if (cmd == "upload_bin")
  {
    String name = safeFileName(_jStr(line, "name"));
    if (!name.length())
    {
      Serial.println("{\"ok\":false,\"error\":\"missing_name\"}");
      return;
    }

    String b64 = _jStr(line, "data_b64");
    if (!b64.length())
    {
      Serial.println("{\"ok\":false,\"error\":\"missing_data_b64\"}");
      return;
    }

    if (b64.length() > SERIAL_B64_MAX_LEN)
    {
      Serial.println("{\"ok\":false,\"error\":\"file_too_large_use_upload_chunk\"}");
      return;
    }

    size_t sz = 0;
    if (!base64DecodeToBuffer(b64, _serialDecodeBuf, SERIAL_DECODE_BUF_SIZE, &sz))
    {
      Serial.println("{\"ok\":false,\"error\":\"base64_decode_failed_or_chunk_too_large\"}");
      return;
    }

    setTransferLed(true);

    if (!ensureUploadDir())
    {
      endTransferLed();
      Serial.println("{\"ok\":false,\"error\":\"cannot_create_uploads_dir\"}");
      return;
    }

    String path = "/uploads/" + name;
    if (SD.exists(path))
      SD.remove(path);

    File f = SD.open(path, FILE_WRITE);
    if (!f)
    {
      endTransferLed();
      Serial.println("{\"ok\":false,\"error\":\"cannot_open_file\"}");
      return;
    }

    size_t written = f.write(_serialDecodeBuf, sz);
    f.flush();
    f.close();

    if (written != sz)
    {
      if (SD.exists(path))
        SD.remove(path);
      Serial.println("{\"ok\":false,\"error\":\"sd_write_failed\"}");
      endTransferLed();
      return;
    }

    String r = "{\"ok\":true,\"name\":\"" + jsonEscape(name) + "\",\"size\":" + String((unsigned long long)sz) + "}";
    Serial.println(r);
    endTransferLed();
    return;
  }

  // ── upload_begin ─────────────────────────────────────────────────────────
  if (cmd == "upload_begin")
  {
    String name = safeFileName(_jStr(line, "name"));
    if (!name.length())
    {
      Serial.println("{\"ok\":false,\"error\":\"missing_name\"}");
      return;
    }
    if (!ensureUploadDir())
    {
      Serial.println("{\"ok\":false,\"error\":\"cannot_create_uploads_dir\"}");
      return;
    }

    String path = "/uploads/" + name;
    if (SD.exists(path))
      SD.remove(path);
    if (_serialUpFile)
      _serialUpFile.close();

    _serialUpFile = SD.open(path, FILE_WRITE);
    if (!_serialUpFile)
    {
      Serial.println("{\"ok\":false,\"error\":\"cannot_open_file\"}");
      return;
    }

    _serialUpName = name;
    _serialUpWritten = 0;
    _serialUpTotal = (size_t)_jInt(line, "total");
    _serialUpStartMs = millis();
    setTransferLed(true);

    Serial.println("{\"ok\":true,\"mode\":\"direct_to_sd\",\"max_b64_len\":" + String(SERIAL_B64_MAX_LEN) + "}");
    return;
  }

  // ── upload_chunk ─────────────────────────────────────────────────────────
  // Nhận 1 chunk base64 -> decode vào buffer cố định -> ghi ngay xuống SD.
  if (cmd == "upload_chunk")
  {
    if (!_serialUpFile)
    {
      Serial.println("{\"ok\":false,\"error\":\"no_upload_in_progress\"}");
      return;
    }

    String b64 = _jStr(line, "data_b64");
    if (!b64.length())
    {
      Serial.println("{\"ok\":false,\"error\":\"missing_data_b64\"}");
      return;
    }

    if (b64.length() > SERIAL_B64_MAX_LEN)
    {
      Serial.println("{\"ok\":false,\"error\":\"chunk_too_large\",\"max_b64_len\":" + String(SERIAL_B64_MAX_LEN) + "}");
      return;
    }

    size_t sz = 0;
    if (!base64DecodeToBuffer(b64, _serialDecodeBuf, SERIAL_DECODE_BUF_SIZE, &sz))
    {
      Serial.println("{\"ok\":false,\"error\":\"base64_decode_failed_or_chunk_too_large\"}");
      return;
    }

    size_t written = _serialUpFile.write(_serialDecodeBuf, sz);
    if (written != sz)
    {
      _serialUpFile.flush();
      _serialUpFile.close();
      SD.remove("/uploads/" + _serialUpName);
      _serialUpName = "";
      _serialUpWritten = 0;
      Serial.println("{\"ok\":false,\"error\":\"sd_write_failed\"}");
      endTransferLed();
      return;
    }

    _serialUpWritten += written;

    // Flush định kỳ để dữ liệu thật sự xuống thẻ, tránh mất file nếu rút nguồn giữa chừng.
    if ((_serialUpWritten % (64 * 1024)) < written)
    {
      _serialUpFile.flush();
    }

    startTransferLed();
    Serial.println("{\"ok\":true,\"written\":" + String((unsigned long)_serialUpWritten) + "}");
    return;
  }

  // ── upload_end ───────────────────────────────────────────────────────────
  if (cmd == "upload_end")
  {
    if (!_serialUpFile)
    {
      Serial.println("{\"ok\":false,\"error\":\"no_upload_in_progress\"}");
      return;
    }

    _serialUpFile.flush();
    _serialUpFile.close();

    if (_serialUpTotal > 0 && _serialUpWritten != _serialUpTotal)
    {
      SD.remove("/uploads/" + _serialUpName);
      String r = "{\"ok\":false,\"error\":\"size_mismatch\",\"expected\":" + String((unsigned long)_serialUpTotal) + ",\"written\":" + String((unsigned long)_serialUpWritten) + "}";
      _serialUpName = "";
      _serialUpWritten = 0;
      _serialUpTotal = 0;
      Serial.println(r);
      endTransferLed();
      return;
    }

    String r = "{\"ok\":true,\"name\":\"" + jsonEscape(_serialUpName) + "\",\"size\":" + String((unsigned long)_serialUpWritten) + ",\"saved_to\":\"/uploads/" + jsonEscape(_serialUpName) + "\"}";
    uint32_t ulMs = millis() - _serialUpStartMs;
    float ulSec = ulMs / 1000.0f;
    float ulKBs = (ulSec > 0) ? (_serialUpWritten / 1024.0f / ulSec) : 0;
    Serial.printf("[SERIAL-UP] OK %s | %lu bytes | %.2f s | %.1f KB/s\n",
                  _serialUpName.c_str(), (unsigned long)_serialUpWritten, ulSec, ulKBs);
    logTransfer("SERIAL_UP", _serialUpName, _serialUpWritten, ulSec, ulKBs, "ok");
    _serialUpName = "";
    _serialUpWritten = 0;
    _serialUpTotal = 0;
    _serialUpStartMs = 0;
    Serial.println(r);
    endTransferLed();
    return;
  }

  // ── delete_all ───────────────────────────────────────────────────
  if (cmd == "delete_all")
  {
    File dir = SD.open("/uploads");
    if (!dir || !dir.isDirectory())
    {
      Serial.println("{\"ok\":false,\"error\":\"cannot_open_uploads_dir\"}");
      return;
    }
    uint32_t ok_count = 0, fail_count = 0;
    while (true)
    {
      File f = dir.openNextFile();
      if (!f)
        break;
      if (!f.isDirectory())
      {
        String fn = String(f.name());
        int sl = fn.lastIndexOf('/');
        if (sl >= 0)
          fn = fn.substring(sl + 1);
        f.close();
        SD.remove("/uploads/" + fn) ? ok_count++ : fail_count++;
      }
      else
        f.close();
    }
    dir.close();
    Serial.println("{\"ok\":" + String(fail_count == 0 ? "true" : "false") + ",\"deleted\":" + String(ok_count) + ",\"failed\":" + String(fail_count) + "}");
    return;
  }

  // ── read_log ─────────────────────────────────────────────────────
  if (cmd == "read_log")
  {
    File f = SD.open("/transfer_log.csv", FILE_READ);
    if (!f)
    {
      Serial.println("{\"ok\":false,\"error\":\"log_not_found\"}");
      return;
    }
    String csv = "";
    while (f.available())
    {
      char c = (char)f.read();
      if (c == '"')       csv += "\\\"";
      else if (c == '\\') csv += "\\\\";
      else if (c == '\r') continue;
      else if (c == '\n') csv += "\\n";
      else                csv += c;
    }
    f.close();
    Serial.println("{\"ok\":true,\"csv\":\"" + csv + "\"}");
    return;
  }

  // ── clear_log ─────────────────────────────────────────────────────
  if (cmd == "clear_log")
  {
    if (SD.exists("/transfer_log.csv"))
      SD.remove("/transfer_log.csv");
    Serial.println("{\"ok\":true}");
    return;
  }

  Serial.println("{\"ok\":false,\"error\":\"unknown_cmd\"}");
}

void handleSerial()
{
  while (Serial.available())
  {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r')
    {
      _serialBuf.trim();
      if (_serialBuf.startsWith("{"))
        processSerialCommand(_serialBuf);
      _serialBuf = "";
    }
    else
    {
      if (_serialBuf.length() > (SERIAL_B64_MAX_LEN + 512))
      {
        _serialBuf = "";
        Serial.println("{\"ok\":false,\"error\":\"serial_line_too_large_use_smaller_chunk\"}");
      }
      else
      {
        _serialBuf += c;
      }
    }
  }
}

// ================== SETUP ==================
void setup()
{
  // Tắt LED ngay khi boot, trước cả delay, tránh 3 màu do GPIO boot state.
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_WHITE, OUTPUT);
  ledAllOff();

  Serial.setRxBufferSize(32768);
  Serial.setTxBufferSize(32768);
  Serial.begin(115200);

  Serial.println();
  Serial.println("=== ESP32-S3 microSD WIFI SERVER ===");

  setLedState(LED_STATE_IDLE);

  Serial.println("[SD] Khoi tao SPI...");
  spiSD.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  Serial.println("[SD] Dang khoi tao the nho...");
  if (!initSDWithFallback())
  {
    Serial.println("[SD] FAIL: Khong nhan duoc the nho!");
    while (true)
    {
      setLedState(LED_STATE_ERROR);
      delay(300);
      ledAllOff();
      delay(300);
    }
  }

  if (!ensureUploadDir())
  {
    Serial.println("[SD] FAIL: Khong tao duoc /uploads");
    while (true)
    {
      setLedState(LED_STATE_ERROR);
      delay(100);
      ledAllOff();
      delay(100);
    }
  }

  loadDinhDanhFromSD();

  Serial.println("[FS] Khoi tao LittleFS Flash...");
  if (!LittleFS.begin(true))
  {
    Serial.println("[FS] WARN: Khong the mount LittleFS!");
  }
  else
  {
    Serial.println("[FS] LittleFS mounted OK");
  }

  loadSsidList();

  uint8_t cardType = SD.cardType();
  Serial.print("[SD] Loai the: ");
  if (cardType == CARD_MMC)
    Serial.println("MMC");
  else if (cardType == CARD_SD)
    Serial.println("SDSC");
  else if (cardType == CARD_SDHC)
    Serial.println("SDHC/SDXC");
  else
    Serial.println("UNKNOWN");

  Serial.printf("[SD] Total: %s  Used: %s\n",
                humanSize(SD.totalBytes()).c_str(), humanSize(SD.usedBytes()).c_str());

  // TRUSTED_SSID an -> khong xac nhan bang scan, ket noi truc tiep roi cho timeout.
  // Nhung AP nay chi bat khi PC thay USB serial: chua cam USB thi bo qua, len AP luon.
  if (isUsbSerialConnected())
  {
    Serial.println("[WIFI] USB da cam -> thu ket noi trusted AP an...");
    WiFi.mode(WIFI_STA);
    WiFi.setHostname("PhantomR3b");

    Serial.println("[WIFI] Ket noi STA...");
    WiFi.setSleep(false);
    WiFi.begin(TRUSTED_SSID, TRUSTED_PASS);
    unsigned long t = millis();
    bool connected = false;
    while (millis() - t < STA_CONNECT_TIMEOUT_MS)
    {
      if (WiFi.status() == WL_CONNECTED)
      {
        connected = true;
        break;
      }
      delay(200);
    }
    if (connected)
    {
      currentMode = MODE_STA;
      updateConnectionLed();
      Serial.printf("[WIFI] STA connected. IP: %s\n", WiFi.localIP().toString().c_str());
      notifyTrustedHost();
    }
    else
    {
      Serial.println("[WIFI] Ket noi STA that bai, chuyen sang AP.");
    }
  }

  if (currentMode != MODE_STA)
  {
    Serial.println("[WIFI] Cau hinh IP AP...");
    g_lockedIndex = -1;
    g_handshakeDone = false;
    startAP();
    if (g_currentApSsid.length() == 0)
    {
      // startAP failed
      while (true)
      {
        setLedState(LED_STATE_ERROR);
        delay(500);
        ledAllOff();
        delay(500);
      }
    }
    updateConnectionLed();
  }

  setWifiLedByClient();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/filelist", HTTP_GET, handleFileList);
  server.on("/api/download", HTTP_GET, handleDownload);

  server.on("/api/delete", HTTP_GET, handleDelete);
  server.on("/api/delete", HTTP_POST, handleDelete);
  server.on("/api/delete", HTTP_DELETE, handleDelete);

  server.on("/api/delete-all", HTTP_POST, handleDeleteAll);
  server.on("/api/delete-all", HTTP_DELETE, handleDeleteAll);

  server.on("/api/upload", HTTP_POST, handleUploadResponse, handleFileUpload);
  server.on("/api/upload-all", HTTP_POST, handleUploadAllResponse, handleFileUploadAll);

  server.on("/api/dinh-danh", HTTP_GET, handleGetDinhDanh);
  server.on("/api/dinh-danh", HTTP_POST, handlePostDinhDanh);

  server.on("/api/device-info", HTTP_GET, handleDeviceInfo);

  server.on("/api/handshake", HTTP_POST, handleHandshake);

  server.onNotFound(handleNotFound);

  server.begin();
  lastScanMs = millis();

  Serial.println("[HTTP] Server started");
  Serial.println("=== READY ===");
}

// ================== LOOP ==================
void loop()
{
  server.handleClient();
  updateStatusLed();
  handleSerial();

  unsigned long now = millis();

  // Handshake lock timeout: nếu đã đổi SSID cho client nhưng 30s không có STA connect → revert.
  if (currentMode == MODE_AP && g_lockedIndex >= 0 && !g_handshakeDone)
  {
    if (WiFi.softAPgetStationNum() > 0)
    {
      g_handshakeDone = true;
      Serial.printf("[HANDSHAKE] client connected to locked SSID index=%ld\n", (long)g_lockedIndex);
    }
    else if (now - g_lockStartMs > HANDSHAKE_LOCK_TIMEOUT_MS)
    {
      revertToHandshakeAP();
    }
  }

  if (currentMode == MODE_STA)
  {
    checkSTAConnection();
  }
  else if (now - lastScanMs >= SCAN_INTERVAL_MS)
  {
    lastScanMs = now;
    doWifiScan();
  }
}
