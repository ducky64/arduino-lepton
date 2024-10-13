# arduino-lepton
FLIR Lepton drivers for Arduino HAL. Tested with ESP32-S3 + Lepton 3.5 on a custom board, but (in concept) should work on most Arduino platforms that have the necessary and computational power.

**This README under construction. Will also be pushed to PlatformIO registry.**


## Examples
- ESP32-S3 webserver example with single-frame capture and streaming MJPEG, in greyscale or RGB888 (colorized) mode. Uses the [JPEGENC](https://github.com/bitbank2/JPEGENC) library with FreeRTOS (part of all ESP32 builds). Pinmaps at the beginning of main.cpp and can be changed for your particular hardware. Likely compatible across the ESP32 family.


## Usage

**Under construction**

### Notes
- VoSPI is a timing-sensitive protocol and desynchronizes if frames are not read out promptly. If using an RTOS, the Lepton driver needs to be high priority to ensure SPI is running fast enough.


## Related Work
These projects do similar things:
- https://github.com/danjulio/tCam: ESP-IDF framework for ESP32E, GPL-3.0 license. Device firmware, not split into a library - though potentially could be done. Potentially needs companion apps to do anything.
- https://github.com/NachtRaveVL/Lepton-FLiR-Arduino: Arduino-based library, MIT license. Appears to be a dead project, last code update Aug 2020. Issues suggest potential issues with ESP32-S3 and Lepton 3.5.
- https://github.com/groupgets/purethermal1-firmware: STM32 using the STM32Cube (?) framework
