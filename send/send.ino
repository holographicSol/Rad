// Precision Geiger Counter written by Benjamin Jack Cullen

// Include the correct display library
// For a connection via I2C using Wire include
#include <Wire.h>         // Only needed for Arduino 1.6.5 and earlier
#include "SSD1306Wire.h"  // legacy include: `#include "SSD1306.h"`
#include <math.h>
#include <stdio.h>
#include <Arduino.h>
#include <SPI.h>
#include <HardwareSerial.h>
#include "printf.h"
#include "RF24.h"
#include "OLEDDisplayUi.h"

#define max_count 10240  // memory limitations require counts log max (todo: increase max) HAS TO BE THE SAME VALUE AS GCTIME AND GCTTIME!
#define CE_PIN 25        // radio can use tx
#define CSN_PIN 26       // radio can use rx
#define GEIGER_PIN 27

RF24 radio(CE_PIN, CSN_PIN);
SSD1306Wire display(0x3c, SDA, SCL);
OLEDDisplayUi ui(&display);

// Radio Addresses
uint64_t address[6] = { 0x7878787878LL,
                        0xB3B4B5B6F1LL,
                        0xB3B4B5B6CDLL,
                        0xB3B4B5B6A3LL,
                        0xB3B4B5B60FLL,
                        0xB3B4B5B605LL };


// Transmission Payload
struct PayloadStruct {
  unsigned long nodeID;
  unsigned long payloadID;
  char message[10];
};
PayloadStruct payload;


// Geiger Counter
struct GCStruct {
  int countsArray[10240];             // stores each impulse as micros
  int countsArrayTemp[10240];         // temporarily stores micros from countsArray that have not yet expired
  bool impulse = false;               // sets true each interrupt on geiger counter pin
  bool warmup = true;                 // sets false after first 60 seconds have passed
  unsigned long counts;               // stores counts and resets to zero every minute
  unsigned long precisionCounts = 0;  // stores how many elements are in counts array
  unsigned long precisionCPM = 0;     // stores cpm value according to precisionCounts (should always be equal to precisionCounts because we are not estimating)
  char precisionCPM_str[12];
  float precisioncUSVH = 0;                       // stores the micro-Sievert/hour for units of radiation dosing
  unsigned long maxPeriod = 60000000;             //Maximum logging period in microseconds. Should always be 60,000,000 for one minute
  unsigned long currentMicrosMain;                // stores current
  unsigned long previousMicrosMain;               // stores previous
  unsigned long precisionMicros;                  // stores main loop time
  unsigned long currentMicrosStateTransmission;   // stores current
  unsigned long previousMicrosStateTransmission;  // stores previous
  char precisionMicros_str[12];                   // stores main loop time
};
GCStruct geigerCounter;


// subprocedure for capturing events from Geiger Kit
void tube_impulse() {
  geigerCounter.counts++;
  geigerCounter.impulse = true;
}

// frame to be displayed on ssd1306 182x64
void GC_Measurements(OLEDDisplay* display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  if (geigerCounter.warmup == true) {
    display->drawString(0, 0, "Precision: pending");
  } else {
    display->drawString(0, 0, "Precision: " + String(geigerCounter.precisionMicros));
  }
  display->drawString(0, 15, "CPM:    " + String(geigerCounter.precisionCPM));
  display->drawString(0, 25, "uSv/h:  " + String(geigerCounter.precisioncUSVH));
  display->drawString(0, 35, "Epoch: " + String(geigerCounter.maxPeriod - (geigerCounter.currentMicrosMain - geigerCounter.previousMicrosMain)));
}

// this array keeps function pointers to all frames are the single views that slide in
FrameCallback frames[] = { GC_Measurements };
int frameCount = 1;

