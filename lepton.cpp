#include "lepton.h"


// Class constants
const SPISettings FlirLepton::kDefaultSpiSettings(20000000, MSBFIRST, SPI_MODE3);  // 20MHz max for VoSPI, CPOL=1, CPHA=1


// Override these to use some other logging framework
static const char* TAG = "lepton";
#define LEP_LOGV(...) ESP_LOGV(TAG, __VA_ARGS__)
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


FlirLepton::FlirLepton(TwoWire& wire, SPIClass& spi, int cs, int reset) : 
    wire_(&wire), spi_(&spi), csPin_(cs), resetPin_(reset) {
};

bool FlirLepton::begin() {
    digitalWrite(csPin_, HIGH);
    pinMode(csPin_, OUTPUT);
    digitalWrite(csPin_, HIGH);

    digitalWrite(resetPin_, LOW);  // assert RESET
    pinMode(resetPin_, OUTPUT);
    digitalWrite(resetPin_, LOW);  // assert RESET
    delay(1);  // wait >5000 clk periods @ 25MHz MCLK, Lepton Eng Datasheet Figure 6
    digitalWrite(resetPin_, HIGH);

    resetMillis_ = millis();
    i2cReady_ = false;
    return true;
  }

  bool FlirLepton::isReady() {
    if (!i2cReady_ && millis() < resetMillis_ + 950) {  // minimum wait before accessing I2C, Lepton Software IDD
      return false;
    }

    uint16_t statusData;
    if (!readReg16(kRegStatus, &statusData)) {
      LEP_LOGE("isReady() read status failed");
      return false;
    }
    LEP_LOGD("isReady() status <- 0x%04x", statusData);

    if (statusData & (1 << 2) && !(statusData & 1)) {
      if (!(statusData & (1 << 1))) {
        LEP_LOGE("isReady() unexpected boot mode bit");
        return false;
      }
    } else {
      return false;  // not yet ready
    }

    Result result;
    uint8_t cmdBuffer[32];

    if (!metadataRead_) {  // read out serial
      result = commandGet(kSys, 0x08 >> 2, 8, cmdBuffer);
      if (result != kLepOk) {
        LEP_LOGE("isReady() SYS FLIR Serial commandGet failed %i", result);
        return false;
      }
      uint64_t flirSerial = bufferToU64(cmdBuffer);
      LEP_LOGI("isReady() SYS FLIR serial = %llu, 0x%016llx", flirSerial, flirSerial);
      if (flirSerial == 0) {  // a sanity check on comms correctness
        LEP_LOGW("isReady() failed sanity check: zero FLIR serial");
      }

      size_t kPartNumberLen = 16;  // 32 in the IDD, but only 16 registers to read out of
      result = commandGet(kOem, 0x1c >> 2, kPartNumberLen, cmdBuffer, true);
      if (result != kLepOk) {
        LEP_LOGE("isReady() OEM FLIR Part Number commandGet failed %i", result);
        return false;
      }
      // result seems in the wrong endianness
      for (size_t i=0; i<kPartNumberLen/2; i++) {
        flirPartNum_[i*2] = cmdBuffer[i*2 + 1];
        flirPartNum_[i*2 + 1] = cmdBuffer[i*2];
      }
      LEP_LOGI("isReady() OEM FLIR Part Number = '%s'", flirPartNum_);

      result = commandGet(kOem, 0x20 >> 2, 8, flirSoftwareVersion_, true);
      if (result != kLepOk) {
        LEP_LOGE("isReady() OEM Camera Software Revision commandGet failed %i", result);
        return false;
      }
      LEP_LOGI("isReady() OEM Camera Software Revision = GPP = 0x %02x %02x %02x, DSP = 0x %02x %02x %02x",
          flirSoftwareVersion_[0], flirSoftwareVersion_[1], flirSoftwareVersion_[2], 
          flirSoftwareVersion_[3], flirSoftwareVersion_[4], flirSoftwareVersion_[5]);

      metadataRead_ = true;
    }
    // guaranteed to have read out metadata by this point

    result = commandGet(kSys, 0x44 >> 2, 4, cmdBuffer);
    if (result != kLepOk) {
      LEP_LOGE("isReady() SYS FFC status commandGet failed %i", result);
      return false;
    }
    int32_t ffcStatus = bufferToI32(cmdBuffer);
    LEP_LOGD("isReady() SYS FFC <- %i", ffcStatus);
    if (ffcStatus == 0) {  // ready, continue with init
    } else if (ffcStatus < 0) {
      LEP_LOGE("isReady() SYS FFC returned error %i", ffcStatus);
      return false;
    } else {
      return false;
    }

    return true;
  }


  void FlirLepton::end() {
    pinMode(csPin_, INPUT);
    pinMode(resetPin_, INPUT);
  }


  bool FlirLepton::enableVsync() {
    uint8_t buffer[4];
    U32ToBuffer(5, buffer);
    Result result = commandSet(kOem, 0x54 >> 2, 4, buffer, true);
    if (result != kLepOk) {
      LEP_LOGW("enableVsync() command returned %i", result);
    }
    return result == kLepOk;
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

  bool FlirLepton::readVoSpi(size_t bufferLen, uint8_t* buffer, bool* readErrorOut) {
    size_t requiredBuffer = videoPacketDataLen_ * packetsPerSegment_ * segmentsPerFrame_;
    if (bufferLen < requiredBuffer) {
      LEP_LOGE("readVoSpi insufficient buffer, got %i need %i", bufferLen, requiredBuffer);
      return false;
    }

    spi_->beginTransaction(kDefaultSpiSettings);
    // spi_->setFrequency(20000000);  // alternate approach without transactions
    // spi_->setBitOrder(MSBFIRST);
    // spi_->setDataMode(SPI_MODE3);
    digitalWrite(csPin_, LOW);

    bool invalidate = false;
    for (uint8_t segment=1; segment <= segmentsPerFrame_ && !invalidate; segment++) {
      bool discardSegment = false;
      for (size_t packet=0; packet < packetsPerSegment_ && !invalidate; packet++) {
        delayMicroseconds(10);  // this is the magic

        uint8_t *bufferPtr = buffer + ((segment - 1) * videoPacketDataLen_ * packetsPerSegment_) + (packet * videoPacketDataLen_);

        // uint16_t id = spi_->transfer16(0);  // broken for some reason
        // uint16_t crc = spi_->transfer16(0);
        uint8_t header[4];
        spi_->transfer(header, 4);
        spi_->transfer(bufferPtr, 160);  // always read a whole packet
        uint16_t id = ((uint16_t)header[0] << 8) | header[1];
        uint16_t crc = ((uint16_t)header[3] << 8) | header[4];

        if (((id >> 8) & 0x0f) == 0x0f) {  // discard packet
          if (packet == 0 && segment == 1) {  // if no frame in progress, return
            invalidate = true;
            break;
          } else {  // otherwise just ignore it - may show up in the middle of a transmission
            packet--;
            continue;
          }
        }
        uint16_t packetNum = id & 0xfff;
        uint8_t ttt = (id >> 12) & 0x7;

        if (packetNum != packet) {
          // desync can happen, the print affects timing and is off by default
          LEP_LOGW("unexpected packet num %i (seg %i), expected %i", packetNum, segment, packet);
          invalidate = true;
          if (readErrorOut != nullptr) {
            *readErrorOut = true;
          }
          break;
        }
        if (packetNum == 20) {
          if (ttt == 0) {
            discardSegment = true;
          } else if (ttt != segment) {
            LEP_LOGW("unexpected ttt %i, expected %i", ttt, segment);
            invalidate = true;
            if (readErrorOut != nullptr) {
              *readErrorOut = true;
            }
            break;
          }
        }
      }
      if (discardSegment) {
        segment--;
      }
    }

    digitalWrite(csPin_, HIGH);
    spi_->endTransaction();
    
    return !invalidate;
  }
