/*
 ESPrinkler2 -- Web Server Enabled Sprinkler Controller
 https://github.com/n0bel/ESPrinkler2
 Original Base code is https://github.com/esp8266/Arduino/blob/master/libraries/ESP8266WebServer/examples/FSBrowser/FSBrowser.ino
 Base Code Copyright (c) 2015 Hristo Gochkov. All rights reserved. LGPL
 This Code Copyright (c) 2017 Kevin Uhlir. All rights reserved. LGPL
 This is a rewrite of ESPrinkler https://github.com/n0bel/ESPrinkler but based
 on the Arduino toolchain instead of ExperssIF.  This should make the project
 more accessable to people.
*/

/* Requirements:
  Arduino-1.8.3
  ESP8266/Arduino :Additional Boards Manager URL: http://arduino.esp8266.com/stable/package_esp8266com_index.json
  Time 1.5.0 https://github.com/PaulStoffregen/Time
  SimpleTimer https://github.com/jfturcot/SimpleTimer (http://playground.arduino.cc/Code/SimpleTimer)
  NtpClientLib 2.0.5 https://github.com/gmag11/NtpClient
  ArduinoJson 5.6.7 https://github.com/bblanchon/ArduinoJson (https://bblanchon.github.io/ArduinoJson/)
  U8G2Lib 2.13.5 https://github.com/olikraus/u8g2 
  orbitalair-arduino-rtc-pcf8563  https://bitbucket.org/orbitalair/arduino_rtc_pcf8563/downloads/ (https://playground.arduino.cc/Main/RTC-PCF8563)

  Don't forget to restart the Arduino IDE after installing these things.


  Set your esp settings.. the board, program method, flash size and spiffs size.

  This uses the SPIFFS file system.  So we need to load that in your esp-xx first.
  Upload the contents of the data folder with MkSPIFFS Tool ("ESP8266 Sketch Data Upload" in Tools menu in Arduino IDE)

  Then compile and upload the .ino.

  */

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <TimeLib.h>
#include <NtpClientLib.h>
#include <FS.h>
#include <stdarg.h>

#include <ArduinoJson.h>
#include <string.h>
#include <U8g2lib.h>
#include <SimpleTimer.h>
#include <SPI.h>
#include <Wire.h>
#include <Rtc_Pcf8563.h>

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

Rtc_Pcf8563 rtc;

#define DBG_OUTPUT_PORT Serial
// #define EXTRA_DEBUG

const char* configFile = "/config.json";   // The config file name
const char* schedFile = "/sched.json";   // The sched file name

// Note that these are the default values if no /config.json exists,
//  or items are missing from it.

char ssid[31] = { "" };               // This is the access point to connect to
char password[31] = { "" };           // And its password
char host[31] = { "ESPrinkler2" };    // The host name .local (mdns) hostname
char assid[31] = { "" };              // This is the access point to connect to
char apassword[31] = { "" };          // And its password

int offsetGMT = 0;                    // Local timezone offset in seconds
char offsetGMTstring[10] = { "0" };   // String version of it
int relayState = 0;                   // The current state of the relayState

bool apMode = false;                  // Are we in Acess Point mode?
bool hasRtc = false;                  // do we have an rtc
bool rtcValid  = false;               // does it have a valid time

char timeServer[31] = { "0.pool.ntp.org" };   // the NTP timeServer to use

#define MAX_SCHED 30
struct _sched {
  int zone;       // 1-8 and 100 for reset
  int days;       // day bits 0-6 = sunday-saturday and 8 and 9 are even and odd
  time_t start;   // seconds like 02:00 am is 120 * 3600
  int duration;   // minutes
  time_t begin;   // begin date
  time_t end;     // end date
  time_t next;    // next time this should turn on a relay
  time_t stop;    // when the relay should turn off
} sched[MAX_SCHED] = { 0 };


ESP8266WebServer server(80);  // The Web Server
File fsUploadFile;            // holds the current upload when files are
                              // uploaded (see edit.htm)
WiFiUDP udp;
SimpleTimer timer;

time_t ms = 0;            // tracking milliseconds
int flagRestart = false;

