/*
The MIT License (MIT)

Copyright (c) 2020-2021 riraosan.github.io

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#define TS_ENABLE_SSL  // Don't forget it for ThingSpeak.h!!
#include <Arduino.h>
//#include <LittleFS.h>
//#define SPIFFS LittleFS
#include <AutoConnect.h>
#include <BME280Class.h>
#include <Button2.h>
#include <ESPmDNS.h>
#include <LED_DisPlay.h>
#include <ThingSpeak.h>
#include <Ticker.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <secrets_sample.h>
#include <timezone.h>

// log
#include <esp32-hal-log.h>
// WiFi Connection
#define HOSTNAME    "ATOMLITE"
#define AP_NAME     "ATOMLITE-G-AP"
#define HTTP_PORT   80
// NTP Clock
#define TIME_ZONE   "JST-9"
#define NTP_SERVER1 "ntp.nict.jp"
#define NTP_SERVER2 "ntp.jst.mfeed.ad.jp"
#define NTP_SERVER3 "asia.pool.ntp.org"
// BME280
#define SDA         21
#define SCL         25
// WiFi reset
#define BUTTON_PIN  39

WebServer Server;
AutoConnect Portal(Server);
AutoConnectConfig Config;  // Enable autoReconnect supported on v0.9.4
AutoConnectAux Timezone;

Ticker clocker;
Ticker sensorChecker;
Ticker blockOFF;
Ticker blockON;

LED_DisPlay led;
Button2 button = Button2(BUTTON_PIN);
WiFiClientSecure _client;

bool sendDataflag = false;

float temperature;
float humidity;
float pressure;

void rootPage(void) {
  String content = R"***(
  <!DOCTYPE html>
  <html>
  <head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <script type="text/javascript">
  setTimeout("location.reload()", 1000)
  </script>
  </head>
  <body>
  <h2 align="center" style="color:blue;margin:20px;">Hello, world</h2>
  <h3 align="center" style="color:gray;margin:10px;">{{DateTime}}</h3>
  <p style="text-align:center;">Reload the page to update the time.</p>
  <p></p>
  <p style="padding-top:15px;text-align:center"> __AUTOCONNECT_LINK__ </p>
  </body>
  </html>
  )***";

  content.replace("__AUTOCONNECT_LINK__", AUTOCONNECT_LINK(COG_24));

  static const char* wd[7] = {"Sun", "Mon", "Tue", "Wed", "Thr", "Fri", "Sat"};
  struct tm* tm;
  time_t t;
  char dateTime[26];

  t  = time(NULL);
  tm = localtime(&t);
  sprintf(dateTime, "%04d/%02d/%02d(%s) %02d:%02d:%02d.",
          tm->tm_year + 1900,
          tm->tm_mon + 1,
          tm->tm_mday,
          wd[tm->tm_wday],
          tm->tm_hour,
          tm->tm_min,
          tm->tm_sec);

  content.replace("{{DateTime}}", String(dateTime));
  Server.send(200, "text/html", content);
}

void startPage(void) {
  // Retrieve the value of AutoConnectElement with arg function of WebServer class.
  // Values are accessible with the element name.
  String tz = Server.arg("timezone");

  for (uint8_t n = 0; n < sizeof(TZ) / sizeof(Timezone_t); n++) {
    String tzName = String(TZ[n].zone);
    if (tz.equalsIgnoreCase(tzName)) {
      configTime(TZ[n].tzoff * 3600, 0, TZ[n].ntpServer);
      log_d("Time zone: %s", tz.c_str());
      log_d("ntp server: %s", String(TZ[n].ntpServer).c_str());
      break;
    }
  }

  // The /start page just constitutes timezone,
  // it redirects to the root page without the content response.
  Server.sendHeader("Location", String("http://") + Server.client().localIP().toString() + String("/"));
  Server.send(302, "text/plain", "");
  Server.client().flush();
  Server.client().stop();
}

void otaPage(void) {
  String content = R"***(
  <!DOCTYPE html>
  <html>
  <head>
  <meta charset="UTF-8" name="viewport" content="width=device-width, initial-scale=1">
  </head>
  <body>
  Place the root page with the sketch application.&ensp;
  __AC_LINK__
  </body>
  </html>
  )***";

  content.replace("__AC_LINK__", String(AUTOCONNECT_LINK(COG_16)));
  Server.send(200, "text/html", content);
}

void sendThingSpeakChannel(float temperature, float humidity, float pressure) {
  char buffer1[16] = {0};
  char buffer2[16] = {0};
  char buffer3[16] = {0};

  sprintf(buffer1, "%2.1f", temperature);
  sprintf(buffer2, "%2.1f", humidity);
  sprintf(buffer3, "%4.1f", pressure);

  String tempe(buffer1);
  String humid(buffer2);
  String press(buffer3);

  ThingSpeak.setField(1, tempe);
  ThingSpeak.setField(2, humid);
  ThingSpeak.setField(3, press);

  // write to the ThingSpeak channel
  int code = ThingSpeak.writeFields(SECRET_CH_ID, SECRET_WRITE_APIKEY);
  if (code == 200)
    log_d("Channel update successful.");
  else
    log_d("Problem updating channel. HTTP error code %d", code);
}

void _checkSensor(void) { sendDataflag = true; }

String getLEDTime(void) {
  time_t t      = time(NULL);
  struct tm* tm = localtime(&t);

  char buffer[16] = {0};
  sprintf(buffer, "%02d%02d", tm->tm_hour, tm->tm_min);

  return String(buffer);
}

String getTime(void) {
  time_t t      = time(NULL);
  struct tm* tm = localtime(&t);

  char buffer[128] = {0};
  sprintf(buffer, "%04d-%02d-%02dT%02d:%02d:%02d+0900", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);

  return String(buffer);
}

void initClock(void) {
  configTzTime(TIME_ZONE, NTP_SERVER1, NTP_SERVER2, NTP_SERVER3);
}

void initBME280(void) {
  bme280.setup(SDA, SCL, MODE::WEATHER_STATION);
  sensorChecker.attach(60, _checkSensor);
}

void released(Button2& btn) {
  WiFi.disconnect(true, true);
  ESP.restart();
}

void initButton(void) {
  button.setReleasedHandler(released);
}

void initThingSpeak(void) {
  _client.setCACert(CERTIFICATE);
  ThingSpeak.begin(_client);
}

void sendThingSpeakData(void) {
  if (bme280.getTemperature(temperature) && bme280.getHumidity(humidity) && bme280.getPressure(pressure)) {
    sendThingSpeakChannel(temperature, humidity, pressure);
    led.drawpix(0, CRGB::Green);
  } else {
    log_e("temperature = %f, humidity = %f, pressure = %f", temperature, humidity, pressure);
    led.drawpix(0, CRGB::Red);
  }
}

void setNtpClockNetworkInfo(void) {
  char buffer[255] = {0};

  sprintf(buffer, "%s %s %s", WiFi.SSID().c_str(), WiFi.macAddress().c_str(), WiFi.localIP().toString().c_str());
  String networkInfo(buffer);

  ThingSpeak.setStatus(networkInfo);  // ThingSpeak limits this to 255 bytes.
}

void initLED(void) {
  led.begin(1);  // for ATOM Lite
  led.setTaskName("ATOM_LITE_LED");
  led.setTaskPriority(2);
  led.start();
  delay(50);
  led.setBrightness(30);
}

void initAutoConnect(void) {
  Config.autoReconnect = true;
  Config.ota           = AC_OTA_BUILTIN;
  Portal.config(Config);

  // Load aux. page
  Timezone.load(AUX_TIMEZONE);
  // Retrieve the select element that holds the time zone code and
  // register the zone mnemonic in advance.
  AutoConnectSelect& tz = Timezone["timezone"].as<AutoConnectSelect>();
  for (uint8_t n = 0; n < sizeof(TZ) / sizeof(Timezone_t); n++) {
    tz.add(String(TZ[n].zone));
  }

  Portal.join({Timezone});  // Register aux. page

  // Behavior a root path of ESP8266WebServer.
  Server.on("/", rootPage);
  Server.on("/start", startPage);  // Set NTP server trigger handler
  Server.on("/ota", otaPage);

  led.drawpix(0, CRGB::Red);

  // Establish a connection with an autoReconnect option.
  if (Portal.begin()) {
    log_i("WiFi connected: %s", WiFi.localIP().toString().c_str());
    led.drawpix(0, CRGB::Green);
    if (MDNS.begin(HOSTNAME)) {
      MDNS.addService("http", "tcp", HTTP_PORT);
      log_i("HTTP Server ready! Open http://%s.local/ in your browser\n", HOSTNAME);
    } else
      log_e("Error setting up MDNS responder");
  }
}

void setup(void) {
  pinMode(0, OUTPUT);
  digitalWrite(0, LOW);

  initLED();
  initAutoConnect();
  initBME280();
  initClock();
  initButton();
  initThingSpeak();
  sendThingSpeakData();
}

void loop(void) {
  Portal.handleClient();
  button.loop();

  // every 60 seconds
  if (sendDataflag) {
    sendThingSpeakData();
    sendDataflag = false;
  }

  delay(1);
}
