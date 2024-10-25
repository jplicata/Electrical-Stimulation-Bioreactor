#include "SSD1306Ascii.h"
#include "SSD1306AsciiAvrI2c.h"
#include "avr/wdt.h"  // watchdog
#include <Wire.h>
#include <Adafruit_INA219.h> // using 1 ohm instead of 0.1 ohm provided
#include <EEPROM.h>

//// Screen Setup *************//////////////
// 0X3C+SA0 - 0x3C or 0x3D
#define I2C_ADDRESS 0x3C
// Define proper RST_PIN if required.
#define RST_PIN -1
SSD1306AsciiAvrI2c oled;

// EEPROM Setup
#define phase_address 10
#define freq_address 20
#define pwidthms_address 30
unsigned long eedelay = 90000; // don't save until 90s has passed from last change
unsigned long eedelay_millis = 0;
bool eechange = false;

//button control setup//////////////////////////////
#define up_pin 5
#define down_pin 6
#define select_pin 7
//#define freq_pin 7
int select = 1; // 1=phase, 2=freq, 3=pulse

#define but_time_wait 200000.0
unsigned long but_start_wait = 0;

// Pulse setup/////////////////////////////////
unsigned long pwidthms = 10; //ms pulse width
float freq = 2;    //freq in Hz
bool biphasic = false;
unsigned long pulse_us;
unsigned long off_us;
unsigned long ontime = 0;
unsigned long offtime = 0;
bool pulse_on = false;
#define pout 9
#define led 10
// h bridge control
#define in1 11
#define in2 12
bool direct = true;

// INA219 /////////////////////////////////
Adafruit_INA219 ina219;
float busvoltage = 0;
float current_mA = 0;
#define volt_correct 1.01 // correction for resistor
int curCount = 0;
float curSum = 0;
unsigned long readWait = 800; // microsecs to wait after pulse on to read

void setup() {
  // Start Serial
  Serial.begin(115200); //115200
  Serial.println("start");
  wdt_enable(WDTO_500MS); // watchdog at 500 ms
  ina219.setCalibration_16V_400mA();
  if (! ina219.begin()) {
    Serial.println("Failed to find INA219 chip");
  }

  // Pull EEPROM Values
  EEPROM.get(phase_address, biphasic);
  EEPROM.get(freq_address, freq);
  EEPROM.get(pwidthms_address, pwidthms);

  Serial.println(biphasic);
  Serial.println(freq);
  Serial.println(pwidthms);

  //pulse setup//////////////////////////////////////////
  pulse_us = pwidthms * 1000;
  off_us = ((1.0 / freq) * 1000000) - pulse_us;
  pinMode(pout, OUTPUT); // pulse pin out
  pinMode(10, OUTPUT); // testing
  digitalWrite(10, HIGH);
  // h bridge
  pinMode(in1, OUTPUT);
  pinMode(in2, OUTPUT);
  digitalWrite(in1, LOW);
  digitalWrite(in2, HIGH);

  // Screen Setup/////////////////////////////////////////////
#if RST_PIN >= 0
  oled.begin(&Adafruit128x64, I2C_ADDRESS, RST_PIN);
#else // RST_PIN >= 0
  oled.begin(&Adafruit128x64, I2C_ADDRESS);
#endif // RST_PIN >= 0
  // Call oled.setI2cClock(frequency) to change from the default frequency.
  oled.setFont(System5x7);
  oled.clear();
  oled.println(F("Pulse Generator"));
  oled.setCursor(1, 1);
  oled.print(F(">"));
  oled.setCursor(7, 1);
  if (!biphasic) {
    oled.print(F("Monophasic"));
  }
  else {
    oled.print(F("Biphasic"));
  }
  oled.setCursor(7, 2);
  oled.print(F("Freq   (Hz): ")); // 75, 2 for Hz
  oled.setCursor(82, 2);
  oled.print(freq);
  oled.setCursor(7, 3);
  oled.print(F("Pulse  (ms): ")); //75, 3 for pulse
  oled.setCursor(82, 3);
  oled.print(pwidthms);
  oled.setCursor(1, 4);
  oled.print(F("Load    (V): ")); // 75, 4 for V
  oled.setCursor(1, 5);
  oled.print(F("Current(mA): ")); // 75, 5 for mA

  // button control/////////////////////////////////////
  pinMode(up_pin, INPUT_PULLUP);
  pinMode(down_pin, INPUT_PULLUP);
  pinMode(select_pin, INPUT_PULLUP);
  //pinMode(freq_pin, INPUT_PULLUP);
}

void loop() {
  wdt_reset();// watchdog reset

  microsOverflow();  // check for overflow of micros()

  pulse();  // control stim pulse, monitor with INA219 and screen updates

  buttonControl();  // listen for button inputs

  eepromSave(); //
}

