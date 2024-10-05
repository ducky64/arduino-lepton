#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE

#include "lepton.h"

// Override these to use some other logging framework
static const char* TAG = "lepton";
#define LEP_LOGD(...) ESP_LOGD(TAG, __VA_ARGS__)
#define LEP_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#define LEP_LOGW(...) ESP_LOGW(TAG, __VA_ARGS__)
#define LEP_LOGE(...) ESP_LOGE(TAG, __VA_ARGS__)


// utility conversions
inline uint32_t bufferToU32(uint8_t* buffer) {
  return ((uint32_t)buffer[0] << 24) | ((uint32_t)buffer[1] << 16) |
      ((uint32_t)buffer[2] << 8) | ((uint32_t)buffer[0]);
}

inline uint32_t bufferToU16(uint8_t* buffer) {
  return ((uint32_t)buffer[0] << 8) | ((uint32_t)buffer[1]);
}

inline int32_t bufferToI32(uint8_t* buffer) {
  return (int32_t)bufferToU32(buffer);
}


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
      LEP_LOGD("begin() status <- 0x%04x", statusData);

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
    while (true) {
      uint8_t resultBuffer[4];
      LEP_LOGI("cmdGet)");
      if (!commandGet(kSys, 0x44, 4, resultBuffer)) {
        LEP_LOGE("begin() SYS FFC status commandGet failed");
      }
      int32_t ffcStatus = bufferToI32(resultBuffer);
      LEP_LOGI("begin() SYS FFC <- %i", ffcStatus);
      if (ffcStatus == 0) {
        break;
      } else if (ffcStatus < 0) {
        LEP_LOGE("begin() SYS FFC returned error %i", ffcStatus);
      } else {
        delay(1);  // continue waiting
      }
    }

    return true;
  }


  void FlirLepton::end() {
    pinMode(csPin_, INPUT);
    pinMode(resetPin_, INPUT);
  }


  bool FlirLepton::commandGet(FlirLepton::ModuleId moduleId, uint8_t moduleCommandId, uint16_t len, uint8_t *dataOut) {
    writeReg16(kRegDataLen, len / 2);
    uint16_t commandId = ((moduleId & 0xf) << 8) | ((moduleCommandId & 0x3f) << 2) | (kGet & 0x3);
    writeReg16(kRegCommandId, commandId);

    if (int8_t result = readNonBusyStatus() != kLepOk) {
      LEP_LOGE("commandGet(%i, %i) returned %i != LEP_OK", moduleId, commandId, result);
      return false;
    }

    if (!readReg(kRegData0, len, dataOut)) {
      LEP_LOGE("commandGet(%i, %i) readReg failed", moduleId, commandId);
      return false;
    }

    return true;
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


  inline bool FlirLepton::writeReg16(uint16_t addr, uint16_t data) {
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
    uint8_t wireStatus = wire_->endTransmission();
    if (wireStatus) {
      LEP_LOGE("writeReg(0x%04x, %i) write failed with %i", addr, len, wireStatus);
      return false;
    }
    return true; 
  }

  inline bool FlirLepton::readReg16(uint16_t addr, uint16_t* dataOut) {
    uint8_t buffer[2];
    bool status = readReg(addr, 2, buffer);
    *dataOut = bufferToU16(buffer);
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

    uint8_t reqCount = wire_->requestFrom(kI2cAddr, len);
    if (reqCount != len) {
      LEP_LOGE("readReg(0x%04x, %i) read failed reqCount %i", addr, len, reqCount);
    }
    for (uint8_t i=0; i<len; i++) {
      dataOut[i] = wire_->read();
    }
    return true; 
  }
