#include <Arduino.h>
#include "lepton.h"

// web server code based on (BSD)
// https://github.com/arkhipenko/esp32-cam-mjpeg/blob/master/esp32_camera_mjpeg.ino
// and RTOS multiclient version (BSD)
// https://github.com/arkhipenko/esp32-cam-mjpeg-multiclient/blob/master/esp32_camera_mjpeg_multiclient.ino
#include <WiFi.h>
#include <WebServer.h>
#include <atomic>
#include <JPEGENC.h>


#if __has_include("WifiConfig.h")
  #include "WifiConfig.h"  // optionally define ssid and password in this .gitignore'd file
#else
  const char* ssid = "YOUR_SSID_HERE";
  const char* password = "";
#endif

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
uint8_t jpegencPixelBytes = 2;
uint8_t vospiBuf[2][160*120*3] = {0};  // up to RGB888, double-buffered
// controlled by the writing (sensor) task
uint8_t bufferWriteIndex = 0;  // buffer being written to, the other one is implicitly the read buffer; 0 means buffer not being read
uint8_t frameCounter = 0;  // incrementing, reflecting the frame count in the read buffer
std::atomic<uint8_t> bufferReaders{0};  // number of readers of the non-writing buffer, locks bufferWriteIndex if >0
SemaphoreHandle_t bufferControlSemaphore = nullptr;  // mutex to control access to the write index / readers count
StaticSemaphore_t bufferControlSemaphoreBuf;


JPEGENC jpgenc;
const size_t kJpegBufferSize = 32768;