void dirControl(bool dir) {
  if (dir) {
    //Serial.println("dir1");
    digitalWrite(in1, HIGH);
    digitalWrite(in2, LOW);
  }
  else {
    digitalWrite(in1, LOW);
    digitalWrite(in2, HIGH);
  }
}

void buttonControl() {
  if (micros() - but_start_wait > but_time_wait) { // check if done waiting
    // select
    if (digitalRead(select_pin) == LOW) {
      oled.setCursor(1, select);// clear old marker
      oled.print(F(" "));
      select = select + 1;
      if (select > 3) {
        select = 1;
      }
      // do screen change
      oled.setCursor(1, select);
      oled.print(F(">"));
      Serial.println(select);
      but_start_wait = micros();
    }
    //up
    if (digitalRead(up_pin) == LOW) { // check up pin
      eechange = true; // mark to update eeprom
      eedelay_millis = millis();

      if (select == 1) {
        if (!biphasic) {
          biphasic = true;
          oled.setCursor(7, 1);
          oled.print(F("Biphasic     "));
        }
        else {
          biphasic = false;
          oled.setCursor(7, 1);
          oled.print(F("Monophasic   "));
        }
        but_start_wait = micros();
      }
      else if (select == 2) {
        freq = freq + 0.1;
        freqUpdate();
      }
      else if (select == 3) {
        pwidthms++;
        pulseUpdate();
      }
    }
    //down
    if (digitalRead(down_pin) == LOW) { // check down pin
      eechange = true; // mark to update eeprom
      eedelay_millis = millis();

      if (select == 1) {
        // do phase setting change
        if (!biphasic) {
          biphasic = true;
          oled.setCursor(7, 1);
          oled.print(F("Biphasic     "));
        }
        else {
          biphasic = false;
          oled.setCursor(7, 1);
          oled.print(F("Monophasic   "));
        }
        but_start_wait = micros();
      }
      else if (select == 2) {
        freq = freq - 0.1;
        freqUpdate();
      }
      else if (select == 3) {
        pwidthms--;
        pulseUpdate();
      }
    }
  }
}

void pulse() {
  if (pulse_on) {
    if (direct) {   /// measure and keep track of avg volt and current for each pulse
      if (micros() - ontime >= readWait) {
        busvoltage = (ina219.getBusVoltage_V() * volt_correct);
        current_mA = ina219.getCurrent_mA() / 10.0; // 1/10 for 1 ohm shunt
        curSum = curSum + current_mA;
        curCount++;
      }
    }
    if (!biphasic && micros() - ontime >= pulse_us) {
      digitalWrite(pout, LOW);
      digitalWrite(led, LOW);
      offtime = micros();
      pulse_on = false;
      oled.setCursor(75, 4);
      oled.print(busvoltage);
      oled.print(F("   "));
      oled.setCursor(75, 5);
      oled.print(curSum / curCount);
      oled.print(F("   "));
      curSum = 0;
      curCount = 0;
    }
    else if (biphasic && micros() - ontime >= pulse_us && direct) { // change direction after 1 pulse width
      direct = false;
      dirControl(direct);
      offtime = micros();
    }
    else if (biphasic && micros() - ontime >= (pulse_us * 2)) {
      digitalWrite(pout, LOW);
      digitalWrite(led, LOW);
      pulse_on = false;
      oled.setCursor(75, 4);
      oled.print(busvoltage);
      oled.print(F("   "));
      oled.setCursor(75, 5);
      oled.print(curSum / curCount);
      oled.print(F("   "));
      curSum = 0;
      curCount = 0;
    }
  }
  else if (!pulse_on) {
    if (micros() - offtime >= off_us) {
      direct = true;
      dirControl(direct);
      digitalWrite(pout, HIGH);
      digitalWrite(led, HIGH);
      pulse_on = true;
      ontime = micros();
    }
  }
}

void microsOverflow() {
  if (micros() < ontime) {
    ontime = 0;
    offtime = 0;
    but_start_wait = 0;
    Serial.println("micros");
  }
}

void freqUpdate() {
  off_us = ((1.0 / freq) * 1000000) - pulse_us;
  oled.setCursor(82, 2);
  oled.print(freq);
  oled.print(F("   "));
  but_start_wait = micros();
}

void pulseUpdate() {
  pulse_us = pwidthms * 1000; // recalculate
  off_us = ((1.0 / freq) * 1000000) - pulse_us;
  oled.setCursor(82, 3);
  oled.print(pwidthms);
  oled.print(F("   "));
  but_start_wait = micros();
}

void eepromSave() {
  if (eechange && (millis() - eedelay > eedelay_millis)) {
    eechange = false;
    //    Serial.println("saving");
    //    Serial.println(biphasic);
    //    Serial.println(freq);
    //    Serial.println(pwidthms);
    EEPROM.put(phase_address, biphasic);
    EEPROM.put(freq_address, freq);
    EEPROM.put(pwidthms_address, pwidthms);
  }
}
