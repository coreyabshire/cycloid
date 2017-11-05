#include <Wire.h>

static const float PWM_HZ = 100;

static const int PIN_PWM_CH1 = 3; // (th)
static const int PIN_PWM_CH2 = 4; // (st)

static const int PIN_ENCs[] = {5, 6, 7, 8};  // wheel encoders

#define STEERING_RC_PIN 11
#define THROTTLE_RC_PIN 12

static volatile uint8_t reg = 0;
static volatile uint8_t i2cdata[32] = {1, 0, 0};
// i2c Register Map:
static const int ADDR_CONTROL = 0x00;
// 00 - bit 0: teensy LED
//    - bit 4: disable RC passthrough (unsupported)
static const int ADDR_PWM = 0x01;
// 01 - PWM channel 1
// 02 - PWM channel 2
// 03 - reserved
// 04 - reserved
static const int ADDR_RC = 0x05;
// 05 - RC input channel 1  (unsupported yet)
// 06 - RC input channel 2  (unsupported yet)
static const int ADDR_SRV = 0x07;
// 07 - servo position voltage
static const int ADDR_ENCODER_COUNT = 0x08;
// 08 - encoder #1 count (low)
// 09 - encoder #1 count (high)
// 0a - encoder #2 count (low)
// 0b - encoder #2 count (high)
// 0c - encoder #3 count (low)
// 0d - encoder #3 count (high)
// 0e - encoder #4 count (low)
// 0f - encoder #4 count (high)
static const int ADDR_ENCODER_PERIOD = 0x10;
// 10 - encoder #1 period (low)
// 11 - encoder #1 period (high)
// 12 - encoder #2 period (low)
// 13 - encoder #2 period (high)
// 14 - encoder #3 period (low)
// 15 - encoder #3 period (high)
// 16 - encoder #4 period (low)
// 17 - encoder #4 period (high)
static const int NUM_ADDRS = 0x18;
static volatile bool dirty = true;  // set to true after a finished write call

class PulseWidthTimer {
public:
  explicit PulseWidthTimer() : start_(0), width_(0) {}
  void Change(bool value) {
    if (value)
      start_ = micros();
    else
      width_ = micros() - start_;
  }
  uint32_t Read() { return width_; }
private:
  uint32_t start_;  
  uint32_t width_;
};

PulseWidthTimer steeringTimer;
PulseWidthTimer throttleTimer;


void i2cOnReceive(int numBytes) {
  Serial.print("received: ");
  Serial.println(numBytes);
  if (Wire.available()) {
    reg = Wire.read() & (sizeof(i2cdata) - 1);
  }
  while (Wire.available()) {
    uint8_t data = Wire.read();
    i2cdata[reg] = data;
    reg = (reg+1) & (sizeof(i2cdata) - 1);
    dirty = true;
  }
}

void i2cOnRequest() {
  // this Wire API is broken and wrong. there's no way to tell how many bytes
  // the master asked for, so we have to assume the only thing you can read is
  // the entire block of data.
  Wire.write((uint8_t*) i2cdata, NUM_ADDRS);
}

void setup() {
  pinMode(PIN_PWM_CH1, OUTPUT);  // Channel 1 PWM  (ESC)
  pinMode(PIN_PWM_CH2, OUTPUT);  // Channel 2 PWM  (Servo)
  pinMode(13, OUTPUT);  // 13 is the LED
  for (int8_t i = 0; i < 4; i++) {
    pinMode(PIN_ENCs[i], INPUT);
  }

  pinMode(STEERING_RC_PIN, INPUT);
  pinMode(THROTTLE_RC_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(STEERING_RC_PIN), steeringTimerChange, CHANGE);
  attachInterrupt(digitalPinToInterrupt(THROTTLE_RC_PIN), throttleTimerChange, CHANGE);

  
  analogWriteFrequency(PIN_PWM_CH1, PWM_HZ);
  analogWriteResolution(16);

  Serial.begin(115200);
  //Serial.begin(2000000);

  Wire.begin(118);
  Wire.onReceive(i2cOnReceive);
  Wire.onRequest(i2cOnRequest);
}