// format bytes
String formatBytes(size_t bytes) {
  if (bytes < 1024) {
    return String(bytes)+"B";
  } else if (bytes < (1024 * 1024)) {
    return String(bytes/1024.0)+"KB";
  } else if (bytes < (1024 * 1024 * 1024)) {
    return String(bytes/1024.0/1024.0)+"MB";
  } else {
    return String(bytes/1024.0/1024.0/1024.0)+"GB";
  }
}

String getContentType(String filename) {
  if (server.hasArg("download")) return "application/octet-stream";
  else if (filename.endsWith(".htm")) return "text/html";
  else if (filename.endsWith(".html")) return "text/html";
  else if (filename.endsWith(".css")) return "text/css";
  else if (filename.endsWith(".js")) return "application/javascript";
  else if (filename.endsWith(".json")) return "text/json";
  else if (filename.endsWith(".png")) return "image/png";
  else if (filename.endsWith(".gif")) return "image/gif";
  else if (filename.endsWith(".jpg")) return "image/jpeg";
  else if (filename.endsWith(".ico")) return "image/x-icon";
  else if (filename.endsWith(".xml")) return "text/xml";
  else if (filename.endsWith(".pdf")) return "application/x-pdf";
  else if (filename.endsWith(".zip")) return "application/x-zip";
  else if (filename.endsWith(".gz")) return "application/x-gzip";
  return "text/plain";
}

bool handleFileRead(String path) {
  DBG_OUTPUT_PORT.printf(" handleFileRead: %s %d\n",
    path.c_str(), ESP.getFreeHeap());
  if (path.endsWith("/")) path += "index.html";

  String contentType = getContentType(path);
  String pathWithGz = path + ".gz";
  if (SPIFFS.exists(pathWithGz) || SPIFFS.exists(path)) {
    if (SPIFFS.exists(pathWithGz))
      path += ".gz";
    File file = SPIFFS.open(path, "r");
    size_t sent = server.streamFile(file, contentType);
    file.close();
    return true;
  }
  return false;
}

void handleFileUpload() {
  if (server.uri() != "/edit") return;
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    String filename = upload.filename;
    if (!filename.startsWith("/")) filename = "/"+filename;
    DBG_OUTPUT_PORT.printf("handleFileUpload Name: %s\n", filename.c_str());
    fsUploadFile = SPIFFS.open(filename, "w");
    filename = String();
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    // DBG_OUTPUT_PORT.printf("handleFileUpload Data: %s\n",upload.currentSize);
    if (fsUploadFile)
      fsUploadFile.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (fsUploadFile)
      fsUploadFile.close();
    DBG_OUTPUT_PORT.printf("handleFileUpload Size: %d\n", upload.totalSize);
    if (upload.filename == configFile) {
      loadConfig();
    }
    if (upload.filename == schedFile) {
      loadSched();
    }
  }
}

void handleFileDelete() {
  if (server.args() == 0) return server.send(500, "text/plain", "BAD ARGS");
  String path = server.arg(0);
  DBG_OUTPUT_PORT.printf("handleFileDelete: %s\n", path.c_str());
  if (path == "/")
    return server.send(500, "text/plain", "BAD PATH");
  if (!SPIFFS.exists(path))
    return server.send(404, "text/plain", "FileNotFound");
  SPIFFS.remove(path);
  server.send(200, "text/plain", "");
  path = String();
}

void handleFileCreate() {
  if (server.args() == 0)
    return server.send(500, "text/plain", "BAD ARGS");
  String path = server.arg(0);
  DBG_OUTPUT_PORT.printf("handleFileCreate: %s\n", path.c_str());
  if (path == "/")
    return server.send(500, "text/plain", "BAD PATH");
  if (SPIFFS.exists(path))
    return server.send(500, "text/plain", "FILE EXISTS");
  File file = SPIFFS.open(path, "w");
  if (file)
    file.close();
  else
    return server.send(500, "text/plain", "CREATE FAILED");
  server.send(200, "text/plain", "");
  path = String();
}

void handleFileList() {
  if (!server.hasArg("dir")) {
    server.send(500, "text/plain", "BAD ARGS"); return;
  }

  String path = server.arg("dir");
  DBG_OUTPUT_PORT.printf(" handleFileList: %s\n", path.c_str());
  Dir dir = SPIFFS.openDir(path);
  path = String();

  String output = "[";
  while (dir.next()) {
    File entry = dir.openFile("r");
    if (output != "[") output += ',';
    bool isDir = false;
    output += "{\"type\":\"";
    output += (isDir)?"dir":"file";
    output += "\",\"name\":\"";
    output += String(entry.name()).substring(1);
    output += "\"}";
    entry.close();
  }

  output += "]";
  server.send(200, "text/json", output);
}

