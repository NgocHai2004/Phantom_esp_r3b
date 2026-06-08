#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>

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
const char *AP_SSID = "7068616e746f6d303030303030300002";
const char *AP_PASS = "12345678";

IPAddress local_IP(10, 42, 0, 1);
IPAddress gateway(10, 42, 0, 1);
IPAddress subnet(255, 255, 255, 0);

#define SERVER_PORT 8765
WebServer server(SERVER_PORT);

#define AP_CHANNEL 1
#define AP_HIDDEN true
#define AP_MAX_CONN 2

// ================== SERIAL COM CONFIG ==================
#define SERIAL_BAUD 115200
#define SERIAL_CMD_TIMEOUT_MS 5000UL
#define SERIAL_UPLOAD_CHUNK 512

// ================== LED STATE ==================
bool uploadLedActive = false;
unsigned long uploadLedUntilMs = 0;
const uint16_t UPLOAD_LED_HOLD_MS = 500;

uint8_t lastClientCount = 0;

// ================== UPLOAD STATE ==================
File uploadFile;
String uploadFileName = "";
String uploadFilePath = "";
bool uploadOK = false;
String uploadError = "";
uint64_t uploadBytes = 0;

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

// ================== DINH DANH ==================
struct DinhDanh {
  String ma_id              = "BN-0001";
  String ho_ten             = "Nguyen Van A";
  int    nam_sinh           = 1990;
  String dia_chi            = "Ha Noi";
  String ngay_cap_dinh_danh = "2026-05-30";
  String ten_thiet_bi       = "Pi Zero 2W";
  String id_thiet_bi        = "DEVICE-0001";
  String dia_chi_thiet_bi   = "Ha Noi";
};

DinhDanh dinhDanh;

static String _jStr(const String &j, const String &key) {
  int p = j.indexOf("\"" + key + "\"");
  if (p < 0) return "";
  p = j.indexOf(':', p + key.length() + 2);
  if (p < 0) return "";
  p++;
  while (p < (int)j.length() && (j[p] == ' ' || j[p] == '\t')) p++;
  if (j[p] != '"') return "";
  p++;
  String v = "";
  while (p < (int)j.length() && j[p] != '"') {
    if (j[p] == '\\' && p + 1 < (int)j.length()) { p++; v += j[p]; }
    else v += j[p];
    p++;
  }
  return v;
}

static int _jInt(const String &j, const String &key) {
  int p = j.indexOf("\"" + key + "\"");
  if (p < 0) return -1;
  p = j.indexOf(':', p + key.length() + 2);
  if (p < 0) return -1;
  p++;
  while (p < (int)j.length() && (j[p] == ' ' || j[p] == '\t')) p++;
  String v = "";
  while (p < (int)j.length() && j[p] >= '0' && j[p] <= '9') v += j[p++];
  return v.length() ? v.toInt() : -1;
}

void loadDinhDanhFromSD() {
  File f = SD.open("/dinh_danh.json", FILE_READ);
  if (!f) { Serial.println("[DD] dinh_danh.json not found, using defaults"); return; }
  String c = "";
  while (f.available()) c += (char)f.read();
  f.close();
  String v;
  int    n;
  if ((v = _jStr(c, "ma_id")).length())              dinhDanh.ma_id              = v;
  if ((v = _jStr(c, "ho_ten")).length())             dinhDanh.ho_ten             = v;
  if ((n = _jInt(c, "nam_sinh")) >= 0)               dinhDanh.nam_sinh           = n;
  if ((v = _jStr(c, "dia_chi")).length())            dinhDanh.dia_chi            = v;
  if ((v = _jStr(c, "ngay_cap_dinh_danh")).length()) dinhDanh.ngay_cap_dinh_danh = v;
  if ((v = _jStr(c, "ten_thiet_bi")).length())       dinhDanh.ten_thiet_bi       = v;
  if ((v = _jStr(c, "id_thiet_bi")).length())        dinhDanh.id_thiet_bi        = v;
  if ((v = _jStr(c, "dia_chi_thiet_bi")).length())   dinhDanh.dia_chi_thiet_bi   = v;
  Serial.println("[DD] Loaded dinh_danh.json");
}

