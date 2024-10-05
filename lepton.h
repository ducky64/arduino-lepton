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
  }

  enum CommandType {
    kGet = 0,
    kSet = 1,
    kRun = 2,
  }

  enum SysCommand {
    kSysFfcStatus = 0x44;  // GET -> 2
  }

  enum Result {
    kLepOk = 0
  }

  // Initializes this class without any hardware operations
  FlirLepton(TwoWire& wire, SPIClass& spi, int cs, int reset) : 
    wire_(&wire), spi_(&spi), csPin_(cs), resetPin_(reset) {};

  // Acquires hardware resources (setting pin direction) and performs initial power-on reset.
  // Wire and Spi must be init'd beforehand.
  // PowerDown must be asserted externally (HIGH).
  // Returns whether init was successful
  bool begin();

  // Releases hardware resources, including tri-stating GPIOs
  // Does not touch Wire and Spi.
  void end();

  // Sends and executes commands to the camera
  // len is in bytes, double the SDK data length in the IDD document
  bool FlirLepton::commandGet(ModuleId moduleId, uint8_t commandId, uint16_t len, uint8_t *dataOut);
  bool FlirLepton::commandRun(ModuleId moduleId, uint8_t commandId, uint16_t len, uint8_t *data);

  // Writes data to a 16-bit register, returning success
  bool writeReg16(uint16_t addr, uint16_t data);

  // Reads the contents of a 16-bit register, returning success and placing data in dataOut (on success)
  bool readReg16(uint16_t addr, uint16_t* dataOut);

protected:
  TwoWire* wire_;
  SPIClass* spi_;
  int csPin_, resetPin_;
};

#endif