void loadConfig() {
  if (SPIFFS.exists(configFile)) {
    File file = SPIFFS.open(configFile, "r");
    char json[500];
    memset(json, 0, sizeof(json));
    file.readBytes(json, sizeof(json));
    file.close();
    StaticJsonBuffer<500> jsonBuffer;
    JsonObject& root = jsonBuffer.parseObject(json);
    if (!root.success()) {
        DBG_OUTPUT_PORT.printf("json parse of configFile failed.\n");
    } else {
      const char *t;
      t = root["ssid"]; if (t) { strncpy(ssid, t, 30); }
      t = root["password"]; if (t) { strncpy(password, t, 30); }
      t = root["assid"]; if (t) { strncpy(assid, t, 30); }
      t = root["apassword"]; if (t) { strncpy(apassword, t, 30); }
      t = root["host"]; if (t) { strncpy(host, t, 30); }
      t = root["timeServer"]; if (t) { strncpy(timeServer, t, 30); }
      t = root["offsetGMT"]; if (t) { strncpy(offsetGMTstring, t, 10); }
      offsetGMT = atoi(offsetGMTstring);

      DBG_OUTPUT_PORT.printf("Config: host: %s ssid: %s assid: %s\n",
        host, ssid, assid);
      DBG_OUTPUT_PORT.printf("  timeServer: %s offsetGMT:%d\n",
        timeServer, offsetGMT);
    }
  } else {
    DBG_OUTPUT_PORT.printf("config file: %s not found\n", configFile);
  }
}

int computeDayFromTm(tmElements_t *tm) {
  // bits 0-6 = day of week
  // bits 8 == even Day
  // bit 9 == odd Day
  return (1 << (tm->Wday - 1)) | ( 1 << ((tm->Day & 1) + 8));
}

#ifdef EXTRA_DEBUG
void dumpSched(const char *m, struct _sched *s) {
  DBG_OUTPUT_PORT.printf(
    "%s: %08x z:%d s:%d d:%02x b:%d e:%d n:%d o:%d d:%d\n",
    m, s, s->zone, s->start, s->days, s->begin, s->end,
    s->next, s->stop, s->duration);
    DBG_OUTPUT_PORT.printf("%s: next:%s",
      m, timeStr(s->next+offsetGMT));
    DBG_OUTPUT_PORT.printf(" stop:%s\n", timeStr(s->stop+offsetGMT));
}
#endif

void computeNext(struct _sched *s) {
  if (now() < 10000000) {
    s->next = s->stop = 0;
    return;
  }
  time_t midnightLocal = (now() + offsetGMT) / 86400;
  if (s->begin > 10000000 && s->begin > now())
    midnightLocal = s->begin / 86400;
  midnightLocal = midnightLocal * 86400;
  time_t midnightLocalAsGmt = midnightLocal - offsetGMT;
  int day = 0xffff;
  s->next = midnightLocalAsGmt + s->start - 86400;
  do {
    s->next += 86400;
    tmElements_t tm;
    breakTime(s->next + offsetGMT, tm);
    day = computeDayFromTm(&tm);
    if (s->next > s->end) {
      s->next = 0;
      break;
    }
    #ifdef EXTRA_DEBUG
    dumpSched("computeNext", s);
    DBG_OUTPUT_PORT.printf("computeNext: day=%02x tm.Day=%d tm.Wday=%d\n",
      day, tm.Day, tm.Wday);
    #endif
  } while (now() >= s->next
      || (day & (s->days)) == 0);
  if (s->stop == 0 && s->next > 0) {
    s->stop = s->next + s->duration;
  }
}

