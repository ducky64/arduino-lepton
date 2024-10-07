#include <Arduino.h>
#include "lepton.h"
#include "lepton_util.h"

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
uint8_t vospiBuf[160*120*3];  // up to RGB888

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

  ESP_LOGI("main", "Lepton wait ready ...");
  while (!lepton.isReady()) {
    delay(10);
  }

  bool result = lepton.enableVsync();
  ESP_LOGI("main", "Lepton Vsync << %i", result);

  ESP_LOGI("main", "Setup complete");

  while (digitalRead(kPinLepVsync) == LOW);

  while (true) {
    bool readError = false;
    bool readResult = lepton.readVoSpi(sizeof(vospiBuf), vospiBuf, &readError);

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
      uint16_t min, max;
      u16_frame_min_max(vospiBuf, 160, 120, &min, &max);

      char line[161];
      for (uint16_t i=0; i<120; i++) {
        u16_frame_line_to_str(vospiBuf, 160, i, line, min, max);
        Serial.println(line);
      }
      Serial.println("");
    }

    if (millis() > 20000) {
      break;
    }
  }

  lepton.end();
  spi.end();
  i2c.end();
  digitalWrite(kPinLepPwrdn, LOW);
  ESP_LOGI("main", "Lepton powerdown");
}

void loop() {
  delay(100);
  digitalWrite(kPinLedR, !digitalRead(kPinLedR));
}