// converts frame into a jpeg, stored in jpegBuf, writing the output length to jpegLenOut
int encodeJpeg(uint8_t* frame, size_t frameWidth, size_t frameHeight, uint8_t ucPixelType, uint8_t* jpegBuf, size_t jpegBufLen, size_t* jpegLenOut) {
  JPEGENCODE enc;
  int rc;

  size_t lineLength;
  if (jpegencPixelBytes == 2) {  // use end of the buffer for 16b->8b conversion
    assert(jpegBufLen > frameHeight * frameWidth);
    uint8_t* frame8b = jpegBuf + jpegBufLen - frameHeight * frameWidth;
    for (uint16_t y=0; y<frameHeight; y++) {
      for (uint16_t x=0; x<frameWidth; x++) {  // for AGC mode
        frame8b[y*frameWidth+x] = (((uint16_t)frame[2*(y*frameWidth+x)] << 8) | frame[2*(y*frameWidth+x) + 1]) >> 0;
      }
    }
    frame = frame8b;
    jpegBufLen -= frameHeight * frameWidth;
    lineLength = frameWidth;
  } else {
    lineLength = frameWidth * jpegencPixelBytes;
  }

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
    rc = jpgenc.addFrame(&enc, frame, lineLength);
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

uint8_t streamingJpegBuffer[kJpegBufferSize];
uint8_t webserverJpegBuffer[kJpegBufferSize];

const char kMjpegHeader[] = "HTTP/1.1 200 OK\r\n" \
                      "Access-Control-Allow-Origin: *\r\n" \
                      "Content-Type: multipart/x-mixed-replace; boundary=FRAME\r\n";
const char kMjpegBoundary[] = "\r\n--FRAME\r\n";  // arbitrarily-chosen delimiter
const char kMjpegContentType[] = "Content-Type: image/jpeg\r\nContent-Length: ";  // written per frame
const int kMjpegHeaderLen = strlen(kMjpegHeader);
const int kMjpegBoundaryLen = strlen(kMjpegBoundary);
const int kMjpegContentTypeLen = strlen(kMjpegContentType);

// For each connected streaming client, send new frames as they become available
void Task_MjpegStream(void *pvParameters) {
  uint8_t lastFrame = frameCounter - 1;
  while (true) {
    if (numStreamingClients <= 0 || frameCounter == lastFrame) {  // quick test
      xTaskNotifyWait(0, 0, nullptr, portMAX_DELAY);
      continue;
    }

    // encode frame
    size_t jpegSize;
    while (xSemaphoreTake(bufferControlSemaphore, portMAX_DELAY) != pdTRUE);
    uint8_t bufferReadIndex = (bufferWriteIndex + 1) % 2;
    bufferReaders++;
    assert(xSemaphoreGive(bufferControlSemaphore) == pdTRUE);

    int encodeStatus = encodeJpeg(vospiBuf[bufferReadIndex], lepton.getFrameWidth(), lepton.getFrameHeight(),
        jpegencPixelType, streamingJpegBuffer, sizeof(streamingJpegBuffer), &jpegSize);
    lastFrame = frameCounter;

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
        streamingClients[i].write(streamingJpegBuffer, jpegSize);
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
  size_t jpegSize;

  while (xSemaphoreTake(bufferControlSemaphore, portMAX_DELAY) != pdTRUE);
  uint8_t bufferReadIndex = (bufferWriteIndex + 1) % 2;
  bufferReaders++;
  assert(xSemaphoreGive(bufferControlSemaphore) == pdTRUE);

  int encodeStatus = encodeJpeg(vospiBuf[bufferReadIndex], lepton.getFrameWidth(), lepton.getFrameHeight(),
      jpegencPixelType, webserverJpegBuffer, sizeof(webserverJpegBuffer), &jpegSize);

  bufferReaders--;

  if (encodeStatus == JPEGE_SUCCESS) {
    ESP_LOGI("main", "MJPEG started %i %i B", thisStreamingClient, jpegSize);
    char buf[32];
    client->write(kMjpegContentType, kMjpegContentTypeLen);
    sprintf(buf, "%d\r\n\r\n", jpegSize);
    client->write(buf, strlen(buf));
    client->write(webserverJpegBuffer, jpegSize);
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

  size_t jpegSize;

  while (xSemaphoreTake(bufferControlSemaphore, portMAX_DELAY) != pdTRUE);
  uint8_t bufferReadIndex = (bufferWriteIndex + 1) % 2;
  bufferReaders++;
  assert(xSemaphoreGive(bufferControlSemaphore) == pdTRUE);

  int encodeStatus = encodeJpeg(vospiBuf[bufferReadIndex], lepton.getFrameWidth(), lepton.getFrameHeight(),
      jpegencPixelType, webserverJpegBuffer, sizeof(webserverJpegBuffer), &jpegSize);

  bufferReaders--;

  if (encodeStatus == JPEGE_SUCCESS) {
    ESP_LOGI("main", "JPG created %i B", jpegSize);
    client.write(kJpgHeader, kJpgHeaderLen);
    client.write(webserverJpegBuffer, jpegSize);
  } else {
    server.send(200, "text / plain", "Error");
  }
}


void handleNotFound() {
  server.send(200, "text / plain", "Unknown request");
}


void Task_Server(void *pvParameters) {
  ESP_LOGI("main", "WiFi init");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(1);
  }
  ESP_LOGI("main", "WiFi connected %s", WiFi.localIP().toString());

  server.on("/mjpeg", HTTP_GET, handle_mjpeg_stream);
  server.on("/jpg", HTTP_GET, handle_jpg);
  server.onNotFound(handleNotFound);
  server.begin();
  ESP_LOGI("main", "WiFi server started");

  while (true) {
    server.handleClient();
    vTaskDelay(1);
  }
}


void Task_Lepton(void *pvParameters) {
  ESP_LOGI("main", "Lepton init");
  pinMode(kPinLepVsync, INPUT);
  assert(lepton.begin());

  while (!lepton.isReady()) {
    vTaskDelay(1);
  }
  ESP_LOGI("main", "Lepton ready");

  ESP_LOGI("main", "Lepton Serial = %llu", lepton.getFlirSerial());
  ESP_LOGI("main", "Lepton Part Number = %s", lepton.getFlirPartNum());
  ESP_LOGI("main", "Lepton Software Version = 0x %02x %02x %02x %02x %02x %02x", 
      lepton.getFlirSoftwareVerison()[0], lepton.getFlirSoftwareVerison()[1], lepton.getFlirSoftwareVerison()[2],
      lepton.getFlirSoftwareVerison()[3], lepton.getFlirSoftwareVerison()[4], lepton.getFlirSoftwareVerison()[5]);

  assert(lepton.enableVsync());

  // optionally comment this and/or the next block out to not use AGC or colorization
  // note, the JPEG encoding only uses the lowest 8 bits (assumes AGC on)
  assert(lepton.setVideoMode(FlirLepton::kAgcHeq));

  assert(lepton.setVideoFormat(FlirLepton::kRgb888));
  jpegencPixelType = JPEGE_PIXEL_RGB888;
  jpegencPixelBytes = 3;

  bool bufferFlipRequested = false;  // allow queueing a buffer flip until data is overwritten
  while (true) {
    bool bufferOverwritten = false;
    bool readResult = lepton.readVoSpi(sizeof(vospiBuf[0]), vospiBuf[bufferWriteIndex], &bufferOverwritten);

    if (bufferOverwritten) {
      bufferFlipRequested = false;
    }

    if (readResult) {
      digitalWrite(kPinLedR, !digitalRead(kPinLedR));

      bufferFlipRequested = true;
    }

    if (bufferFlipRequested) {
      bool bufferFlipSuccess = false;
      while (xSemaphoreTake(bufferControlSemaphore, portMAX_DELAY) != pdTRUE);
      if (bufferReaders == 0) {
        bufferWriteIndex = (bufferWriteIndex + 1) % 2;
        frameCounter++;
        bufferFlipSuccess = true;
      }
      assert(xSemaphoreGive(bufferControlSemaphore) == pdTRUE);

      if (bufferFlipSuccess) {
        if (streamingTask != nullptr) {
          xTaskNotify(streamingTask, 0, eNoAction);
        }
        bufferFlipRequested = false;
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

  // Lepton interface is timing-sensitive and needs to be high priority
  xTaskCreatePinnedToCore(Task_Lepton, "Task_Lepton", 4096, NULL, 16, NULL, ARDUINO_RUNNING_CORE);
  xTaskCreatePinnedToCore(Task_MjpegStream, "Task_MjpegStream", 4096, NULL, 1, &streamingTask, ARDUINO_RUNNING_CORE);
  xTaskCreatePinnedToCore(Task_Server, "Task_Server", 4096, NULL, 1, NULL, ARDUINO_RUNNING_CORE);
}

void loop() {
  vTaskDelay(1000);
}