void checkSched() {
  time_t timeNow = now();
  for (int i = 0; i < MAX_SCHED; i++) {
    if (sched[i].zone > 0) {
      #ifdef EXTRA_DEBUG
      dumpSched("checkSched", &sched[i]);
      #endif
      if (sched[i].stop > 0 && timeNow >= sched[i].stop) {
        sched[i].stop = 0;
        if (sched[i].next > 0) {
          sched[i].stop = sched[i].next + sched[i].duration;
        }
        if (sched[i].zone < 9) {
          relayState &= ~(1 << (sched[i].zone - 1));
          setRelays();
        }
      }
      if (sched[i].next > 0 && timeNow >= sched[i].next) {
        if (sched[i].zone == 100) {
          // reset
        } else {
          relayState |= (1 << (sched[i].zone - 1));
          setRelays();
        }
        computeNext(&sched[i]);
      }
    }
  }
}

void recalcSched() {
  for (int i = 0; i < MAX_SCHED; i++) {
    if (sched[i].zone > 0) {
      computeNext(&sched[i]);
    }
  }
}

void loadSched() {
  if (SPIFFS.exists(schedFile)) {
    File file = SPIFFS.open(schedFile, "r");
    char json[1000];
    memset(json, 0, sizeof(json));
    file.readBytes(json, sizeof(json));
    file.close();
    StaticJsonBuffer<1000> jsonBuffer;
    JsonArray& root = jsonBuffer.parseArray(json);
    if (!root.success()) {
        DBG_OUTPUT_PORT.printf("json parse of schedFile failed.\n");
    } else {
      tmElements_t tm;
      relayState = 0;
      setRelays();
      for (int i = 0; i < MAX_SCHED; i++) {
        const char *zoneString = root[i]["zone"];
        const char *startTime = root[i]["startTime"];
        sched[i].zone = 0;
        sched[i].next = 0;
        sched[i].duration = 0;
        sched[i].stop = 0;
        if (zoneString && zoneString[0] != '\0'
            && startTime && startTime[0] != '\0') {
//
          if (strstr(zoneString, "Zone ") == zoneString) {
            sched[i].zone = zoneString[5] - '0';
          }

          if (strstr(zoneString, "Restart") == zoneString) {
            sched[i].zone = 100;
          }

          sched[i].start = atoi(startTime) * 3600 +
                           atoi(startTime + 3) * 60;

          const char *beginDate = root[i]["begin"];
          if (!beginDate || beginDate[0] == '\0') {
            sched[i].begin = 10000000;   // more or less beginning of time
          } else  {
            tm.Second = tm.Hour = tm.Minute = tm.Wday = 0;
            tm.Year = atoi(beginDate) - 1970;
            tm.Month = atoi(beginDate+5);
            tm.Day = atoi(beginDate+8);
            sched[i].begin = makeTime(tm) - offsetGMT;
          }

          const char *endDate = root[i]["end"];
          if (!endDate || endDate[0] == '\0') {
            sched[i].end = 0x7fffffff;   // more or less end of time
          } else {
            tm.Second = tm.Hour = tm.Minute = tm.Wday = 0;
            tm.Year = atoi(endDate) - 1970;
            tm.Month = atoi(endDate+5);
            tm.Day = atoi(endDate+8);
            sched[i].end = makeTime(tm) - offsetGMT;
          }

          const char *duration = root[i]["duration"];
          if (!duration || duration[0] == '\0') {
            sched[i].duration = 15 * 60;  // default = 15 minutes
          } else {
            sched[i].duration = atoi(duration) * 60;
          }


          sched[i].days = 0;
          for (int j = 0; j < 10; j++) {
            const char *dayString = root[i]["days"][j];
            if (!dayString) break;
            if (strstr(dayString, "Sunday") == dayString )
              sched[i].days |= 1 << 0;
            if (strstr(dayString, "Monday") == dayString )
              sched[i].days |= 1 << 1;
            if (strstr(dayString, "Tuesday") == dayString )
              sched[i].days |= 1 << 2;
            if (strstr(dayString, "Wednesday") == dayString )
              sched[i].days |= 1 << 3;
            if (strstr(dayString, "Thursday") == dayString )
              sched[i].days |= 1 << 4;
            if (strstr(dayString, "Friday") == dayString )
              sched[i].days |= 1 << 5;
            if (strstr(dayString, "Saturday") == dayString )
              sched[i].days |= 1 << 6;
            if (strstr(dayString, "Even") == dayString )
              sched[i].days |= 1 << 8;
            if (strstr(dayString, "Odd") == dayString )
            sched[i].days |= 1 << 9;
          }
          if (sched[i].days == 0) sched[i].days = 0xffff;

          computeNext(&sched[i]);

          #ifdef EXTRA_DEBUG
          dumpSched("schedlist", &sched[i]);
          #endif
          DBG_OUTPUT_PORT.printf("schedule %d loaded next: %s duration %d\n",
              i, timeStr(sched[i].next + offsetGMT), sched[i].duration);
        }
      }
    }
  } else {
    DBG_OUTPUT_PORT.printf("sched file: %s not found\n", schedFile);
  }
}

