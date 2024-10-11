#include <Arduino.h>
#include "lepton.h"
#include "lepton_util.h"

// web server code based on (BSD)
// https://github.com/arkhipenko/esp32-cam-mjpeg/blob/master/esp32_camera_mjpeg.ino
// and RTOS multiclient version (BSD)
// https://github.com/arkhipenko/esp32-cam-mjpeg-multiclient/blob/master/esp32_camera_mjpeg_multiclient.ino
#include <WiFi.h>
#include "WifiConfig.h"  // must define 'const char* ssid' and 'const char* password'
#include <WebServer.h>
#include <JPEGENC.h>

// Unused (so far) pinmaps from HDL
// touch_duck=TOUCH6, 6,
// flir=SPI2, 
// flir.sck=GPIO1, 39, 
// flir.mosi=GPIO5, 5, 
// flir.miso=GPIO4, 4, 
// i2c=I2CEXT0, 
// i2c.scl=GPIO39, 32, 
// i2c.sda=GPIO38, 31, 
// cam_rst=GPIO21, 23, 
// flir_rst=GPIO41, 34, 
// flir_pwrdn=GPIO40, 33, 
// flir_cs=GPIO2, 38, 
// flir_vsync=GPIO7, 7, 
// cam=DVP, 
// cam.xclk=GPIO13, 21, 
// cam.pclk=GPIO11, 19, 
// cam.href=GPIO47, 24, 
// cam.vsync=GPIO48, 25, 
// cam.y0=GPIO10, 18, 
// cam.y1=GPIO9, 17, 
// cam.y2=GPIO18, 11, 
// cam.y3=GPIO17, 10, 
// cam.y4=GPIO8, 12, 
// cam.y5=GPIO3, 15, 
// cam.y6=GPIO12, 20, 
// cam.y7=GPIO14, 22, 
// 0=USB, 
// 0.dp=GPIO20, 14, 
// 0.dm=GPIO19, 13

const int kPinLedR = 0;  // overlaps with strapping pin

const int kPinI2cScl = 39;
const int kPinI2cSda = 38;

const int kPinLepRst = 41;
const int kPinLepPwrdn = 40;
const int kPinLepCs = 2;
const int kPinLepVsync = 7;
const int kPinLepSck = 1;
const int kPinLepMosi = 5;
const int kPinLepMiso = 4;

SPIClass spi(HSPI);
TwoWire i2c(0);

FlirLepton lepton(i2c, spi, kPinLepCs, kPinLepRst);
uint8_t vospiBuf[2][160*120*3] = {0};  // up to RGB888, double-buffered
uint8_t bufferWriteIndex = 0;  // buffer being written to


WebServer server(80);


const char kMjpegHeader[] = "HTTP/1.1 200 OK\r\n" \
                      "Access-Control-Allow-Origin: *\r\n" \
                      "Content-Type: multipart/x-mixed-replace; boundary=123456789000000000000987654321\r\n";
const char kMjpegBoundary[] = "\r\n--123456789000000000000987654321\r\n";  // arbitrarily-chosen delimiter
const char kMjpegContentType[] = "Content-Type: image/jpeg\r\nContent-Length: ";  // written per frame
const int kMjpegHeaderLen = strlen(kMjpegHeader);
const int kMjpegBoundaryLen = strlen(kMjpegBoundary);
const int kMjpegContentTypeLen = strlen(kMjpegContentType);

void handle_mjpeg_stream(void)
{
  char buf[32];
  int s;

  WiFiClient client = server.client();

  client.write(kMjpegHeader, kMjpegHeaderLen);
  client.write(kMjpegBoundary, kMjpegBoundaryLen);

  while (true)
  {
    if (!client.connected()) break;
    cam.run();
    s = cam.getSize();
    client.write(kMjpegContentType, kMjpegContentTypeLen);
    // sprintf( buf, "%d\r\n\r\n", s );  // TODO write length
    client.write(buf, strlen(buf));
    // client.write((char *)cam.getfb(), s);  // TODO write framebuffer
    client.write(kMjpegBoundary, kMjpegBoundaryLen);
  }
}


const char kJpgHeader[] = "HTTP/1.1 200 OK\r\n" \
                          "Content-disposition: inline; filename=capture.jpg\r\n" \
                          "Content-type: image/jpeg\r\n\r\n";
const int kJpgHeaderLen= strlen(kJpgHeader);
uint8_t jpegBuf[32768];
JPEGENC jpgenc;

void handle_jpg(void) {
  WiFiClient client = server.client();
  if (!client.connected()) return;

  JPEGENCODE enc;
  int rc;
  rc = jpgenc.open(jpegBuf, sizeof(jpegBuf));
  if (rc != JPEGE_SUCCESS) {
    ESP_LOGE("jpg", "Open error %i", rc);
  }

  if (rc == JPEGE_SUCCESS) {
    rc = jpgenc.encodeBegin(&enc, 160, 120, JPEGE_PIXEL_GRAYSCALE, JPEGE_SUBSAMPLE_444, JPEGE_Q_BEST);
    // jpgenc.encodeBegin(&enc, 160, 120, JPEGE_PIXEL_RGB565, JPEGE_SUBSAMPLE_444, JPEGE_Q_BEST);
    if (rc != JPEGE_SUCCESS) {
      ESP_LOGE("jpg", "encodeBegin error %i", rc);
    }
  }
  
  if (rc == JPEGE_SUCCESS) {
    rc = jpgenc.addFrame(&enc, vospiBuf[(bufferWriteIndex+1) % 2], 160);
    if (rc != JPEGE_SUCCESS) {
      ESP_LOGE("jpg", "addFrame error %i", rc);
    }
  }
  
  if (rc == JPEGE_SUCCESS) {
    size_t encodedLen = jpgenc.close();
    ESP_LOGI("main", "JPG created %i B", encodedLen);
    client.write(kJpgHeader, kJpgHeaderLen);
    client.write(jpegBuf, encodedLen);
  } else {
    server.send(200, "text / plain", "Error");
  }
}


