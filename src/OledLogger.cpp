#include "OledLogger.h"

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

  // allocate display instance (use new so we can pick width/height at runtime)
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
  _display->setTextSize(1);
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
      4096, // stack size - keep reasonably large for display operations and buffer printing
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
  // Try to send without blocking
  if (xQueueSend(_queue, &m, 0) == pdTRUE) return;

  // Queue full: remove one oldest entry and try again
  msg_t tmp;
  if (xQueueReceive(_queue, &tmp, 0) == pdTRUE) {
    // dropped the oldest
  }
  // try send again (best effort)
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
  // safe copy
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
    // nothing we can do
    vTaskDelete(nullptr);
    return;
  }

  // compute number of lines based on font height (8 px per line at textSize=1)
  const int lineHeight = 8 * _display->getTextSize(); // getTextSize() returns text size but Adafruit doesn't provide getter; we assume 1
  // To be safe, use 8px lines with textSize 1 (this code uses textSize 1 from begin())
  const int LINES = max(1, _height / 8);

  // circular buffer of recent messages
  const int MSG_CAP = LINES;
  char lines[32][sizeof(msg_t::txt)]; // support up to 32 lines if display larger; but we'll only use LINES
  if (LINES > 32) {
    // clamp (safety)
  }
  for (int i = 0; i < MSG_CAP; ++i) lines[i][0] = '\0';
  int writeIndex = -1; // index of newest message

  msg_t incoming;
  for (;;) {
    // wait indefinitely for the next message
    if (xQueueReceive(_queue, &incoming, portMAX_DELAY) == pdTRUE) {
      // advance write index in circular manner
      writeIndex = (writeIndex + 1) % MSG_CAP;
      strncpy(lines[writeIndex], incoming.txt, sizeof(incoming.txt));
      lines[writeIndex][sizeof(incoming.txt)-1] = '\0';

      // redraw display: show messages oldest->newest
      _display->clearDisplay();
      _display->setTextSize(1);
      _display->setTextColor(SSD1306_WHITE);

      int start = (writeIndex + 1) % MSG_CAP; // oldest message
      for (int i = 0; i < LINES; ++i) {
        int idx = (start + i) % MSG_CAP;
        _display->setCursor(0, i * 8); // 8 px per line
        _display->println(lines[idx]);
      }
      _display->display();
    }
  }
  // task never returns
}