void setRelays() {
  DBG_OUTPUT_PORT.printf("set relays=%02x\n", relayState);
  SPI.transfer(relayState);
  u8g2.setDrawColor(0);
  u8g2.drawBox(0, 64 - 12, 128, 12);
  for (int i = 0; i < 8; i++) {
    if (relayState & (1 << i)) {
      u8g2.setDrawColor(1);
      u8g2.drawBox(i * 16 + 2, 64 - 12, 12, 12);
      u8g2.setDrawColor(0);
      u8g2.drawGlyph(i * 16 + 6, 64 - 11, i + '1');
    } else {
      u8g2.setDrawColor(0);
      u8g2.drawBox(i * 16 + 2, 64 - 12, 12, 12);
      u8g2.setDrawColor(1);
      u8g2.drawGlyph(i * 16 + 6, 64 - 11, i + '1');
    }
  }
  u8g2.sendBuffer();
}

void displayStatus(int line, const char *fmt, ...) {
  int count;
  char *buffer;
  va_list Arglist;

  count = 0;
  buffer = reinterpret_cast<char*>(malloc(128));
  if (buffer == NULL) return;
  va_start(Arglist, fmt);
  vsnprintf(buffer, 128, fmt, Arglist); // NOLINT

  u8g2.setDrawColor(0);
  u8g2.drawBox(0, line * 12, 128, 12);
  u8g2.setDrawColor(1);
  u8g2.drawStr(0, line * 12, buffer);
  u8g2.sendBuffer();
  free(buffer);
}

void clearTimedFunc(int *id, const char *name) {
  DBG_OUTPUT_PORT.printf("clearTimedFunc %s\n", name);
  if (*id > -1) {
    DBG_OUTPUT_PORT.printf("clearingTimer id=%d\n", *id);
    timer.disable(*id);
    timer.deleteTimer(*id);
  }
  *id = -1;
}

int setTimedFunc(bool repeat, int *id, sint32_t t,
    void (*func)(), const char *name) {
  if (*id > -1) {
    #ifdef EXTRA_DEBUG
    DBG_OUTPUT_PORT.printf("clearingTimer id=%d\n", *id);
    #endif
    timer.disable(*id);
    timer.deleteTimer(*id);
  }
  *id = -1;
  if (t > 0) {
    if (repeat) {
      *id = timer.setInterval(t, func);
    } else {
      *id = timer.setTimeout(t, func);
    }
  }
  #ifdef EXTRA_DEBUG
  DBG_OUTPUT_PORT.printf("setTimedFunc %d %d %s %08x id=%d\n",
    repeat, t, name, func, *id);
  for (int i = 0; i < timer.MAX_TIMERS; i++) {
    if (timer.callbacks[i]) {
      DBG_OUTPUT_PORT.printf("%d cb:%08x num:%d numMax:%d\n",
        i, timer.callbacks[i], timer.numRuns[i], timer.maxNumRuns[i]);
    }
  }
  #endif
  return(*id);
}

// LED CONTROL
int blinkerTimerId = -1;
#define setBlinker(t) setTimedFunc(true, &blinkerTimerId, t, blinker, "blinker")
void blinker() {
  static bool onoff = false;
  onoff = !onoff;
  digitalWrite(BUILTIN_LED, onoff?HIGH:LOW);
}

// mdns
bool mdnsStarted = false;
void startMDNS() {
  if (mdnsStarted) return;
  mdnsStarted = true;
  MDNS.begin(host);
  DBG_OUTPUT_PORT.printf("MDNS Starting\nOpen http://%s.local or http://%s/\n",
    host, apMode ? WiFi.softAPIP().toString().c_str() :
                   WiFi.localIP().toString().c_str());
}

