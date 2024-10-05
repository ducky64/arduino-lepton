#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE

#include "lepton.h"

// Override these to use some other logging framework
static const char* TAG = "lepton";
#define LEP_LOGD(...) ESP_LOGD(TAG, __VA_ARGS__)
#define LEP_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#define LEP_LOGW(...) ESP_LOGW(TAG, __VA_ARGS__)
#define LEP_LOGE(...) ESP_LOGE(TAG, __VA_ARGS__)


bool FlirLepton::begin() {
    digitalWrite(csPin_, HIGH);
    pinMode(csPin_, OUTPUT);
    digitalWrite(csPin_, HIGH);

    pinMode(resetPin_, OUTPUT);
    digitalWrite(resetPin_, LOW);  // assert RESET
    delay(1);  // wait >5000 clk periods @ 25MHz MCLK, Lepton Eng Datasheet Figure 6
    digitalWrite(resetPin_, HIGH);

    delay(950); // minimum wait before accessing I2C, Lepton Software IDD

    // TODO timeout
    while (true) {  // wait for BOOT status bit and not busy
      uint16_t statusData;
      if (!readReg16(kRegStatus, &statusData)) {
        LEP_LOGE("begin() read status failed");
        return false;
      }
      LEP_LOGD("status <- 0x%04x", statusData);

      if (statusData & (1 << 2) && !(statusData & 1)) {
        if (!(statusData & (1 << 1))) {
          LEP_LOGE("begin() unexpected boot mode bit");
          return false;
        }
        break;
      } else {  // continue waiting for boot
        delay(100);
      }
    }

    // TODO wait for SYS FFC

    return true;
  }


  void FlirLepton::end() {
    pinMode(csPin_, INPUT);
    pinMode(resetPin_, INPUT);
  }


  bool FlirLepton::commandGet(ModuleId moduleId, uint8_t commandId, uint16_t len, uint8_t *dataOut) {
    uint16_t commandId = ((moduleId & 0xf) << 8) | ((commandId & 0x3f) << 2) | (kGet & 0x3);
  }


  bool FlirLepton::commandSet(ModuleId moduleId, uint8_t commandId, uint16_t len, uint8_t *data) {
    // TBD
  }


  bool FlirLepton::writeReg16(uint16_t addr, uint16_t data) {
    wire_->beginTransmission(kI2cAddr);
    wire_->write(addr >> 8);
    wire_->write(addr & 0xff);
    wire_->write(data >> 8);
    wire_->write(data & 0xff);
    uint8_t wireStatus = wire_->endTransmission(false);
    if (wireStatus) {
      LEP_LOGE("writeReg16(0x%04x) write failed with %i", addr, wireStatus);
      return false;
    }
    return true; 
  }

  bool FlirLepton::readReg16(uint16_t addr, uint16_t* dataOut) {
    wire_->beginTransmission(kI2cAddr);
    wire_->write(addr >> 8);
    wire_->write(addr & 0xff);
    uint8_t wireStatus = wire_->endTransmission(false);
    if (wireStatus) {
      LEP_LOGE("readReg16(0x%04x) write failed with %i", addr, wireStatus);
      return false;
    }

    uint8_t reqCount = wire_->requestFrom(kI2cAddr, (size_t)2);
    if (reqCount != 2) {
      LEP_LOGE("readReg16(0x%04x) read failed reqCount %i", addr, reqCount);
    }
    uint8_t bytes[2];
    for (uint8_t i=0; i<2; i++) {
      bytes[i] = wire_->read();
    }

    *dataOut = ((uint16_t)bytes[0] << 8) | (uint16_t)bytes[1];
    return true; 
  }
