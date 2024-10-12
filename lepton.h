#ifndef __LEPTON_H__
#define __LEPTON_H__

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>


class FlirLepton {
public:
  const uint8_t kI2cAddr = 0x2a;  // 7-bit addressing
  enum RegAddr {
    kRegPowerOnOff = 0x0000,
    kRegStatus = 0x0002,
    kRegCommandId = 0x0004,
    kRegDataLen = 0x0006,
    kRegData0 = 0x0008,  // 16 data registers
  };

  enum ModuleId {
    kAgc = 1,
    kSys = 2,
    kVid = 3,
    kOem = 8,
    kRad = 14,
  };

  enum CommandType {
    kGet = 0,
    kSet = 1,
    kRun = 2,
  };

  enum Result {
    kLepOk = 0,
    kUndefinedError = -127,  // also used here for I2C errors
  };

  // Initializes this class without any hardware operations
  FlirLepton(TwoWire& wire, SPIClass& spi, int cs, int reset);

  // Acquires hardware resources (setting pin direction) and performs initial power-on reset.
  // Wire and Spi should be init'd beforehand.
  // PowerDown must be asserted externally (HIGH).
  // Returns whether init was successful
  bool begin();

  // Releases hardware resources, including tri-stating GPIOs
  // Does not touch Wire and Spi.
  void end();

  // Returns true if the device has booted and is ready for operation.
  bool isReady();

  /** Utility functions
  */
  // Enable the VSYNC output on GPIO
  bool enableVsync();

  /** SPI Operations
   * TODO: move into a separate class to allow more optimization
   */
  // Reads VoSpi frame. Must be called regularly to maintain sync.
  // Returns true if a frame was read (and stored in buffer), otherwise false (eg, discard frame read).
  // buffer may still be (over)written to even if no video data is present.
  bool readVoSpi(size_t bufferLen, uint8_t* buffer, bool* readErrorOut);

protected:
  /** I2C Operations 
   */
  // Sends and executes commands to the camera
  // len is in bytes, double the SDK data length in the IDD document
  Result commandGet(ModuleId moduleId, uint8_t moduleCommandId, uint16_t len, uint8_t *dataOut, bool oemBit = false);
  Result commandSet(ModuleId moduleId, uint8_t moduleCommandId, uint16_t len, uint8_t *data, bool oemBit = false);
  Result commandRun(ModuleId moduleId, uint8_t moduleCommandId);

  // Polls until the status register is non-busy, and return the error code.
  // Comms errors map to -127
  Result readNonBusyStatus();

  // Writes data to a 16-bit register, returning success
  bool writeReg16(uint16_t addr, uint16_t data);
  // Writes len sequential bytes to a register, returning success
  bool writeReg(uint16_t addr, size_t len, uint8_t* data);

  // Reads the contents of a 16-bit register, returning success and placing data in dataOut (on success)
  bool readReg16(uint16_t addr, uint16_t* dataOut);
  // Reads len sequential bytes from a register, placing the results in dataOut, returning success
  bool readReg(uint16_t addr, size_t len, uint8_t* dataOut);

  /** State and configuration variables
   */
  TwoWire* wire_;
  SPIClass* spi_;
  // SPISettings spiSettings_;  // TODO kDefaultSpiSettings seems to be unavailable until after construction so this can't be init'd
  int csPin_, resetPin_;

  int resetMillis_;  // millis() at which the device exited reset
  bool i2cReady_ = false;  // if sufficient millis() has elapsed since reset for I2C to be up

  uint64_t flirSerial_ = 0;
  char flirPartNum_[33] = {0};
  uint8_t flirSoftwareVersion_[8];

  bool metadataRead_ = false;  // true when the above fields have been attempted to be read

  uint8_t bytesPerPixel_ = 2;
  uint8_t frameWidth_ = 160, frameHeight_ = 120;
  size_t videoPacketDataLen_ = 160;  // bytes, 160 in Raw14 mode (default), 240 in RGB888 mode
  size_t packetsPerSegment_ = 60;  // Lepton 3.5, telemetry disabled
  size_t segmentsPerFrame_ = 4;

  static const SPISettings kDefaultSpiSettings;
};

#endif
