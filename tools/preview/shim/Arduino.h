#pragma once
// Minimal Arduino shim so Adafruit_GFX + U8g2_for_Adafruit_GFX build on a PC.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string>
#ifndef ARDUINO
#define ARDUINO 10819
#endif
#define PROGMEM
#define pgm_read_byte(addr) (*(const unsigned char *)(addr))
#define pgm_read_word(addr) (*(const unsigned short *)(addr))
#define pgm_read_dword(addr) (*(const unsigned long *)(addr))
#define pgm_read_ptr(addr) (*(void* const*)(addr))
#define radians(d) ((d) * 0.017453292519943295)
typedef bool boolean;
typedef uint8_t byte;
class __FlashStringHelper;
class String {
 public:
  std::string s;
  String(const char* c = "") : s(c) {}
  const char* c_str() const { return s.c_str(); }
  unsigned length() const { return (unsigned)s.size(); }
};
#include "Print.h"