bool saveDinhDanhToSD() {
  File f = SD.open("/dinh_danh.json", FILE_WRITE);
  if (!f) return false;
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

uint64_t getUsedBytes()  { return SD.usedBytes(); }
uint64_t getTotalBytes() { return SD.totalBytes(); }

// ================== LED FUNCTIONS ==================
void ledAllOff()
{
  digitalWrite(LED_RED, LOW);
  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_WHITE, LOW);
}

void ledRed(bool on)   { digitalWrite(LED_RED,   on ? HIGH : LOW); }
void ledGreen(bool on) { digitalWrite(LED_GREEN, on ? HIGH : LOW); }
void ledWhite(bool on) { digitalWrite(LED_WHITE, on ? HIGH : LOW); }
void ledPink(bool on)  { digitalWrite(LED_RED, on ? HIGH : LOW); digitalWrite(LED_WHITE, on ? HIGH : LOW); }

void setWifiLedByClient()
{
  uint8_t clientCount = WiFi.softAPgetStationNum();
  ledGreen(clientCount > 0);
  lastClientCount = clientCount;
}

void startUploadBlinkWhite()
{
  unsigned long now = millis();
  uploadLedActive = true;
  if ((int32_t)(uploadLedUntilMs - now) <= 0)
    uploadLedUntilMs = now + UPLOAD_LED_HOLD_MS;
  else
    uploadLedUntilMs += UPLOAD_LED_HOLD_MS;
  ledWhite(true);
}

void updateStatusLed()
{
  unsigned long now = millis();

  if (uploadLedActive)
  {
    if ((int32_t)(uploadLedUntilMs - now) > 0)
    {
      ledWhite(true);
      return;
    }
    uploadLedActive = false;
    ledWhite(false);
    setWifiLedByClient();
    return;
  }

  uint8_t clientCount = WiFi.softAPgetStationNum();
  if (clientCount != lastClientCount)
    setWifiLedByClient();
}

