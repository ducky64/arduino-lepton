#include <Arduino.h>
#include "lepton.h"


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
uint8_t vospiBuf[160*120*3] = {0};  // up to RGB888, double-buffered


void setup() {
  Serial.begin(115200);

  // wait for post-flash reset
  pinMode(kPinLedR, OUTPUT);
  digitalWrite(kPinLedR, LOW);
  delay(2000);
  digitalWrite(kPinLedR, HIGH);

  spi.begin(kPinLepSck, kPinLepMiso, -1, -1);
  i2c.begin(kPinI2cSda, kPinI2cScl, 400000);

  Serial.println("Lepton start");
  pinMode(kPinLepVsync, INPUT);
  assert(lepton.begin());

  while (!lepton.isReady()) {
    delay(1);
  }
  Serial.println("Lepton ready");

  Serial.print("Lepton Serial = ");
  Serial.print(lepton.getFlirSerial());
  Serial.println("");

  Serial.print("Lepton Part Number = ");
  Serial.print(lepton.getFlirPartNum());
  Serial.println("");

  assert(lepton.enableVsync());
  
  while (!digitalRead(kPinLepVsync));  // seems necessary

  char line[lepton.getFrameWidth()] = {0};
  while (true) {
    bool readResult = lepton.readVoSpi(sizeof(vospiBuf), vospiBuf);
    if (readResult) {
      digitalWrite(kPinLedR, !digitalRead(kPinLedR));
      Serial.println("Got frame");

      // run basic linear AGC
      size_t width = lepton.getFrameWidth(), height = lepton.getFrameHeight();
      uint16_t min = 65535, max = 0;
      for (size_t y=0; y<height; y++) {
        for (size_t x=0; x<width; x++) {
          uint16_t pixel = ((uint16_t)vospiBuf[2*(y*width+x)] << 8) | vospiBuf[2*(y*width+x) + 1];
          if (pixel < min) {
            min = pixel;
          }
          if (pixel > max) {
            max = pixel;
          } 
        }
      }

      Serial.print("Min = ");
      Serial.print(min);
      Serial.print(", max = ");
      Serial.print(max);
      Serial.println("");

      uint16_t range = max - min;
      for (size_t y=0; y<height; y++) {  // print each pixel as between 0-9
        for (size_t x=0; x<width; x++) {
          uint16_t pixel = ((uint16_t)vospiBuf[2*(y*width+x)] << 8) | vospiBuf[2*(y*width+x) + 1];
          line[x] = ((uint32_t)(pixel - min) * 10) / (range + 1) + '0';
        }
        Serial.println(line);
      } 
    }
  }
}

void loop() {
}