void setup() {

  // serial
  Serial.begin(115200);

  // display
  display.init();
  display.flipScreenVertically();
  ui.setTargetFPS(60);
  ui.setFrames(frames, frameCount);
  display.setContrast(255);
  display.setFont(ArialMT_Plain_10);
  display.cls();
  display.println("starting..");

  // system
  Serial.print("XTAL Crystal Frequency: ");
  Serial.print(getXtalFrequencyMhz());
  Serial.println(" MHz");
  Serial.print("CPU Frequency: ");
  Serial.print(getCpuFrequencyMhz());
  Serial.println(" MHz");
  Serial.print("APB Bus Frequency: ");
  Serial.print(getApbFrequency());
  Serial.println(" Hz");

  // radio
  if (!radio.begin()) {
    Serial.println(F("radio hardware is not responding!!"));
    while (1) {}
  }
  radio.flush_rx();
  radio.flush_tx();
  radio.setPALevel(RF24_PA_LOW);          // RF24_PA_MAX is default.
  radio.setPayloadSize(sizeof(payload));  // 2x int datatype occupy 8 bytes
  Serial.println("Channel:  " + String(radio.getChannel()));
  Serial.println("Data Rate:" + String(radio.getDataRate()));
  Serial.println("PA Level: " + String(radio.getPALevel()));
  radio.openWritingPipe(address[1]);     // always uses pipe 0
  radio.openReadingPipe(1, address[0]);  // using pipe 1
  radio.stopListening();

  // attach geiger counter pin to interrupts last
  attachInterrupt(GEIGER_PIN, tube_impulse, FALLING);  //define external interrupts
}

void loop() {

  // store time started
  geigerCounter.currentMicrosMain = micros();

  // reset counts every minute (60 million micro seconds)
  if (geigerCounter.currentMicrosMain - geigerCounter.previousMicrosMain > geigerCounter.maxPeriod) {
    geigerCounter.previousMicrosMain = geigerCounter.currentMicrosMain;
    geigerCounter.counts = 0;      // resets every 60 seconds
    geigerCounter.warmup = false;  // completed 60 second warmup required for precision
  }

  // refresh ssd1306 128x64 display
  ui.update();

  // if input add current micros to geigerCounter so that we can remove it when max period has expired
  if (geigerCounter.impulse == true) {
    geigerCounter.impulse = false;
    geigerCounter.countsArray[geigerCounter.counts] = micros();  // add count to array as micros
    // transmit counts seperately so that the receiver(s) can behave like the actual geiger counter
    memset(payload.message, 0, 12);
    memcpy(payload.message, "X", 1);
    payload.payloadID = 1000;
    radio.write(&payload, sizeof(payload));
  }

  // precision guage relies on a 60s warmup after which we can begin removing expired counts from the array
  geigerCounter.precisionCounts = 0;  // reset precision counter
  memset(geigerCounter.countsArrayTemp, 0, sizeof(geigerCounter.countsArrayTemp));
  for (int i = 0; i < max_count; i++) {
    if (geigerCounter.countsArray[i] >= 1) {                                                                          // only entertain non zero elements
      if ((micros() - geigerCounter.countsArray[i]) > geigerCounter.maxPeriod) { geigerCounter.countsArray[i] = 0; }  // set expired counters to zero
      else {
        geigerCounter.precisionCounts++;                                  // non expired counters increment the precision counter
        geigerCounter.countsArrayTemp[i] = geigerCounter.countsArray[i];  // non expired counters go into the new temporary array
      }
    }
  }
  memset(geigerCounter.countsArray, 0, sizeof(geigerCounter.countsArray));
  memcpy(geigerCounter.countsArray, geigerCounter.countsArrayTemp, sizeof(geigerCounter.countsArray));

  // then calculate usv/h
  geigerCounter.precisionCPM = geigerCounter.precisionCounts;
  geigerCounter.precisioncUSVH = geigerCounter.precisionCPM * 0.00332;

  // store taken to complete
  geigerCounter.precisionMicros = micros() - geigerCounter.currentMicrosMain;

  // transmit the resultss
  memset(payload.message, 0, 12);
  memset(geigerCounter.precisionCPM_str, 0, 12);
  dtostrf(geigerCounter.precisionCPM, 0, 4, geigerCounter.precisionCPM_str);
  memcpy(payload.message, geigerCounter.precisionCPM_str, sizeof(geigerCounter.precisionCPM_str));
  payload.payloadID = 1111;
  radio.write(&payload, sizeof(payload));
}