void handleNotFound() {
  server.send(200, "text / plain", "Unknown request");
}


void setup() {
  Serial.begin(115200);
  esp_log_level_set("*", ESP_LOG_INFO);
  esp_log_level_set("lepton", ESP_LOG_DEBUG);

  pinMode(kPinLedR, OUTPUT);
  digitalWrite(kPinLedR, LOW);

  delay(2000);
  digitalWrite(kPinLedR, HIGH);

  spi.begin(kPinLepSck, kPinLepMiso, -1, -1);
  i2c.begin(kPinI2cSda, kPinI2cScl, 400000);

  pinMode(kPinLepPwrdn, OUTPUT);
  digitalWrite(kPinLepPwrdn, HIGH);

  ESP_LOGI("main", "Start init");
  pinMode(kPinLepVsync, INPUT);
  bool beginResult = lepton.begin();
  ESP_LOGI("main", "Lepton init << %i", beginResult);

  ESP_LOGI("main", "Start wifi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);


  ESP_LOGI("main", "Wait for all ready");
  while (WiFi.status() != WL_CONNECTED);
  ESP_LOGI("main", "WiFi connected %s", WiFi.localIP().toString());

  ESP_LOGI("main", "Server started");
  server.on("/mjpeg", HTTP_GET, handle_mjpeg_stream);
  server.on("/jpg", HTTP_GET, handle_jpg);
  server.onNotFound(handleNotFound);
  server.begin();

  while (!lepton.isReady()) {
    delay(10);
  }

  bool result = lepton.enableVsync();
  ESP_LOGI("main", "Lepton Vsync << %i", result);

  digitalWrite(kPinLedR, LOW);
  ESP_LOGI("main", "Setup complete");

  // while (digitalRead(kPinLepVsync) == LOW);

  // lepton.end();
  // spi.end();
  // i2c.end();
  // digitalWrite(kPinLepPwrdn, LOW);
  // ESP_LOGI("main", "Lepton powerdown");
}

void loop() {
  bool readError = false;
  bool readResult = lepton.readVoSpi(sizeof(vospiBuf[0]), vospiBuf[bufferWriteIndex], &readError);

  if (readError) {
    // const int kIncr = 16;
    // for (int i=0; i<20; i++) {
    //   if (i == 10) {
    //     ESP_LOGI("main", "");
    //   }
    //   ESP_LOGI("main", "  %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x",
    //       vospiBuf[i*kIncr+0], vospiBuf[i*kIncr+1], vospiBuf[i*kIncr+2], vospiBuf[i*kIncr+3],
    //       vospiBuf[i*kIncr+4], vospiBuf[i*kIncr+5], vospiBuf[i*kIncr+6], vospiBuf[i*kIncr+7],
    //       vospiBuf[i*kIncr+8], vospiBuf[i*kIncr+9], vospiBuf[i*kIncr+10], vospiBuf[i*kIncr+11],
    //       vospiBuf[i*kIncr+12], vospiBuf[i*kIncr+13], vospiBuf[i*kIncr+14], vospiBuf[i*kIncr+15]);
    // }

    ESP_LOGW("main", "Read error, re-establishing sync");
    delay(185);  // establish sync
    // while (digitalRead(kPinLepVsync) == LOW);
  }

  if (readResult) {
    digitalWrite(kPinLedR, !digitalRead(kPinLedR));

    uint16_t min, max;
    u16_frame_min_max(vospiBuf[bufferWriteIndex], 160, 120, &min, &max);
    uint16_t range = max - min;
    if (range == 0) {  // avoid division by zero
      ESP_LOGW("main", "empty thermal image");
      range = 1;
    }

    // flip active buffer
    uint8_t lastBuf = bufferWriteIndex;
    bufferWriteIndex = (bufferWriteIndex + 1) % 2;

    // really jank AGC
    const size_t height = 120, width = 160;
    for (uint16_t y=0; y<height; y++) {
      for (uint16_t x=0; x<width; x++) {
        uint16_t pixel = ((uint16_t)vospiBuf[lastBuf][2*(y*width+x)] << 8) | vospiBuf[lastBuf][2*(y*width+x) + 1];
        
        pixel = (uint32_t)(pixel - min) * 255 / range;
        vospiBuf[lastBuf][y*width+x] = pixel;

        // pixel = (uint32_t)(pixel - min) * 65535 / range;
        // vospiBuf[lastBuf][2*(y*width+x)] = pixel >> 8;
        // vospiBuf[lastBuf][2*(y*width+x) + 1] = pixel & 0xff;
      }
    }
  }

  server.handleClient();
}