uint16_t servo_pw(int8_t value) {
  // convert an int8_t -127..127 value to a 1ms..2ms pulse  (0 is 1.5ms)
  // so 65536 is 10ms
  // we want 1..1.5..2 ms
  // Since PWM is at 100Hz, 
  // 12484608 = 10*1.5*65536*127
  // 1000 / PWM_HZ = number of milliseconds for full PWM scale
  // assumed to be 10 here. 1270 was a convenient common denominator to do this
  // with integer math.
  return (12484608L + 32768L * value) / 1270;
}

int clamp(int x, int a, int b) {
  return x < a ? a : (x > b ? b : x);
}

int8_t rc_value(uint16_t pw) {
  return clamp(pw - 1500, -500, 500) >> 2;
}

static bool last_w[4] = {false, false, false, false};
static uint16_t enc_counts[4] = {0, 0, 0, 0};
static uint32_t enc_timestamps[4] = {0, 0, 0, 0};

static uint32_t rc_pw[2] = {0, 0};
static int8_t rc_values[2] = {0, 0};


void loop() {
  uint16_t servo_value = analogRead(0);  // or is it 

  bool any_changed = false;

#if 1
  {
    bool w;
    for (uint8_t i = 0; i < 4; i++) {
      w = digitalRead(PIN_ENCs[i]);
      if (w != last_w[i]) {
        any_changed = true;
        uint16_t count = enc_counts[i] + 1;
        uint32_t timestamp = micros();
        cli();  // make sure to atomically set i2cdata
        i2cdata[ADDR_ENCODER_COUNT + 2*i] = count & 255;
        i2cdata[ADDR_ENCODER_COUNT + 1 + 2*i] = count >> 8;
        if (w == 0) {  // on falling edge, also record period in us
          uint32_t dt = timestamp - enc_timestamps[i];
          if (dt > 65535) dt = 65535;  // indicate overflowed counter
          i2cdata[ADDR_ENCODER_PERIOD + 2*i] = dt & 255;
          i2cdata[ADDR_ENCODER_PERIOD + 1 + 2*i] = dt >> 8;
        }
        sei();
        enc_counts[i] = count;
        if (w == 0) {
          enc_timestamps[i] = timestamp;
        }
        last_w[i] = w;
      }
    }
  }
#endif

  noInterrupts();
  rc_pw[0] = steeringTimer.Read();
  rc_pw[1] = throttleTimer.Read();
  interrupts();
  
  for (uint8_t i = 0; i < 2; i++) {
    rc_values[i] = rc_value(rc_pw[i]);
    i2cdata[ADDR_RC + i] = rc_values[i];
  }

  #if 0
    if (any_changed) {
        Serial.print("enc: ");
        for (uint8_t i = 0; i < 4; i++) {
          Serial.print(enc_counts[i]);
          Serial.print(" ");
        }
        Serial.print("rc: ");
        for (uint8_t i = 0; i < 2; i++) {
          Serial.print(rc_pw[i]);
          Serial.print(" ");
          Serial.print(rc_values[i]);
          Serial.print(" ");
        }
        Serial.println();
    }
//        Serial.print(" dt ");
//        Serial.println(dt);
#endif

#if 1
  i2cdata[ADDR_SRV] = servo_value >> 2;  // 10 bits -> 8 bits, though the practical range may be smaller
  if (dirty) {
    digitalWrite(13, i2cdata[0] & 1);
    analogWrite(PIN_PWM_CH1, servo_pw((int8_t) i2cdata[1]));
    analogWrite(PIN_PWM_CH2, servo_pw((int8_t) i2cdata[2]));
    // reload encoder counts from i2cdata also
    for (uint8_t i = 0; i < 4; i++) {
      uint16_t count = i2cdata[ADDR_ENCODER_COUNT + 2*i]
        + ((uint16_t) i2cdata[ADDR_ENCODER_COUNT + 1 + 2*i] << 8);
      enc_counts[i] = count;
    }
    dirty = false;
  }
#endif

}

void steeringTimerChange() {
  steeringTimer.Change(digitalRead(STEERING_RC_PIN));
}

void throttleTimerChange() {
  throttleTimer.Change(digitalRead(THROTTLE_RC_PIN));
}