// ================== API HANDLERS (WiFi AP) ==================
void handleRoot()
{
  String ip = WiFi.softAPIP().toString();
  String msg = "ESP32-S3 microSD WiFi Server\n";
  msg += "Mode: AP\n";
  msg += "IP: " + ip + "\nPort: " + String(SERVER_PORT) + "\n\n";
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

void handleStatus()
{
  uint8_t cardType = SD.cardType();
  String card = "UNKNOWN";
  if (cardType == CARD_NONE)       card = "NONE";
  else if (cardType == CARD_MMC)   card = "MMC";
  else if (cardType == CARD_SD)    card = "SDSC";
  else if (cardType == CARD_SDHC)  card = "SDHC/SDXC";

  String ip = WiFi.softAPIP().toString();

  String json = "{";
  json += "\"ok\":true,";
  json += "\"board\":\"ESP32-S3\",";
  json += "\"wifi_mode\":\"AP\",";
  json += "\"ip\":\"" + ip + "\",";
  json += "\"port\":" + String(SERVER_PORT) + ",";
  json += "\"ap_ssid\":\"" + jsonEscape(String(AP_SSID)) + "\",";
  json += "\"ap_pass\":\"" + jsonEscape(String(AP_PASS)) + "\",";
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
  if (body.length() == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"empty body\"}");
    return;
  }
  String v;
  int    n;
  if ((v = _jStr(body, "ma_id")).length())              dinhDanh.ma_id              = v;
  if ((v = _jStr(body, "ho_ten")).length())             dinhDanh.ho_ten             = v;
  if ((n = _jInt(body, "nam_sinh")) >= 0)               dinhDanh.nam_sinh           = n;
  if ((v = _jStr(body, "dia_chi")).length())            dinhDanh.dia_chi            = v;
  if ((v = _jStr(body, "ngay_cap_dinh_danh")).length()) dinhDanh.ngay_cap_dinh_danh = v;
  if ((v = _jStr(body, "ten_thiet_bi")).length())       dinhDanh.ten_thiet_bi       = v;
  if ((v = _jStr(body, "id_thiet_bi")).length())        dinhDanh.id_thiet_bi        = v;
  if ((v = _jStr(body, "dia_chi_thiet_bi")).length())   dinhDanh.dia_chi_thiet_bi   = v;

  if (!saveDinhDanhToSD()) {
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
    if (!file) break;

    if (!file.isDirectory())
    {
      String fullName = String(file.name());
      int slash = fullName.lastIndexOf('/');
      String name = (slash >= 0) ? fullName.substring(slash + 1) : fullName;
      uint64_t size = file.size();

      if (!first) json += ",";
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
  if (!server.hasArg("name")) return "";
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

  server.sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
  server.streamFile(file, "application/octet-stream");
  file.close();

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
    if (!file) break;

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
    uploadFileName = safeFileName(upload.filename);
    uploadFilePath = "/uploads/" + uploadFileName;

    Serial.printf("[UPLOAD] START %s\n", uploadFileName.c_str());

    if (!ensureUploadDir()) { uploadError = "cannot_create_uploads_dir"; return; }
    if (SD.exists(uploadFilePath)) SD.remove(uploadFilePath);

    uploadFile = SD.open(uploadFilePath, FILE_WRITE);
    if (!uploadFile) { uploadError = "cannot_open_file_for_write"; return; }
  }
  else if (upload.status == UPLOAD_FILE_WRITE)
  {
    if (uploadFile)
    {
      size_t written = uploadFile.write(upload.buf, upload.currentSize);
      uploadBytes += written;
      if (written != upload.currentSize) uploadError = "sd_write_failed";
    }
    else
    {
      uploadError = "upload_file_not_open";
    }
  }
  else if (upload.status == UPLOAD_FILE_END)
  {
    if (uploadFile) { uploadFile.flush(); uploadFile.close(); }

    if (uploadError.length() == 0 && uploadBytes > 0)
    {
      uploadOK = true;
      startUploadBlinkWhite();
      Serial.printf("[UPLOAD] OK %s %llu bytes\n", uploadFileName.c_str(), (unsigned long long)uploadBytes);
    }
    else
    {
      if (uploadError.length() == 0) uploadError = "empty_upload";
      if (SD.exists(uploadFilePath)) SD.remove(uploadFilePath);
      Serial.printf("[UPLOAD] FAIL %s err=%s\n", uploadFileName.c_str(), uploadError.c_str());
    }
  }
  else if (upload.status == UPLOAD_FILE_ABORTED)
  {
    if (uploadFile) uploadFile.close();
    if (SD.exists(uploadFilePath)) SD.remove(uploadFilePath);
    uploadOK = false;
    uploadError = "upload_aborted";
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
    if (!uploadAllActive) resetUploadAllState();

    uploadOK = false;
    uploadError = "";
    uploadBytes = 0;
    uploadFileName = safeFileName(upload.filename);
    uploadFilePath = "/uploads/" + uploadFileName;

    Serial.printf("[UPLOAD-ALL] START %s\n", uploadFileName.c_str());

    if (!ensureUploadDir())
    {
      uploadError = "cannot_create_uploads_dir";
      uploadAllFailed++;
      uploadAllErrors += uploadFileName + ": cannot_create_uploads_dir; ";
      return;
    }

    if (SD.exists(uploadFilePath)) SD.remove(uploadFilePath);

    uploadFile = SD.open(uploadFilePath, FILE_WRITE);
    if (!uploadFile)
    {
      uploadError = "cannot_open_file_for_write";
      uploadAllFailed++;
      uploadAllErrors += uploadFileName + ": cannot_open_file_for_write; ";
      return;
    }
  }
  else if (upload.status == UPLOAD_FILE_WRITE)
  {
    if (uploadFile)
    {
      size_t written = uploadFile.write(upload.buf, upload.currentSize);
      uploadBytes += written;
      if (written != upload.currentSize) uploadError = "sd_write_failed";
    }
    else
    {
      uploadError = "upload_file_not_open";
    }
  }
  else if (upload.status == UPLOAD_FILE_END)
  {
    if (uploadFile) { uploadFile.flush(); uploadFile.close(); }

    if (uploadError.length() == 0 && uploadBytes > 0)
    {
      uploadOK = true;
      uploadAllOK++;
      uploadAllBytes += uploadBytes;
      startUploadBlinkWhite();
      Serial.printf("[UPLOAD-ALL] OK %s %llu bytes\n", uploadFileName.c_str(), (unsigned long long)uploadBytes);
    }
    else
    {
      if (uploadError.length() == 0) uploadError = "empty_upload";
      uploadAllFailed++;
      uploadAllErrors += uploadFileName + ": " + uploadError + "; ";
      if (SD.exists(uploadFilePath)) SD.remove(uploadFilePath);
      Serial.printf("[UPLOAD-ALL] FAIL %s err=%s\n", uploadFileName.c_str(), uploadError.c_str());
    }
  }
  else if (upload.status == UPLOAD_FILE_ABORTED)
  {
    if (uploadFile) uploadFile.close();
    if (SD.exists(uploadFilePath)) SD.remove(uploadFilePath);
    uploadOK = false;
    uploadError = "upload_aborted";
    uploadAllFailed++;
    uploadAllErrors += uploadFileName + ": upload_aborted; ";
  }
}

void handleDeviceInfo()
{
  String json = "{\"ok\":true,";
  json += "\"id_thiet_bi\":\"" + jsonEscape(dinhDanh.id_thiet_bi) + "\",";
  json += "\"mac\":\"" + WiFi.macAddress() + "\"}";
  server.send(200, "application/json", json);
}

void handleNotFound()
{
  String json = "{\"ok\":false,\"error\":\"not_found\",\"uri\":\"" + jsonEscape(server.uri()) + "\"}";
  server.send(404, "application/json", json);
}

// ================== SERIAL COM PROTOCOL ==================
// Giao thuc text qua COM:
//   LIST\n              -> JSON danh sach file trong /uploads
//   STATUS\n            -> JSON thong tin SD + dinhDanh
//   DELETE:ten_file\n   -> OK:ten_file\n hoac ERR:ly_do\n
//   DELETE_ALL\n        -> OK:N\n hoac ERR:ly_do\n
//   DOWNLOAD:ten_file\n -> SIZE:N\n + N bytes raw + DONE\n
//   UPLOAD:ten_file:N\n -> (PC gui N bytes) -> OK:ten_file\n hoac ERR:ly_do\n
//   PING\n              -> PONG\n

String serialBuildFileList()
{
  File dir = SD.open("/uploads");
  if (!dir || !dir.isDirectory())
    return "{\"ok\":false,\"error\":\"cannot_open_uploads_dir\"}";

  String json = "{\"ok\":true,\"dir\":\"/uploads\",\"files\":[";
  bool first = true;

  while (true)
  {
    File file = dir.openNextFile();
    if (!file) break;
    if (!file.isDirectory())
    {
      String fullName = String(file.name());
      int slash = fullName.lastIndexOf('/');
      String name = (slash >= 0) ? fullName.substring(slash + 1) : fullName;
      uint64_t size = file.size();
      if (!first) json += ",";
      first = false;
      json += "{\"name\":\"" + jsonEscape(name) + "\",";
      json += "\"size\":" + String((unsigned long long)size) + ",";
      json += "\"size_human\":\"" + humanSize(size) + "\"}";
    }
    file.close();
  }
  dir.close();
  json += "]}";
  return json;
}

String serialBuildStatus()
{
  uint8_t cardType = SD.cardType();
  String card = "UNKNOWN";
  if (cardType == CARD_NONE)       card = "NONE";
  else if (cardType == CARD_MMC)   card = "MMC";
  else if (cardType == CARD_SD)    card = "SDSC";
  else if (cardType == CARD_SDHC)  card = "SDHC/SDXC";

  String json = "{\"ok\":true,\"board\":\"ESP32-S3\",";
  json += "\"mac\":\"" + WiFi.macAddress() + "\",";
  json += "\"sd_card_type\":\"" + card + "\",";
  json += "\"sd_spi_hz\":" + String((unsigned long)mountedSDFreq) + ",";
  json += "\"sd_total\":" + String((unsigned long long)getTotalBytes()) + ",";
  json += "\"sd_used\":" + String((unsigned long long)getUsedBytes()) + ",";
  json += "\"sd_total_human\":\"" + humanSize(getTotalBytes()) + "\",";
  json += "\"sd_used_human\":\"" + humanSize(getUsedBytes()) + "\",";
  json += "\"dinh_danh\":{";
  json += "\"ma_id\":\"" + jsonEscape(dinhDanh.ma_id) + "\",";
  json += "\"ho_ten\":\"" + jsonEscape(dinhDanh.ho_ten) + "\",";
  json += "\"nam_sinh\":" + String(dinhDanh.nam_sinh) + ",";
  json += "\"dia_chi\":\"" + jsonEscape(dinhDanh.dia_chi) + "\",";
  json += "\"ten_thiet_bi\":\"" + jsonEscape(dinhDanh.ten_thiet_bi) + "\",";
  json += "\"id_thiet_bi\":\"" + jsonEscape(dinhDanh.id_thiet_bi) + "\"";
  json += "}}";
  return json;
}

void handleSerialCommands()
{
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd.length() == 0) return;

  // PING
  if (cmd == "PING") {
    Serial.println("PONG");
    return;
  }

  // LIST
  if (cmd == "LIST") {
    Serial.println(serialBuildFileList());
    return;
  }

  // STATUS
  if (cmd == "STATUS") {
    Serial.println(serialBuildStatus());
    return;
  }

  // DELETE_ALL
  if (cmd == "DELETE_ALL") {
    File dir = SD.open("/uploads");
    if (!dir || !dir.isDirectory()) { Serial.println("ERR:cannot_open_uploads_dir"); return; }
    uint32_t del = 0;
    while (true) {
      File f = dir.openNextFile();
      if (!f) break;
      if (!f.isDirectory()) {
        String fn = String(f.name());
        int sl = fn.lastIndexOf('/');
        String nm = (sl >= 0) ? fn.substring(sl + 1) : fn;
        f.close();
        if (SD.remove("/uploads/" + nm)) del++;
      } else { f.close(); }
    }
    dir.close();
    Serial.println("OK:" + String(del));
    return;
  }

  // DELETE:filename
  if (cmd.startsWith("DELETE:")) {
    String name = safeFileName(cmd.substring(7));
    if (name.length() == 0) { Serial.println("ERR:missing_name"); return; }
    String path = "/uploads/" + name;
    if (!SD.exists(path)) { Serial.println("ERR:file_not_found"); return; }
    if (SD.remove(path)) Serial.println("OK:" + name);
    else                 Serial.println("ERR:delete_failed");
    return;
  }

  // DOWNLOAD:filename
  if (cmd.startsWith("DOWNLOAD:")) {
    String name = safeFileName(cmd.substring(9));
    if (name.length() == 0) { Serial.println("ERR:missing_name"); return; }
    String path = "/uploads/" + name;
    if (!SD.exists(path)) { Serial.println("ERR:file_not_found"); return; }
    File f = SD.open(path, FILE_READ);
    if (!f) { Serial.println("ERR:cannot_open_file"); return; }
    uint64_t sz = f.size();
    Serial.println("SIZE:" + String((unsigned long long)sz));
    uint8_t buf[SERIAL_UPLOAD_CHUNK];
    while (f.available()) {
      int n = f.read(buf, SERIAL_UPLOAD_CHUNK);
      if (n > 0) Serial.write(buf, n);
    }
    f.close();
    Serial.println("DONE");
    return;
  }

  // UPLOAD:filename:size
  if (cmd.startsWith("UPLOAD:")) {
    String rest = cmd.substring(7);
    int colon = rest.lastIndexOf(':');
    if (colon < 1) { Serial.println("ERR:bad_command"); return; }
    String name = safeFileName(rest.substring(0, colon));
    uint32_t size = rest.substring(colon + 1).toInt();
    if (name.length() == 0 || size == 0) { Serial.println("ERR:bad_command"); return; }

    if (!ensureUploadDir()) { Serial.println("ERR:cannot_create_uploads_dir"); return; }
    String path = "/uploads/" + name;
    if (SD.exists(path)) SD.remove(path);
    File f = SD.open(path, FILE_WRITE);
    if (!f) { Serial.println("ERR:cannot_open_file_for_write"); return; }

    Serial.println("READY");

    uint32_t received = 0;
    uint8_t buf[SERIAL_UPLOAD_CHUNK];
    unsigned long lastRecv = millis();

    while (received < size) {
      if (millis() - lastRecv > SERIAL_CMD_TIMEOUT_MS) {
        f.close();
        SD.remove(path);
        Serial.println("ERR:timeout");
        return;
      }
      int avail = Serial.available();
      if (avail <= 0) continue;
      uint32_t need = size - received;
      int toRead = (avail < (int)SERIAL_UPLOAD_CHUNK) ? avail : SERIAL_UPLOAD_CHUNK;
      if ((uint32_t)toRead > need) toRead = (int)need;
      int n = Serial.readBytes(buf, toRead);
      if (n > 0) {
        f.write(buf, n);
        received += n;
        lastRecv = millis();
      }
    }
    f.flush();
    f.close();
    startUploadBlinkWhite();
    Serial.println("OK:" + name);
    Serial.printf("[SERIAL] Upload OK: %s %lu bytes\n", name.c_str(), (unsigned long)received);
    return;
  }

  Serial.println("ERR:unknown_command");
}

