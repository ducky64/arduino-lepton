#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE

#include "lepton.h"

// Override these to use some other logging framework
static const char* TAG = "lepton";
#define LEP_LOGD(...) ESP_LOGD(TAG, __VA_ARGS__)
#define LEP_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#define LEP_LOGW(...) ESP_LOGW(TAG, __VA_ARGS__)
#define LEP_LOGE(...) ESP_LOGE(TAG, __VA_ARGS__)


// utility conversions
// note, bits in a 16b word in big-endian order, words in little-endian order
inline uint64_t bufferToU64(uint8_t* buffer) {
  return ((uint64_t)buffer[0] << 8) | ((uint64_t)buffer[1] << 0) |
      ((uint64_t)buffer[2] << 24) | ((uint64_t)buffer[3] << 16) |
      ((uint64_t)buffer[4] << 40) | ((uint64_t)buffer[5] << 32) |
      ((uint64_t)buffer[6] << 56) | ((uint64_t)buffer[7] << 48);
}

inline uint32_t bufferToU32(uint8_t* buffer) {
  return ((uint32_t)buffer[0] << 8) | ((uint32_t)buffer[1] << 0) |
      ((uint32_t)buffer[2] << 24) | ((uint32_t)buffer[3] << 16);
}

inline uint32_t bufferToU16(uint8_t* buffer) {
  return ((uint32_t)buffer[0] << 8) | ((uint32_t)buffer[1]);
}

inline int32_t bufferToI32(uint8_t* buffer) {
  return (int32_t)bufferToU32(buffer);
}

