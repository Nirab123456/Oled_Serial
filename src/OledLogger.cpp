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
  // Use safe I2C clock (many cheap modules misbehave at 400kHz)
  Wire.setClock(100000);

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

  // Stable, deterministic display config
  _display->clearDisplay();
  _display->setRotation(0);
  _display->setTextSize(1);                 // fixed text size
  _display->setTextColor(SSD1306_WHITE);    // draw white pixels
  _display->cp437(false);                   // normal ASCII mapping (avoid CP437 remap)
  _display->setTextWrap(false);             // we handle wrap/clip manually
  _display->display();
  _display->setContrast(0xFF);

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

  // Ensure string is printable ASCII only (strip control chars)
  for (size_t i = 0; i < sizeof(m.txt); ++i) {
    if ((unsigned char)m.txt[i] < 0x20) {
      if (m.txt[i] == '\0') break;
      m.txt[i] = '?';
    }
  }

  sendOrDropOldest(m);
}

BaseType_t OledLogger::logFromISR(const char* utf8msg)
{
  if (!_queue) return pdFALSE;
  msg_t m;
  strncpy(m.txt, utf8msg, sizeof(m.txt) - 1);
  m.txt[sizeof(m.txt) - 1] = '\0';

  // sanitize control chars that may corrupt glyph rendering
  for (size_t i = 0; i < sizeof(m.txt); ++i) {
    if ((unsigned char)m.txt[i] < 0x20) {
      if (m.txt[i] == '\0') break;
      m.txt[i] = '?';
    }
  }

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
  const int TEXT_SIZE = 1;               // must match begin() setting
  const int LINE_HEIGHT = 8 * TEXT_SIZE; // 8 px per font line for textSize=1

  // Maximum number of lines we can reasonably store in RAM (safe upper bound)
  const int MAX_LINES = 16;

  // Compute how many lines fit on the display, clamp to MAX_LINES
  int LINES = _height / LINE_HEIGHT;
  LINES = std::max(1, std::min(LINES, MAX_LINES));

  // Circular buffer for lines: fixed-size array
  static char lines[MAX_LINES][sizeof(msg_t::txt)];
  // Initialize buffer (only once)
  static bool initOnce = false;
  if (!initOnce) {
    for (int i = 0; i < MAX_LINES; ++i) lines[i][0] = '\0';
    initOnce = true;
  }

  int writeIndex = -1; // newest message index (circular)
  msg_t incoming;

  for (;;) {
    if (xQueueReceive(_queue, &incoming, portMAX_DELAY) == pdTRUE) {
      writeIndex = (writeIndex + 1) % LINES;
      // copy safely
      strncpy(lines[writeIndex], incoming.txt, sizeof(incoming.txt));
      lines[writeIndex][sizeof(incoming.txt) - 1] = '\0';

      // redraw display: oldest -> newest
      _display->clearDisplay();
      _display->setTextSize(TEXT_SIZE);
      _display->setTextColor(SSD1306_WHITE);

      // Compute oldest index in circular buffer
      int start = (writeIndex + 1) % LINES;

      for (int i = 0; i < LINES; ++i) {
        int idx = (start + i) % LINES;

        // CLEAR the line background before printing the new text to avoid leftover pixels
        _display->fillRect(0, i * LINE_HEIGHT, _width, LINE_HEIGHT, SSD1306_BLACK);

        // Ensure cursor at the start of the line and print (use print, not println)
        _display->setCursor(0, i * LINE_HEIGHT);
        _display->print(lines[idx]); // no println -> deterministic X/Y
      }

      // finally push to hardware once per frame (faster and avoids flicker)
      _display->display();
    }
  }
  // never returns
}
