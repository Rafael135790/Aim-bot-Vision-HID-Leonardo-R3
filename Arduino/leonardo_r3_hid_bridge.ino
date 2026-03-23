#include <SPI.h> 
#include <usbhid.h>
#include <hiduniversal.h>
#include <usbhub.h>
#include <HID-Project.h>
#include <avr/wdt.h>

uint8_t hidBuffer[64];

// ======================
// USB (DECLARADO ANTES ✔)
// ======================
USB Usb;
HIDUniversal Hid(&Usb);

// ======================
// USB RECOVERY
// ======================
unsigned long lastRecoveryAttempt = 0;
unsigned long lastRunningTime = 0;

const unsigned long RECOVERY_COOLDOWN_MS = 2000;
const unsigned long NOT_RUNNING_TIMEOUT_MS = 3000;

void recoverUsbHost() {
  unsigned long now = millis();

  if (now - lastRecoveryAttempt < RECOVERY_COOLDOWN_MS) return;
  lastRecoveryAttempt = now;

  delay(100);
  Usb.Init();
  delay(200);
}

// ======================
// BUFFERS
// ======================
int16_t deltaMouseX = 0;
int16_t deltaMouseY = 0;

int16_t deltaPyX = 0;
int16_t deltaPyY = 0;
int16_t deltaWheel = 0;

float remainderX = 0.0;
float remainderY = 0.0;

bool leftButtonClicked = false;
unsigned long lastClickTimeMicros = 0;
const unsigned long persistenceDurationMicros = 1000000UL;

unsigned long lastProcessTime = 0;
const unsigned long pollingInterval = 1000UL;

// ======================
// RAW HID
// ======================
static const uint8_t RAW_REPORT_SIZE = 64;
uint8_t rawRx[RAW_REPORT_SIZE];
uint8_t rawFill = 0;

// ======================
// PARSER
// ======================
class MouseRptParser : public HIDReportParser {
public:
  void Parse(USBHID *hid, bool is_rpt_id, uint8_t len, uint8_t *buf) {
    if (len < 6) return;

    uint8_t buttons = buf[0];
    leftButtonClicked = (buttons & 1);

    if (leftButtonClicked) {
      lastClickTimeMicros = micros();
    }

    int8_t xf = (int8_t)buf[1];
    int8_t yf = (int8_t)buf[3];
    int8_t wf = (int8_t)buf[5];

    deltaMouseX += xf;
    deltaMouseY += yf;
    deltaWheel += wf;

    if (buttons & 1) Mouse.press(MOUSE_LEFT); else Mouse.release(MOUSE_LEFT);
    if (buttons & 2) Mouse.press(MOUSE_RIGHT); else Mouse.release(MOUSE_RIGHT);
    if (buttons & 4) Mouse.press(MOUSE_MIDDLE); else Mouse.release(MOUSE_MIDDLE);
    if (buttons & 8) Mouse.press(MOUSE_PREV); else Mouse.release(MOUSE_PREV);
    if (buttons & 16) Mouse.press(MOUSE_NEXT); else Mouse.release(MOUSE_NEXT);
  }
};

MouseRptParser Prs;

// ======================
// SETUP
// ======================
void setup() {
  wdt_disable();

  delay(800); // estabiliza energia

  Mouse.begin();
  RawHID.begin(hidBuffer, sizeof(hidBuffer));

  if (Usb.Init() == -1) {
    delay(300);
    if (Usb.Init() == -1) {
      while (1) delay(1000);
    }
  }

  delay(200);
  Hid.SetReportParser(0, &Prs);

  lastProcessTime = micros();
  lastRunningTime = millis();

  wdt_enable(WDTO_2S);
}

// ======================
// LOOP
// ======================
void loop() {
  wdt_reset();
  Usb.Task();

  // ======================
  // USB WATCHDOG (NOVO)
  // ======================
  uint8_t state = Usb.getUsbTaskState();
  unsigned long nowMillis = millis();

  if (state == USB_STATE_RUNNING) {
    lastRunningTime = nowMillis;
  }

  if (state == USB_STATE_ERROR) {
    recoverUsbHost();
    return;
  }

  if ((nowMillis - lastRunningTime) > NOT_RUNNING_TIMEOUT_MS) {
    recoverUsbHost();
    return;
  }

  // ======================
  // LÓGICA ORIGINAL
  // ======================
  unsigned long nowMicros = micros();
  unsigned long timeSinceLastClick = nowMicros - lastClickTimeMicros;

  bool pythonActive = leftButtonClicked || (timeSinceLastClick < persistenceDurationMicros);

  static unsigned long lastRawArrivalMicros = 0;

  if (rawFill > 0 && (nowMicros - lastRawArrivalMicros > 500000UL)) {
    rawFill = 0;
  }

  while (RawHID.available() > 0) {
    lastRawArrivalMicros = nowMicros;

    int v = RawHID.read();
    if (v < 0) {
      rawFill = 0;
      break;
    }

    rawRx[rawFill++] = (uint8_t)v;

    if (rawFill == RAW_REPORT_SIZE) {
      if (pythonActive) {
        float pyX = (float)((int8_t)rawRx[0]);
        float pyY = (float)((int8_t)rawRx[1]);

        if (!leftButtonClicked && timeSinceLastClick > 500000UL) {
          pyX /= 2.0;
          pyY /= 2.0;
        }

        if (pyX > 0) pyX += 0.3; else if (pyX < 0) pyX -= 0.3;
        if (pyY > 0) pyY += 0.3; else if (pyY < 0) pyY -= 0.3;

        remainderX += pyX;
        remainderY += pyY;

        int16_t moveIntX = (int16_t)remainderX;
        int16_t moveIntY = (int16_t)remainderY;

        remainderX -= moveIntX;
        remainderY -= moveIntY;

        deltaPyX += moveIntX;
        deltaPyY += moveIntY;
        deltaWheel += (int8_t)rawRx[2];
      }

      rawFill = 0;
    }
  }

  // ======================
  // PROCESSAMENTO
  // ======================
  if (nowMicros - lastProcessTime >= pollingInterval) {
    lastProcessTime = nowMicros;

    int32_t totalX = deltaMouseX;
    int32_t totalY = deltaMouseY;

    if (pythonActive) {
      totalX += deltaPyX;
      totalY += deltaPyY;
    } else {
      deltaPyX = 0;
      deltaPyY = 0;
      remainderX = 0.0;
      remainderY = 0.0;
    }

    if (totalX != 0 || totalY != 0 || deltaWheel != 0) {
      int16_t moveX = (int16_t)constrain(totalX, -32767, 32767);
      int16_t moveY = (int16_t)constrain(totalY, -32767, 32767);
      int16_t moveW = (int16_t)constrain(deltaWheel, -127, 127);

      Mouse.move(moveX, moveY, moveW);

      deltaMouseX -= (int16_t)constrain(deltaMouseX, -32767, 32767);
      deltaMouseY -= (int16_t)constrain(deltaMouseY, -32767, 32767);

      if (pythonActive) {
        deltaPyX -= (int16_t)constrain(deltaPyX, -32767, 32767);
        deltaPyY -= (int16_t)constrain(deltaPyY, -32767, 32767);
      }

      deltaWheel -= moveW;
    }
  }
}