inline void U32ToBuffer(uint32_t data, uint8_t* bufferOut) {
  bufferOut[0] = (data >> 8) & 0xff;
  bufferOut[1] = (data >> 0) & 0xff;
  bufferOut[2] = (data >> 24) & 0xff;
  bufferOut[3] = (data >> 16) & 0xff;
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

    uint8_t cmdBuffer[8];
    Result result = commandGet(kSys, 0x08 >> 2, 8, cmdBuffer);
    if (result != kLepOk) {
      LEP_LOGE("begin() SYS FLIR Serial status commandGet failed %i", result);
      return false;
    }
    uint64_t flirSerial = bufferToU64(cmdBuffer);
    LEP_LOGI("begin() read FLIR serial = %llu, 0x%016llx", flirSerial, flirSerial);
    if (flirSerial == 0) {  // a sanity check on comms correctness
      LEP_LOGW("begin() failed sanity check: zero FLIR serial");
    }

    while (true) {
      result = commandGet(kSys, 0x44 >> 2, 4, cmdBuffer);
      if (result != kLepOk) {
        LEP_LOGE("begin() SYS FFC status commandGet failed %i", result);
        return false;
      }
      int32_t ffcStatus = bufferToI32(cmdBuffer);
      LEP_LOGD("begin() SYS FFC <- %i", ffcStatus);
      if (ffcStatus == 0) {
        break;
      } else if (ffcStatus < 0) {
        LEP_LOGE("begin() SYS FFC returned error %i", ffcStatus);
        return false;
      } else {
        delay(1);  // continue waiting
      }
    }

    while (true) {
      result = commandGet(kSys, 0x04 >> 2, 4, cmdBuffer);
      if (result != kLepOk) {
        LEP_LOGE("begin() SYS status commandGet failed %i", result);
        return false;
      }
      int32_t sysStatus = bufferToI32(cmdBuffer);
      LEP_LOGI("begin() SYS status <- %i", sysStatus);
      if (sysStatus == 0) {
        break;
        return false;
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


  FlirLepton::Result FlirLepton::enableVsync() {
    uint8_t buffer[4];
    U32ToBuffer(5, buffer);
    return commandSet(kOem, 0x54 >> 2, 4, buffer, true);
  }


  FlirLepton::Result FlirLepton::commandGet(FlirLepton::ModuleId moduleId, uint8_t moduleCommandId, uint16_t len, uint8_t *dataOut, bool oemBit) {
    if (!writeReg16(kRegDataLen, len / 2)) {
      LEP_LOGE("commandGet(%i, %i) write data len failed", moduleId, moduleCommandId);
      return kUndefinedError;
    }
    uint16_t commandId = (oemBit ? 0x4000 : 0) | ((moduleId & 0xf) << 8) | ((moduleCommandId & 0x3f) << 2) | (kGet & 0x3);
    if (!writeReg16(kRegCommandId, commandId)) {
      LEP_LOGE("commandGet(%i, %i) write command id failed", moduleId, moduleCommandId);
      return kUndefinedError;
    }

    Result result = readNonBusyStatus();

    if (!readReg(kRegData0, len, dataOut)) {
      LEP_LOGE("commandGet(%i, %i) readReg failed", moduleId, moduleCommandId);
      return kUndefinedError;
    }

    return result;
  }


  FlirLepton::Result FlirLepton::commandSet(FlirLepton::ModuleId moduleId, uint8_t moduleCommandId, uint16_t len, uint8_t *data, bool oemBit) {
    if (!writeReg16(kRegDataLen, len / 2)) {
      LEP_LOGE("commandSet(%i, %i) write data len failed", moduleId, moduleCommandId);
      return kUndefinedError;
    }
    if (!writeReg(kRegData0, len, data)) {
      LEP_LOGE("commandSet(%i, %i) write data failed", moduleId, moduleCommandId);
      return kUndefinedError;
    }
    uint16_t commandId = (oemBit ? 0x4000 : 0) | ((moduleId & 0xf) << 8) | ((moduleCommandId & 0x3f) << 2) | (kSet & 0x3);
    if (!writeReg16(kRegCommandId, commandId)) {
      LEP_LOGE("commandSet(%i, %i) write command id failed", moduleId, moduleCommandId);
      return kUndefinedError;
    }

    return readNonBusyStatus();
  }

  FlirLepton::Result FlirLepton::commandRun(FlirLepton::ModuleId moduleId, uint8_t moduleCommandId) {
    uint16_t commandId = ((moduleId & 0xf) << 8) | ((moduleCommandId & 0x3f) << 2) | (kRun & 0x3);
    if (!writeReg16(kRegCommandId, commandId)) {
      LEP_LOGE("commandRun(%i, %i) write command id failed", moduleId, moduleCommandId);
      return kUndefinedError;
    }
    return readNonBusyStatus();
  }


  FlirLepton::Result FlirLepton::readNonBusyStatus() {
    while (true) {  // wait for status bit not busy
      uint16_t statusData;
      if (!readReg16(kRegStatus, &statusData)) {
        LEP_LOGE("readNonBusyStatus() read status failed");
        return kUndefinedError;
      }
      LEP_LOGD("status <- 0x%04x", statusData);

      if (!(statusData & 1)) {
        return (FlirLepton::Result)(int8_t)(statusData >> 8);          
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

  bool FlirLepton::readVoSpi(size_t bufferLen, uint8_t* buffer) {
    digitalWrite(csPin_, LOW);
    spi_->beginTransaction(spiSettings_);

    uint16_t id = spi_->transfer16(0);
    bool isDiscard = ((id >> 8) & 0x0f) == 0x0f;
    uint16_t crc = spi_->transfer16(0);
    LEP_LOGI("VoSpi ID 0x%04x, CRC 0x%04x", id, crc);
    for (size_t i=0; i<videoPacketDataLen_/2; i++) {
      uint16_t buf = spi_->transfer16(0);
      if (!isDiscard) {
      buffer[i*2] = buf >> 8;
      buffer[i*2+1] = buf & 0xff;
      }
    }

    spi_->endTransaction();
    digitalWrite(csPin_, HIGH);

    return !isDiscard;
  }
