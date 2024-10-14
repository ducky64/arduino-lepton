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
  FlirLepton(TwoWire& wire, SPIClass& spi, int cs, int reset, int pwrdn = -1);

  // Acquires hardware resources (setting pin direction) and performs initial power-on reset. PWRDN is asserted if not NC.
  // Wire and Spi should be init'd beforehand.
  // Returns whether init was successful.
  // Must poll isReady() afterwards to check for camera ready.
  // Can be called multiple times to initiate a reset.
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

  enum VideoMode {
    k14Bit,  // default 14-bit
    kTLinear,  // 16-bit TLinear
    kAgcLinear,  // histogram-based AGC
    kAgcHeq
  };
  // Sets the video mode, managing both the AGC and TLinear registers
  bool setVideoMode(VideoMode mode);

  enum VideoFormat {
    kGrey14,
    kRgb888
  };
  enum PColorLut {
    kLutWheel6 = 0,
    kLutFusion,
    kLutRainbow,
    kLutGlobow,
    kLutSephia,
    kLutColor,
    kLutIceFire,
    kLutRain,
    kLutUser,
  };
  // Sets the video format, with an optional colorization LUT (ignored for non-RGB cases)
  bool setVideoFormat(VideoFormat format, PColorLut lut = kLutFusion);

  /** SPI Operations
   * TODO: move into a separate class to allow more optimization
   */
  // Reads a VoSpi frame. Must be called regularly to maintain sync.
  // Blocks while a frame is being read out, returns quicker inbetween frames.
  // Returns true if a frame was read (and stored in buffer), otherwise false (eg, discard packet read).
  // bufferWrittenOut is set to true if the buffer has been overwritten, even partially.
  bool readVoSpi(size_t bufferLen, uint8_t* buffer, bool* bufferWrittenOut = nullptr);

  /** Metadata operations
  */
 // returns the FLIR serial number from the device, valid only after isReady()
  uint64_t getFlirSerial() {
    return flirSerial_;
  }

  // returns the FLIR part number from the device as a string, valid only after isReady()
  const char* getFlirPartNum() {
    return flirPartNum_;
  }

  // returns the FLIR software version from the device as a 6-byte array, valid only after isReady()
  const uint8_t* getFlirSoftwareVerison() {
    return flirSoftwareVersion_;
  }

  // returns frame width in pixels, valid only after isReady()
  size_t getFrameWidth() {
    return frameWidth_;
  }

  // returns frame width in pixels, valid only after isReady()
  size_t getFrameHeight() {
    return frameHeight_;
  }

  // returns bytes per pixel, currently 2=16-bit grey mode, 3-RGB888 format, valid only after isReady()
  size_t getBytesPerPixel() {
    return bytesPerPixel_;
  }

  // sets the video parameters, can be useful if using a different device or configuration this library doesn't support
  void setVideoParameters(uint8_t bytesPerPixel, uint8_t frameWidth, uint8_t frameHeight,
      size_t videoPacketDataLen, size_t packetsPerSegment, size_t segmentsPerFrame) {
    bytesPerPixel_ = bytesPerPixel;
    frameWidth_ = frameWidth;
    frameHeight_ = frameHeight;
    videoPacketDataLen_ = videoPacketDataLen;
    packetsPerSegment_ = packetsPerSegment;
    segmentsPerFrame_ = segmentsPerFrame;
  }

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
  int csPin_, resetPin_, pwrdnPin_;

  int resetMillis_;  // millis() at which the device exited reset
  bool i2cReady_ = false;  // if sufficient millis() has elapsed since reset for I2C to be up

  uint64_t flirSerial_ = 0;
  char flirPartNum_[33] = {0};
  uint8_t flirSoftwareVersion_[8] = {0};

  bool metadataRead_ = false;  // true when the above fields have been attempted to be read

  // mode configuration
  VideoMode videoMode_ = kTLinear;  // default for Lepton 3.5, TODO for non-radiometric devices

  // video data configuration
  uint8_t bytesPerPixel_ = 2;
  uint8_t frameWidth_ = 160, frameHeight_ = 120;

  size_t videoPacketDataLen_ = 160;  // bytes, 160 in Raw14 mode (default), 240 in RGB888 mode
  size_t packetsPerSegment_ = 60;  // Lepton 3.5, telemetry disabled
  size_t segmentsPerFrame_ = 4;

  bool resyncRequested_ = false;
  int resyncStartMillis_ = 0;  // millis() at which resync ends
  bool inResync_ = false;

  const uint16_t kResyncMillis = 185;
  static const SPISettings kDefaultSpiSettings;
};

#endif
