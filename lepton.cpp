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


  bool FlirLepton::commandGet(FlirLepton::ModuleId moduleId, uint8_t moduleCommandId, uint16_t len, uint8_t *dataOut) {
    writeReg16(kRegDataLen, len / 2);
    uint16_t commandId = ((moduleId & 0xf) << 8) | ((commandId & 0x3f) << 2) | (kGet & 0x3);
    writeReg16(kRegCommandId, commandId);

    int8_t result = readNonBusyStatus();
    if (result != kLepOk) {
      LEP_LOGE("commandGet(%i, %i) returned %i != LEP_OK", moduleId, commandId, result);
    }

    // TBD
    return false;
  }


  bool FlirLepton::commandSet(FlirLepton::ModuleId moduleId, uint8_t moduleCommandId, uint16_t len, uint8_t *data) {
    // TBD
    return false;
  }

  bool commandRun(FlirLepton::ModuleId moduleId, uint8_t moduleCommandId) {
    // TBD
    return false;
  }


  int8_t FlirLepton::readNonBusyStatus() {
    while (true) {  // wait for status bit not busy
      uint16_t statusData;
      if (!readReg16(kRegStatus, &statusData)) {
        LEP_LOGE("readNonBusyStatus() read status failed");
        return -127;  // note, overlaps with LEP_UNDEFINED_ERROR_CODE
      }
      LEP_LOGD("status <- 0x%04x", statusData);

      if (!(statusData & 1)) {
        return (int8_t)(statusData >> 8);          
      } else {
        delay(1);
      }
    }
  }


  bool FlirLepton::writeReg16(uint16_t addr, uint16_t data) {
    uint8_t buffer[2] = {(uint8_t)(data >> 8), (uint8_t)(data & 0xff)};
    return writeReg(addr, 2, buffer);
  }

  bool FlirLepton::writeReg(uint16_t addr, size_t len, uint8_t* data) {
    wire_->beginTransmission(kI2cAddr);
    wire_->write(addr >> 8);
    wire_->write(addr & 0xff);
    for (size_t i=0; i<len; i++) {
      wire_->write(data[i]);
    }
    uint8_t wireStatus = wire_->endTransmission(false);
    if (wireStatus) {
      LEP_LOGE("writeReg(0x%04x, %i) write failed with %i", addr, len, wireStatus);
      return false;
    }
    return true; 
  }

  bool FlirLepton::readReg16(uint16_t addr, uint16_t* dataOut) {
    uint8_t buffer[2];
    bool status = readReg(addr, 2, buffer);
    *dataOut = ((uint16_t)buffer[0] << 8) | (uint16_t)buffer[1];
    return status; 
  }

  bool FlirLepton::readReg(uint16_t addr, size_t len, uint8_t* dataOut) {
    wire_->beginTransmission(kI2cAddr);
    wire_->write(addr >> 8);
    wire_->write(addr & 0xff);
    uint8_t wireStatus = wire_->endTransmission(false);
    if (wireStatus) {
      LEP_LOGE("readReg(0x%04x, %i) write failed with %i", addr, len, wireStatus);
      return false;
    }

    uint8_t reqCount = wire_->requestFrom(kI2cAddr, (size_t)2);
    if (reqCount != len) {
      LEP_LOGE("readReg(0x%04x, %i) read failed reqCount %i", addr, len, reqCount);
    }
    for (uint8_t i=0; i<len; i++) {
      dataOut[i] = wire_->read();
    }
    return true; 
  }
