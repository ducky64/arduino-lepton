# esp32-lepton
ESP32-S3 FLIR Lepton firmware with Arduino HAL and PlatformIO.

Goal is an generic Arduino library with a few self-contained applications - maybe USB UVC and a web steamer.

## Related Work
These projects do similar things:
- https://github.com/danjulio/tCam: ESP-IDF framework for ESP32E, GPL-3.0 license. Device firmware, not split into a library - though potentially could be done. Potentially needs companion apps to do anything.
- https://github.com/NachtRaveVL/Lepton-FLiR-Arduino: Arduino-based library, MIT license. Appears to be a dead project, last code update Aug 2020. Issues suggest potential issues with ESP32-S3, doesn't appear to be support for Lepton 3.5.
