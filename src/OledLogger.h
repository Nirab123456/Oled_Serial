#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <stdarg.h>

class OledLogger {
public:
  // Begin the logger. Call in setup().
  // sda_pin/scl_pin default to -1 (Wire.begin() default) if you set to -1.
  static bool begin(uint8_t i2c_addr = 0x3C,
                    int width = 128,
                    int height = 64,
                    int sda_pin = -1,
                    int scl_pin = -1,
                    size_t queue_len = 16,
                    UBaseType_t task_priority = 1,
                    BaseType_t pinned_core = 1);

  // printf style logging from tasks (non-blocking, drops oldest on overflow)
  static void logf(const char* fmt, ...);

  // safe logging from ISR. returns pdTRUE if posted, pdFALSE if queue full.
  static BaseType_t logFromISR(const char* utf8msg);

  // optional: check if initialized
  static bool isReady();

private:
  // internal message structure
  struct msg_t {
    char txt[64]; // keep same size as your original; increase if you need longer lines
  };

  static TaskHandle_t    _taskHandle;
  static QueueHandle_t   _queue;
  static Adafruit_SSD1306* _display;
  static int            _width;
  static int            _height;
  static uint8_t        _i2c_addr;
  static size_t         _queue_len;

  static void taskFunc(void* pv);

  // helper to safely send a message (non-ISR)
  static void sendOrDropOldest(const msg_t &m);
};