// ================== SETUP ==================
void setup()
{
  Serial.begin(SERIAL_BAUD);
  delay(1500);

  Serial.println();
  Serial.println("=== ESP32-S3 microSD SERVER (AP+COM) ===");

  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_WHITE, OUTPUT);
  ledAllOff();
  ledRed(true);

  Serial.println("[SD] Khoi tao SPI...");
  spiSD.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  Serial.println("[SD] Dang khoi tao the nho...");
  if (!initSDWithFallback())
  {
    Serial.println("[SD] FAIL: Khong nhan duoc the nho!");
    while (true) { ledRed(true); delay(300); ledRed(false); delay(300); }
  }

  if (!ensureUploadDir())
  {
    Serial.println("[SD] FAIL: Khong tao duoc /uploads");
    while (true) { ledRed(true); delay(100); ledRed(false); delay(100); }
  }

  loadDinhDanhFromSD();

  uint8_t cardType = SD.cardType();
  Serial.print("[SD] Loai the: ");
  if (cardType == CARD_MMC)        Serial.println("MMC");
  else if (cardType == CARD_SD)    Serial.println("SDSC");
  else if (cardType == CARD_SDHC)  Serial.println("SDHC/SDXC");
  else                             Serial.println("UNKNOWN");

  Serial.printf("[SD] Total: %s  Used: %s\n",
    humanSize(SD.totalBytes()).c_str(), humanSize(SD.usedBytes()).c_str());

  Serial.println("[WIFI] Khoi dong AP...");
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  WiFi.softAPConfig(local_IP, gateway, subnet);
  if (!WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL, AP_HIDDEN, AP_MAX_CONN))
  {
    Serial.println("[WIFI] FAIL: Khong phat duoc WiFi AP");
    while (true) { ledRed(true); delay(500); ledRed(false); delay(500); }
  }
  ledRed(false);
  Serial.printf("[WIFI] AP SSID: %s  IP: %s  PORT: %d\n",
    AP_SSID, WiFi.softAPIP().toString().c_str(), SERVER_PORT);

  setWifiLedByClient();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/filelist", HTTP_GET, handleFileList);
  server.on("/api/download", HTTP_GET, handleDownload);

  server.on("/api/delete", HTTP_GET,    handleDelete);
  server.on("/api/delete", HTTP_POST,   handleDelete);
  server.on("/api/delete", HTTP_DELETE, handleDelete);

  server.on("/api/delete-all", HTTP_POST,   handleDeleteAll);
  server.on("/api/delete-all", HTTP_DELETE, handleDeleteAll);

  server.on("/api/upload",     HTTP_POST, handleUploadResponse, handleFileUpload);
  server.on("/api/upload-all", HTTP_POST, handleUploadAllResponse, handleFileUploadAll);

  server.on("/api/dinh-danh", HTTP_GET,  handleGetDinhDanh);
  server.on("/api/dinh-danh", HTTP_POST, handlePostDinhDanh);

  server.on("/api/device-info", HTTP_GET, handleDeviceInfo);

  server.onNotFound(handleNotFound);

  server.begin();

  Serial.println("[HTTP] WiFi AP server started");
  Serial.println("[COM]  Serial COM ready - giao thuc: LIST/STATUS/PING/DELETE/DOWNLOAD/UPLOAD");
  Serial.println("=== READY ===");
}

// ================== LOOP ==================
void loop()
{
  server.handleClient();
  updateStatusLed();
  handleSerialCommands();
}
