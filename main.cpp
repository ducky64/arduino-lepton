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
#include <atomic>
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
// controlled by the writing (sensor) task
uint8_t bufferWriteIndex = 0;  // buffer being written to, the other one is implicitly the read buffer; 0 means buffer not being read
std::atomic<uint8_t> bufferReaders{0};  // number of readers of the non-writing buffer, locks bufferWriteIndex if >0
// const size_t kMaxSimultaneousReaders = 8;
SemaphoreHandle_t bufferControlSemaphore = nullptr;  // mutex to control access to the write index / readers count
StaticSemaphore_t bufferControlSemaphoreBuf;



JPEGENC jpgenc;
const size_t kJpegBufferSize = 8192;

// converts frame into a jpeg, stored in jpegBuf, writing the output length to jpegLenOut
int encodeJpeg(uint8_t* frame, size_t frameWidth, size_t frameHeight, uint8_t* jpegBuf, size_t jpegBufLen, size_t* jpegLenOut) {
  JPEGENCODE enc;
  int rc;

  rc = jpgenc.open(jpegBuf, jpegBufLen);
  if (rc != JPEGE_SUCCESS) {
    ESP_LOGE("jpg", "Open error %i", rc);
    return rc;
  }

  if (rc == JPEGE_SUCCESS) {
    rc = jpgenc.encodeBegin(&enc, frameWidth, frameHeight, JPEGE_PIXEL_GRAYSCALE, JPEGE_SUBSAMPLE_444, JPEGE_Q_BEST);
    // jpgenc.encodeBegin(&enc, 160, 120, JPEGE_PIXEL_RGB565, JPEGE_SUBSAMPLE_444, JPEGE_Q_BEST);
    if (rc != JPEGE_SUCCESS) {
      ESP_LOGE("jpg", "encodeBegin error %i", rc);
      return rc;
    }
  }
  
  if (rc == JPEGE_SUCCESS) {
    rc = jpgenc.addFrame(&enc, frame, frameWidth);
    if (rc != JPEGE_SUCCESS) {
      ESP_LOGE("jpg", "addFrame error %i", rc);
      return rc;
    }
  }
  
  if (rc == JPEGE_SUCCESS) {
    *jpegLenOut = jpgenc.close();
  }
  return rc;
}


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
    if (!client.connected()) {
      break;
    }
    // size_t jpegSize;
    // if (encodeJpeg(vospiBuf[(bufferWriteIndex+1) % 2], 160, 120, jpegBuf, sizeof(jpegBuf), &jpegSize) != JPEGE_SUCCESS) {
    //   break;
    // }
    // client.write(kMjpegContentType, kMjpegContentTypeLen);
    // sprintf(buf, "%d\r\n\r\n", jpegSize);
    // client.write(buf, strlen(buf));
    // client.write(jpegBuf, jpegSize);  // TODO write framebuffer
    // client.write(kMjpegBoundary, kMjpegBoundaryLen);

    // get next frame
    // TODO IMPLEMENT ME
  }
}


const char kJpgHeader[] = "HTTP/1.1 200 OK\r\n" \
                          "Content-disposition: inline; filename=capture.jpg\r\n" \
                          "Content-type: image/jpeg\r\n\r\n";
const int kJpgHeaderLen = strlen(kJpgHeader);

void handle_jpg(void) {
  WiFiClient client = server.client();
  if (!client.connected()) return;

  uint8_t jpegBuf[kJpegBufferSize];
  size_t jpegSize;

  while (xSemaphoreTake(bufferControlSemaphore, portMAX_DELAY) != pdTRUE);
  uint8_t bufferReadIndex = (bufferWriteIndex + 1) % 2;
  bufferReaders++;
  assert(xSemaphoreGive(bufferControlSemaphore) == pdTRUE);

  int encodeStatus = encodeJpeg(vospiBuf[bufferReadIndex], 160, 120, jpegBuf, sizeof(jpegBuf), &jpegSize);

  bufferReaders--;

  if (encodeStatus == JPEGE_SUCCESS) {
    ESP_LOGI("main", "JPG created %i B", jpegSize);
    client.write(kJpgHeader, kJpgHeaderLen);
    client.write(jpegBuf, jpegSize);
  } else {
    server.send(200, "text / plain", "Error");
  }
}