// NTP

void setRtc() {
  rtc.setDate(day(), weekday() - 1, month(), year() < 2000 ? 1 : 0,
    year()%100);
  rtc.setTime(hour(), minute(), second());
}

bool ntpStarted = false;
void startNTP() {
    if (timeServer[0] == '\0') return;
    if (rtcValid) return;
    if (ntpStarted) return;
    ntpStarted = true;
    DBG_OUTPUT_PORT.printf("Starting NTP %s\n", timeServer);
    NTP.begin(timeServer, 0, false);
}
void stopNTP() {
    NTP.stop();
    ntpStarted = false;
}
void recalcSched();  // forward ref
void handleNtpSync(NTPSyncEvent_t event) {
  if (event) {
    DBG_OUTPUT_PORT.printf("NTP Error %d\n", event);
  } else {
    DBG_OUTPUT_PORT.printf("NTP Successfull\n");
    timer.setTimeout(1000, recalcSched);
    if (hasRtc && !rtcValid) {
        timer.setTimeout(1000, setRtc);
        stopNTP();
    }
  }
}

const char *timeStr(time_t t) {
  static char tmbuf[20];
  if (t < 10000000) {
    strncpy(tmbuf, "Not Set", sizeof(tmbuf));
    return tmbuf;
  }
  tmElements_t tm;
  breakTime(t, tm);
  snprintf(tmbuf, sizeof(tmbuf), "%02d:%02d:%02d %04d/%02d/%02d",
    tm.Hour, tm.Minute, tm.Second, tm.Year + 1970, tm.Month, tm.Day);
  return tmbuf;
}

int tickId = -1;
void checkSched();  // forward ref
void tick() {
  const char *tx = timeStr(now() > 100000000 ? now()+offsetGMT : 0);
  #ifdef EXTRA_DEBUG
    DBG_OUTPUT_PORT.printf("now(): %d %s\n", now(), tx);
  #endif
  displayStatus(1, "time %s\n", tx);
  checkSched();
}

// WIFI STATUS CHANGES
int apModeTimerId = -1;
int staModeTimerId = -1;
#define setApModeTimeout(t) \
  setTimedFunc(false, &apModeTimerId, t, apModeTimeout, "apModeTimeout")
#define setStaModeTimeout(t) \
  setTimedFunc(false, &staModeTimerId, t, staModeTimeout, "staModeTimeout")
void apModeTimeout() {
  DBG_OUTPUT_PORT.printf("apModeTimeout\n");
  apModeTimerId = -1;
  setApMode(true);
  setBlinker(100);
  if (ssid[0] != '\0') {   // if no ssid
    setStaModeTimeout(600000);
  }
}

void staModeTimeout() {
  DBG_OUTPUT_PORT.printf("staModeTimeout\n");
  staModeTimerId = -1;
  setApMode(false);
  setBlinker(50);
  setApModeTimeout(60000);
}

void setApMode(bool mode) {
    if (apMode == mode) return;
    apMode = mode;
    if (apMode) {
      DBG_OUTPUT_PORT.printf("going to AP mode\n");
      WiFi.disconnect();
      delay(500);
      WiFi.mode(WIFI_AP);
      apMode = true;
      uint8_t mac[6];
      delay(500);
      WiFi.softAPmacAddress(mac);
      delay(500);
      if (assid[0] == '\0') {
        // making a nice unique SSID
        snprintf(assid, sizeof(host), "%s_%02x%02x%02x",
          host, mac[3], mac[4], mac[5]);
      }
      DBG_OUTPUT_PORT.printf("SoftAP ssid:%s\n", assid);
      if (strlen(apassword) >= 8) {  // softAP doesn't work if password < 8
        WiFi.softAP(assid, apassword);
      } else {
        WiFi.softAP(assid);
      }
      DBG_OUTPUT_PORT.printf("AP mode. IP address: %s\n",
        WiFi.softAPIP().toString().c_str());
      #ifdef EXTRA_DEBUG
      WiFi.printDiag(DBG_OUTPUT_PORT);
      #endif
      displayStatus(0, "AP:%s %s", WiFi.softAPIP().toString().c_str(), assid);
    } else {
      DBG_OUTPUT_PORT.printf("going to STA mode %s\n", ssid);
      displayStatus(0, "Try: %s", ssid);
      WiFi.mode(WIFI_STA);
      delay(500);
      if (password[0] == '\0') {
        WiFi.begin(ssid);
      } else {
        WiFi.begin(ssid, password);
      }
    }
}

