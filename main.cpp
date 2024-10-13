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

FlirLepton lepton(i2c, spi, kPinLepCs, kPinLepRst, kPinLepPwrdn);
uint8_t jpegencPixelType = JPEGE_PIXEL_GRAYSCALE;
uint8_t vospiBuf[2][160*120*3] = {0};  // up to RGB888, double-buffered
// controlled by the writing (sensor) task
uint8_t bufferWriteIndex = 0;  // buffer being written to, the other one is implicitly the read buffer; 0 means buffer not being read
std::atomic<uint8_t> bufferReaders{0};  // number of readers of the non-writing buffer, locks bufferWriteIndex if >0
SemaphoreHandle_t bufferControlSemaphore = nullptr;  // mutex to control access to the write index / readers count
StaticSemaphore_t bufferControlSemaphoreBuf;


JPEGENC jpgenc;
const size_t kJpegBufferSize = 16384;

// converts frame into a jpeg, stored in jpegBuf, writing the output length to jpegLenOut
int encodeJpeg(uint8_t* frame, size_t frameWidth, size_t frameHeight, uint8_t ucPixelType, uint8_t* jpegBuf, size_t jpegBufLen, size_t* jpegLenOut) {
  JPEGENCODE enc;
  int rc;

  rc = jpgenc.open(jpegBuf, jpegBufLen);
  if (rc != JPEGE_SUCCESS) {
    ESP_LOGE("jpg", "Open error %i", rc);
    return rc;
  }

  if (rc == JPEGE_SUCCESS) {
    rc = jpgenc.encodeBegin(&enc, frameWidth, frameHeight, ucPixelType, JPEGE_SUBSAMPLE_444, JPEGE_Q_BEST);
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

const size_t kMaxStreamingClients = 4;
size_t numStreamingClients = 0;  // synchronized with the streamingClients buffer
WiFiClient streamingClients[kMaxStreamingClients];  // always continuous from zero when mutex is released

TaskHandle_t streamingTask = nullptr;
SemaphoreHandle_t streamingClientsSemaphore = nullptr;  // mutex to control access to the streaming clients count / buffer
StaticSemaphore_t streamingClientsSemaphoreBuf;


const char kMjpegHeader[] = "HTTP/1.1 200 OK\r\n" \
                      "Access-Control-Allow-Origin: *\r\n" \
                      "Content-Type: multipart/x-mixed-replace; boundary=123456789000000000000987654321\r\n";
const char kMjpegBoundary[] = "\r\n--123456789000000000000987654321\r\n";  // arbitrarily-chosen delimiter
const char kMjpegContentType[] = "Content-Type: image/jpeg\r\nContent-Length: ";  // written per frame
const int kMjpegHeaderLen = strlen(kMjpegHeader);
const int kMjpegBoundaryLen = strlen(kMjpegBoundary);
const int kMjpegContentTypeLen = strlen(kMjpegContentType);

// For each connected streaming client, send new frames as they become available
void Task_MjpegStream(void *pvParameters) {
  while (true) {
    while (xTaskNotifyWait(0, 0, nullptr, portMAX_DELAY) == pdFALSE);

    if (numStreamingClients <= 0) {  // quick test
      continue;
    }

    // encode frame
    uint8_t jpegBuf[kJpegBufferSize];
    size_t jpegSize;
    while (xSemaphoreTake(bufferControlSemaphore, portMAX_DELAY) != pdTRUE);
    uint8_t bufferReadIndex = (bufferWriteIndex + 1) % 2;
    bufferReaders++;
    assert(xSemaphoreGive(bufferControlSemaphore) == pdTRUE);

    int encodeStatus = encodeJpeg(vospiBuf[bufferReadIndex], lepton.getFrameWidth(), lepton.getFrameHeight(),
        jpegencPixelType, jpegBuf, sizeof(jpegBuf), &jpegSize);

    bufferReaders--;

    // deallocate disconnected clients
    while (xSemaphoreTake(streamingClientsSemaphore, portMAX_DELAY) != pdTRUE);
    size_t writeClientIndex = 0;
    for (size_t readClientIndex=0; readClientIndex<numStreamingClients; readClientIndex++) {
      // TODO faster to swap with last element
      if (streamingClients[readClientIndex].connected()) {
        if (readClientIndex != writeClientIndex) {
          streamingClients[writeClientIndex] = streamingClients[readClientIndex];
        }
        writeClientIndex++;
      } else {
        ESP_LOGI("main", "MJPEG disconnected %i", readClientIndex);
      }
    }
    uint8_t currStreamingClients = numStreamingClients = writeClientIndex;
    assert(xSemaphoreGive(streamingClientsSemaphore) == pdTRUE);

    if (encodeStatus == JPEGE_SUCCESS) {
      ESP_LOGI("main", "MJPEG stream %i B", jpegSize);
      char buf[32];
      sprintf(buf, "%d\r\n\r\n", jpegSize);
      size_t bufLen = strlen(buf);

      for (size_t i=0; i<currStreamingClients; i++) {
        streamingClients[i].write(kMjpegContentType, kMjpegContentTypeLen);
        streamingClients[i].write(buf, bufLen);
        streamingClients[i].write(jpegBuf, jpegSize);
        streamingClients[i].write(kMjpegBoundary, kMjpegBoundaryLen);
      }
    }
  }
}

// Starts the MJPEG stream, including sending the current frame or a max-clients error
void handle_mjpeg_stream(void) {
  WiFiClient* client;
  size_t thisStreamingClient;  // valid if client != nullptr
  while (xSemaphoreTake(streamingClientsSemaphore, portMAX_DELAY) != pdTRUE);
  if (numStreamingClients >= kMaxStreamingClients) {
    client = nullptr;
  } else {
    streamingClients[numStreamingClients] = server.client();
    client = &(streamingClients[numStreamingClients]);
    thisStreamingClient = numStreamingClients++;
  }
  assert(xSemaphoreGive(streamingClientsSemaphore) == pdTRUE);

  if (client == nullptr) {
    server.send(200, "text / plain", "Max streaming clients");
    return;
  }

  client->write(kMjpegHeader, kMjpegHeaderLen);
  client->write(kMjpegBoundary, kMjpegBoundaryLen);

  // encode and send the first frame, if valid
  uint8_t jpegBuf[kJpegBufferSize];
  size_t jpegSize;

  while (xSemaphoreTake(bufferControlSemaphore, portMAX_DELAY) != pdTRUE);
  uint8_t bufferReadIndex = (bufferWriteIndex + 1) % 2;
  bufferReaders++;
  assert(xSemaphoreGive(bufferControlSemaphore) == pdTRUE);

  int encodeStatus = encodeJpeg(vospiBuf[bufferReadIndex], lepton.getFrameWidth(), lepton.getFrameHeight(),
      jpegencPixelType, jpegBuf, sizeof(jpegBuf), &jpegSize);

  bufferReaders--;

  if (encodeStatus == JPEGE_SUCCESS) {
    ESP_LOGI("main", "MJPEG started %i %i B", thisStreamingClient, jpegSize);
    char buf[32];
    client->write(kMjpegContentType, kMjpegContentTypeLen);
    sprintf(buf, "%d\r\n\r\n", jpegSize);
    client->write(buf, strlen(buf));
    client->write(jpegBuf, jpegSize);
    client->write(kMjpegBoundary, kMjpegBoundaryLen);
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

  int encodeStatus = encodeJpeg(vospiBuf[bufferReadIndex], lepton.getFrameWidth(), lepton.getFrameHeight(),
      jpegencPixelType, jpegBuf, sizeof(jpegBuf), &jpegSize);

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


void Task_Stream(void *pvParameters) {
  while (true) {
    server.handleClient();
    vTaskDelay(1);
  }
}


void Task_Lepton(void *pvParameters) {
  ESP_LOGI("main", "Start init");
  pinMode(kPinLepVsync, INPUT);
  bool beginResult = lepton.begin();
  ESP_LOGI("main", "Lepton init << %i", beginResult);

  while (!lepton.isReady()) {
    vTaskDelay(1);
  }

  bool result = lepton.enableVsync();
  ESP_LOGI("main", "Lepton Vsync << %i", result);

  result = lepton.setVideoMode(FlirLepton::kAgcHeq);
  ESP_LOGI("main", "Lepton VideoMode << %i", result);

  delay(185);  // resync after changing mode - required or no video data sent

  while (true) {
    bool readResult = lepton.readVoSpi(sizeof(vospiBuf[0]), vospiBuf[bufferWriteIndex]);

    if (readResult) {
      digitalWrite(kPinLedR, !digitalRead(kPinLedR));

      size_t width = lepton.getFrameWidth(), height = lepton.getFrameHeight();

      uint16_t min, max;
      u16_frame_min_max(vospiBuf[bufferWriteIndex], width, height, &min, &max);
      uint16_t range = max - min;
      if (range == 0) {  // avoid division by zero
        ESP_LOGW("main", "empty thermal image");
        range = 1;
      }

      // reformat to 8-bit
      for (uint16_t y=0; y<height; y++) {
        for (uint16_t x=0; x<width; x++) {
          vospiBuf[bufferWriteIndex][y*width+x] = (((uint16_t)vospiBuf[bufferWriteIndex][2*(y*width+x)] << 8) | vospiBuf[bufferWriteIndex][2*(y*width+x) + 1]) >> 0;
        }
      }

      while (xSemaphoreTake(bufferControlSemaphore, portMAX_DELAY) != pdTRUE);
      if (bufferReaders == 0) {
        bufferWriteIndex = (bufferWriteIndex + 1) % 2;
      } else {
        ESP_LOGW("main", "skipped frame");
      }
      assert(xSemaphoreGive(bufferControlSemaphore) == pdTRUE);

      if (streamingTask != nullptr) {
        xTaskNotify(streamingTask, 0, eNoAction);
      }
    }

    vTaskDelay(1);
  }
}


void setup() {
  Serial.begin(115200);

  // wait for post-flash reset
  pinMode(kPinLedR, OUTPUT);
  digitalWrite(kPinLedR, LOW);
  delay(2000);
  digitalWrite(kPinLedR, HIGH);

  spi.begin(kPinLepSck, kPinLepMiso, -1, -1);
  i2c.begin(kPinI2cSda, kPinI2cScl, 400000);

  // initialize shared data structures
  bufferControlSemaphore = xSemaphoreCreateMutexStatic(&bufferControlSemaphoreBuf);
  assert(bufferControlSemaphore != nullptr);
  streamingClientsSemaphore = xSemaphoreCreateMutexStatic(&streamingClientsSemaphoreBuf);
  assert(streamingClientsSemaphore != nullptr);

  // webserver is relatively low priority
  xTaskCreatePinnedToCore(Task_Lepton, "Task_Lepton", 4096, NULL, 4, NULL, ARDUINO_RUNNING_CORE);
  xTaskCreatePinnedToCore(Task_MjpegStream, "Task_MjpegStream", kJpegBufferSize + 4096, NULL, 1, &streamingTask, ARDUINO_RUNNING_CORE);
  xTaskCreatePinnedToCore(Task_Server, "Task_Server", kJpegBufferSize + 4096, NULL, 1, NULL, ARDUINO_RUNNING_CORE);
}

void loop() {
  vTaskDelay(1000);
}