void handleNotFound() {
  server.send(200, "text / plain", "Unknown request");
}


void Task_Server(void *pvParameters) {
  ESP_LOGI("main", "Start wifi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(10);  // yield
  }
  ESP_LOGI("main", "WiFi connected %s", WiFi.localIP().toString());

  server.on("/mjpeg", HTTP_GET, handle_mjpeg_stream);
  server.on("/jpg", HTTP_GET, handle_jpg);
  server.onNotFound(handleNotFound);
  server.begin();
  ESP_LOGI("main", "Server started");

  while (true) {
    server.handleClient();
    vTaskDelay(1);
  }
}


void Task_Lepton(void *pvParameters) {
  pinMode(kPinLepPwrdn, OUTPUT);
  digitalWrite(kPinLepPwrdn, HIGH);

  ESP_LOGI("main", "Start init");
  pinMode(kPinLepVsync, INPUT);
  bool beginResult = lepton.begin();
  ESP_LOGI("main", "Lepton init << %i", beginResult);

  while (!lepton.isReady()) {
    vTaskDelay(1);
  }

  bool result = lepton.enableVsync();
  ESP_LOGI("main", "Lepton Vsync << %i", result);

  while (true) {
    bool readError = false;
    bool readResult = lepton.readVoSpi(sizeof(vospiBuf[0]), vospiBuf[bufferWriteIndex], &readError);

    if (readError) {
      ESP_LOGW("main", "Read error, re-sync");
      delay(185);  // de-sync to re-sync
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

      // really jank AGC
      const size_t height = 120, width = 160;
      for (uint16_t y=0; y<height; y++) {
        for (uint16_t x=0; x<width; x++) {
          uint16_t pixel = ((uint16_t)vospiBuf[bufferWriteIndex][2*(y*width+x)] << 8) | vospiBuf[bufferWriteIndex][2*(y*width+x) + 1];
          
          pixel = (uint32_t)(pixel - min) * 255 / range;
          vospiBuf[bufferWriteIndex][y*width+x] = pixel;

          // pixel = (uint32_t)(pixel - min) * 65535 / range;
          // vospiBuf[bufferWriteIndex][2*(y*width+x)] = pixel >> 8;
          // vospiBuf[bufferWriteIndex][2*(y*width+x) + 1] = pixel & 0xff;
        }
      }

      while (xSemaphoreTake(bufferControlSemaphore, portMAX_DELAY) != pdTRUE);
      if (bufferReaders == 0) {
        bufferWriteIndex = (bufferWriteIndex + 1) % 2;
      }
      assert(xSemaphoreGive(bufferControlSemaphore) == pdTRUE);
    }

    vTaskDelay(1);
  }
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

  // initialize shared data structures
  bufferControlSemaphore = xSemaphoreCreateMutexStatic(&bufferControlSemaphoreBuf);
  assert(bufferControlSemaphore != nullptr);
  // bufferReadSemaphore = xSemaphoreCreateCountingStatic(kMaxSimultaneousReaders, 0, &bufferReadSemaphoreBuffer);

  // webserver is relatively low priority
  xTaskCreatePinnedToCore(Task_Server, "Task_Server", kJpegBufferSize + 4096, NULL, 1, NULL, ARDUINO_RUNNING_CORE);
  xTaskCreatePinnedToCore(Task_Lepton, "Task_Lepton", 4096, NULL, 4, NULL, ARDUINO_RUNNING_CORE);
}

void loop() {
  vTaskDelay(1000);
}
