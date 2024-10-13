/** Utilities functions not related to core interfacing with the Lepton device */
#ifndef __LEPTON_UTIL_H__
#define __LEPTON_UTIL_H__

#include "stdint.h"

// Given the frame buffer, buffer width, line number, and a pointer to the output buffer, min value, max value
// converts the line to a numeric string
// Useful to visualize the output with just the console.
// outStr must be length of at least width + 1 (to include the null terminator)
void u16_frame_line_to_str(uint8_t* frame, uint16_t width, uint16_t line, char* outStr, uint16_t min=0, uint16_t max=65535) {
  uint8_t* linePtr = frame + (line * width * 2);
  uint16_t range = max - min;
  if (range == 0) {  // avoid division by zero
    range = 1;
  }
  for (uint16_t i=0; i<width; i++) {
    uint16_t pixel = ((uint16_t)(*linePtr) << 8) | *(linePtr+1);
    if (pixel < min) {
      pixel = min;
    } else if (pixel > max) {
      pixel = max;
    }
    linePtr += 2;
    *outStr = '0' + (uint32_t)(pixel - min) * 9 / range;
    outStr++;
  }
  *outStr = 0;  // null terminator
}

// Given a frame buffer, buffer width, buffer height, return the min and max values (as out-pointers)
void u16_frame_min_max(uint8_t* frame, uint16_t width, uint16_t height, uint16_t* minOut, uint16_t* maxOut) {
  uint16_t min = 65535, max = 0;
  for (uint16_t y=0; y<height; y++) {
    for (uint16_t x=0; x<width; x++) {
      uint16_t pixel = ((uint16_t)frame[2*(y*width+x)] << 8) | frame[2*(y*width+x) + 1];
      if (pixel < min) {
        min = pixel;
      }
      if (pixel > max) {
        max = pixel;
      }
    }
  }
  *minOut = min;
  *maxOut = max;
}

#endif
