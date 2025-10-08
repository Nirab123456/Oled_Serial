// OledLogger.cpp
#include "OledLogger.h"
#include <algorithm> // for std::min/std::max
#include <string.h>  // for strncpy

// Static member definitions
TaskHandle_t      OledLogger::_taskHandle = nullptr;
QueueHandle_t     OledLogger::_queue = nullptr;
Adafruit_SSD1306* OledLogger::_display = nullptr;
int               OledLogger::_width = 128;
int               OledLogger::_height = 64;
uint8_t           OledLogger::_i2c_addr = 0x3C;
size_t            OledLogger::_queue_len = 16;

bool OledLogger::isReady() {
  return (_queue != nullptr) && (_display != nullptr);
}

bool OledLogger::begin(uint8_t i2c_addr,
                       int width,
                       int height,
                       int sda_pin,
                       int scl_pin,
                       size_t queue_len,
                       UBaseType_t task_priority,
                       BaseType_t pinned_core)
{
  // store config
  _i2c_addr = i2c_addr;
  _width = width;
  _height = height;
  _queue_len = (queue_len < 1) ? 1 : queue_len;

  // init Wire (only set pins if valid)
  if (sda_pin >= 0 && scl_pin >= 0) {
    Wire.begin((int)sda_pin, (int)scl_pin);
  } else {
    Wire.begin();
  }

  // allocate display instance
  _display = new Adafruit_SSD1306((uint8_t)_width, (uint8_t)_height, &Wire, -1);
  if (!_display) {
    Serial.println("OLED: memory allocation failed");
    return false;
  }

  if (!_display->begin(SSD1306_SWITCHCAPVCC, _i2c_addr)) {
    Serial.println("OLED INIT FAILED");
    delete _display;
    _display = nullptr;
    return false;
  }

  _display->clearDisplay();
  _display->setTextSize(1);           // fixed text size (adjust here if you will use a different text size)
  _display->setTextColor(SSD1306_WHITE);
  _display->setCursor(0, 0);
  _display->display();

  // create queue
  _queue = xQueueCreate((UBaseType_t)_queue_len, sizeof(msg_t));
  if (!_queue) {
    Serial.println("OLED QUEUE creation failed");
    delete _display;
    _display = nullptr;
    return false;
  }

  // create task
  BaseType_t created = xTaskCreatePinnedToCore(
      &OledLogger::taskFunc,
      "OLED_DEBUGGER",
      4096,
      nullptr,
      task_priority,
      &_taskHandle,
      pinned_core);

  if (created != pdPASS) {
    Serial.println("OLED task creation failed");
    vQueueDelete(_queue);
    _queue = nullptr;
    delete _display;
    _display = nullptr;
    return false;
  }

  return true;
}

void OledLogger::sendOrDropOldest(const msg_t &m)
{
  if (xQueueSend(_queue, &m, 0) == pdTRUE) return;

  // Queue full: remove one oldest entry and try again (drop oldest policy)
  msg_t tmp;
  if (xQueueReceive(_queue, &tmp, 0) == pdTRUE) {
    // dropped
  }
  xQueueSend(_queue, &m, 0);
}

void OledLogger::logf(const char* fmt, ...)
{
  if (!_queue) return;

  msg_t m;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(m.txt, sizeof(m.txt), fmt, ap);
  va_end(ap);

  sendOrDropOldest(m);
}

BaseType_t OledLogger::logFromISR(const char* utf8msg)
{
  if (!_queue) return pdFALSE;
  msg_t m;
  strncpy(m.txt, utf8msg, sizeof(m.txt) - 1);
  m.txt[sizeof(m.txt) - 1] = '\0';

  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  BaseType_t res = xQueueSendFromISR(_queue, &m, &xHigherPriorityTaskWoken);
  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
  return res;
}

void OledLogger::taskFunc(void* pv)
{
  (void)pv;
  if (!_display) {
    vTaskDelete(nullptr);
    return;
  }

  // TEXT_SIZE: keep explicit and deterministic
  const int TEXT_SIZE = 1;              // change if you intentionally use a different text size
  const int LINE_HEIGHT = 8 * TEXT_SIZE; // 8 px per font line for textSize=1

  // Maximum number of lines we can reasonably store in RAM (safe upper bound)
  const int MAX_LINES = 16;

  // Compute how many lines fit on the display, clamp to MAX_LINES
  int LINES = _height / LINE_HEIGHT;
  LINES = std::max(1, std::min(LINES, MAX_LINES));

  // Circular buffer for lines: fixed-size array
  static char lines[MAX_LINES][sizeof(msg_t::txt)];
  // Initialize buffer
  for (int i = 0; i < MAX_LINES; ++i) lines[i][0] = '\0';

  int writeIndex = -1; // newest message index (circular)

  msg_t incoming;
  for (;;) {
    if (xQueueReceive(_queue, &incoming, portMAX_DELAY) == pdTRUE) {
      writeIndex = (writeIndex + 1) % LINES;
      strncpy(lines[writeIndex], incoming.txt, sizeof(incoming.txt));
      lines[writeIndex][sizeof(incoming.txt) - 1] = '\0';

      // redraw display: oldest -> newest
      _display->clearDisplay();
      _display->setTextSize(TEXT_SIZE);
      _display->setTextColor(SSD1306_WHITE);

      int start = (writeIndex + 1) % LINES; // index of the oldest stored message
      for (int i = 0; i < LINES; ++i) {
        int idx = (start + i) % LINES;
        _display->setCursor(0, i * LINE_HEIGHT);
        _display->println(lines[idx]);
      }
      _display->display();
    }
  }
}