WiFiEventHandler onSTAGotIPHandler;
void onSTAGotIP(WiFiEventStationModeGotIP ipInfo) {
  if (apMode) return;
  DBG_OUTPUT_PORT.printf("Got IP: %s\n", ipInfo.ip.toString().c_str());
  displayStatus(0, "STA:%s %s %d", WiFi.localIP().toString().c_str(),
      ssid, WiFi.RSSI());
  setApModeTimeout(0);
  setBlinker(500);
  timer.setTimeout(1000, startMDNS);
  timer.setTimeout(5000, startNTP);
}

WiFiEventHandler onSTADisconnectedHandler;
void onSTADisconnected(WiFiEventStationModeDisconnected event_info) {
  if (apMode) return;
  Serial.printf("Disconnected from SSID: %s Reason: %d\n",
    event_info.ssid.c_str(), event_info.reason);
  displayStatus(0, "DIS:%s %d", event_info.ssid.c_str(), event_info.reason);
  setBlinker(50);
  if (apModeTimerId < 0) setApModeTimeout(600000);
  timer.setTimeout(20, stopNTP);
}

void setup(void) {
  pinMode(BUILTIN_LED, OUTPUT);
  setBlinker(50);

  SPI.setDataMode(SPI_MODE0);
  SPI.setBitOrder(MSBFIRST);
  SPI.setClockDivider(SPI_CLOCK_DIV16);
  SPI.setHwCs(true);
  SPI.begin();
  SPI.transfer(relayState);

  DBG_OUTPUT_PORT.begin(74880);
  DBG_OUTPUT_PORT.setDebugOutput(false);
  #ifdef EXTRA_DEBUG
  DBG_OUTPUT_PORT.setDebugOutput(true);
  #endif

  u8g2.begin();
  u8g2.setFont(u8g2_font_helvR08_tf);
  u8g2.setFontRefHeightExtendedText();
  u8g2.setDrawColor(1);
  u8g2.setFontPosTop();
  u8g2.setFontDirection(0);
  u8g2.clearBuffer();
  displayStatus(0, "Start...");


  SPIFFS.begin();
  {
    Dir dir = SPIFFS.openDir("/");
    while (dir.next()) {
      String fileName = dir.fileName();
      size_t fileSize = dir.fileSize();
      DBG_OUTPUT_PORT.printf("FS File: %s, size: %s\n",
        fileName.c_str(), formatBytes(fileSize).c_str());
    }
    DBG_OUTPUT_PORT.printf("\n");
  }


  loadConfig();

  #ifdef EXTRA_DEBUG
  for (int address = 1; address < 127; address++) {
    // The i2c_scanner uses the return value of
    // the Write.endTransmisstion to see if
    // a device did acknowledge to the address.
    Wire.beginTransmission(address);
    int error = Wire.endTransmission();

    if (error == 0) {
      DBG_OUTPUT_PORT.printf("I2C device found at address 0x%02x\n", address);
    } else {
      if (error == 4) {
        DBG_OUTPUT_PORT.printf("Unknown error at address 0x%02x\n", address);
      }
    }
  }
  #endif

  // pcf8563
  Wire.beginTransmission(RTCC_R>>1);
  if (Wire.endTransmission() == 0) {
    hasRtc = true;
    DBG_OUTPUT_PORT.printf("RTC 8563 Found @ %02x\n", RTCC_R>>1);
    displayStatus(0, "RTC found");
    Wire.beginTransmission(RTCC_R>>1);
    Wire.write(RTCC_STAT1_ADDR);
    Wire.endTransmission();
    Wire.requestFrom(RTCC_R >> 1, 3);   // request 3 bytes
    byte stat1 = Wire.read();
    byte stat2 = Wire.read();
    byte stat3 = Wire.read();
    DBG_OUTPUT_PORT.printf("RTC 8563 status %02x %02x %02x\n",
      stat1, stat2, stat3);
    // we'll init the control values 1 and 2
    Wire.beginTransmission(RTCC_R>>1);
    Wire.write(RTCC_STAT1_ADDR);
    Wire.write(0);
    Wire.write(0);
    Wire.endTransmission();

    if ((stat1  & (1 << 7 | 1 << 5)) == 0) {  // if clock is running
      if ((stat3 & 0x80) == 0) rtcValid = true;
    } else {
      displayStatus(0, "RTC not valid");
    }
    DBG_OUTPUT_PORT.printf("RTC 8563 time is %svalid: %s %s\n",
      rtcValid ? "" : "not ", rtc.formatDate(RTCC_DATE_ASIA),
      rtc.formatTime(RTCC_TIME_HMS));
    if (rtcValid) {
      setTime(rtc.getHour(), rtc.getMinute(), rtc.getSecond(),
        rtc.getDay(), rtc.getMonth(), 2000 + rtc.getYear());
    }
  }

  loadSched();

  setRelays();

  // WIFI INIT

  onSTAGotIPHandler = WiFi.onStationModeGotIP(onSTAGotIP);
  onSTADisconnectedHandler = WiFi.onStationModeDisconnected(onSTADisconnected);
  NTP.onNTPSyncEvent(handleNtpSync);
  WiFi.persistent(false);
  if (ssid[0] == '\0')  {   // no ssid, therefore go to apmode
    apMode = false;  // forced mode change
    setApMode(true);
  } else {
    setApModeTimeout(60000);
    apMode = true;  // forced mode change
    setApMode(false);
  }

  setTimedFunc(true, &tickId, 1000, tick, "tick");

// SERVER INIT
  // list directory
  server.on("/list", HTTP_GET, handleFileList);
  // load editor
  server.on("/edit", HTTP_GET, []() {
    if (!handleFileRead("/edit.html")) {
      server.send(404, "text/plain", "FileNotFound");
    }
  });
  // create file
  server.on("/edit", HTTP_PUT, handleFileCreate);
  // delete file
  server.on("/edit", HTTP_DELETE, handleFileDelete);
  // first callback is called after the request has
  // ended with all parsed arguments
  // second callback handles file uploads at that location
  server.on("/edit", HTTP_POST,
    []() { server.send(200, "text/plain", ""); },
    handleFileUpload);

  // called when the url is not defined here
  // use it to load content from SPIFFS
  server.onNotFound([]() {
    if (!handleFileRead(server.uri()))
      server.send(404, "text/plain", "FileNotFound");
  });

  server.on("/status", HTTP_GET, []() {
    String json = "{";
    for (int i = 0; i < 8; i++) {
      json += String("\"zone") +
        String(i) +
        String(relayState&(1 << i) ? "\":\"on\"," : "\":\"off\",");
    }
    json += String("\"time\":")+String(now())+",";
    json += String("\"offsetGMT\":")+String(offsetGMT)+",";
    json += String("\"host\":\"")+String(host)+"\"";
    json += "}";
    server.send(200, "text/json", json);
    DBG_OUTPUT_PORT.printf("status %s\n", json.c_str());
    json = String();
  });

  server.on("/toggle", HTTP_GET, []() {
    int i = 0;
    if (server.args() > 0)
      i = atoi(server.arg("zone").c_str());
    relayState ^= (1 << i);
    server.send(200, "text/text", "OK");
    DBG_OUTPUT_PORT.printf("toggle %d = %s\n", i,
      relayState&(1 << i) ? "on" : "off");
    setRelays();
  });
  server.on("/settime", HTTP_GET, []() {
    int i = 0;
    if (server.args() > 0) {
      i = atoi(server.arg("time").c_str());
    }
    setTime(i);
    if (hasRtc) {
      setRtc();
    }
    server.send(200, "text/text", "OK");
    DBG_OUTPUT_PORT.printf("settime %d\n", i);
    setRelays();
  });

  server.on("/restart", HTTP_GET, []() {
    server.send(200, "text/text",
      "Restarting.... Wait a minute or so and then refresh.");
    delay(2000);
    ESP.restart();
  });

  server.begin();
  DBG_OUTPUT_PORT.printf("HTTP server started\n");
}


void loop(void) {
// deal with timer (SimpleTimer)
  timer.run();
// deal with http server.
  server.handleClient();
}
