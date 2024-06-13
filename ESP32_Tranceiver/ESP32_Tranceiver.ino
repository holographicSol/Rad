// Precision Geiger Counter written by Benjamin Jack Cullen

// Include the correct display library
// For a connection via I2C using Wire include
#include <Wire.h> // Only needed for Arduino 1.6.5 and earlier
#include "SSD1306Wire.h" // legacy include: `#include "SSD1306.h"`
#include "OLEDDisplayUi.h"
// #include "sova.h"
#include <math.h>
#include <stdio.h>
#include <Arduino.h>
#include <SPI.h>
#include <HardwareSerial.h>
#include <printf.h>
#include <RF24.h>
#include <OLEDDisplayUi.h>
#include <stdlib.h>

// memory limitations require counts log max.
// on esp32 a maxcount of 100 should mean main loop time will be half the time of main loop time with max count 10240.
// it may be preferrable to have a max count <=100 (cpm 100 considered unsafe to humans) if all you are interested in
// is reacting to a precise cpm reading within the shortest time you can. if instead you are actually trying to get as
// precise (arduino is not medical/military grade) a reading as you can at any level of activity then you may increase
// max count from 10240 providing you beleive there is the memory and performance available on the MCU your building for.
#define max_count       100
#define warning_level_0 99 // warn at this cpm 

#define CE_PIN     25 // radio can use tx
#define CSN_PIN    26 // radio can use rx

// on esp32 if broadcast false then precision is to approximately 40 microseconds at around 35 cpm with max_count 100.
// on esp32 if broadcast true then precision is to approximately 700 microseconds at around 35 cpm with max_count 100.
volatile bool broadcast = true;

SSD1306Wire display(0x3c, SDA, SCL);
OLEDDisplayUi ui ( &display );
RF24 radio(CE_PIN, CSN_PIN);
bool serial = false;

char inData[20]; // Allocate some space for the string
char inChar = -1; // Where to store the character read
int charIndex = 0; // Index into array; where to store the character


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
  char message[12];
};
PayloadStruct payload;

// Geiger Counter
struct GCStruct {
  double countsArray[max_count]; // stores each impulse as timestamps
  double countsArrayTemp[max_count]; // temporarily stores timestamps
  bool impulse = false; // sets true each interrupt on geiger counter pin
  bool warmup = true; // sets false after first 60 seconds have passed
  unsigned long counts; // stores counts and resets to zero every minute
  unsigned long precisionCounts = 0; // stores how many elements are in counts array
  unsigned long CPM = 0;
  char serialBuffer[12];
  float uSvh = 0; // stores the micro-Sievert/hour for units of radiation dosing
  unsigned long maxPeriod = 60; // maximum logging period in seconds.
  unsigned long countsIter;
};
GCStruct geigerCounter;

// frame to be displayed on ssd1306 182x64
void GC_Measurements(OLEDDisplay* display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  display->drawString(display->getWidth()/2, 25, "testing");
}

// this array keeps function pointers to all frames are the single views that slide in
FrameCallback frames[] = { GC_Measurements };
int frameCount = 1;

void setup() {

  // serial
  Serial.begin(2000000);
  Serial0.begin(2000000);

  // display
  display.init();
  display.flipScreenVertically();
  ui.setTargetFPS(60);
  ui.setFrames(frames, frameCount);
  display.setContrast(255);
  display.setFont(ArialMT_Plain_10);
  display.cls();
  display.println("starting..");
  ui.disableAllIndicators();

  // radio
  if (!radio.begin()) {
    Serial.println(F("radio hardware is not responding!!"));
    while (1) {}
  }
  radio.flush_rx();
  radio.flush_tx();
  radio.setPayloadSize(sizeof(payload)); // 2x int datatype occupy 8 bytes
  radio.openWritingPipe(address[1]); // always uses pipe 0
  radio.openReadingPipe(1, address[0]); // using pipe 1
  radio.stopListening();
  // configure the trancievers identically and be sure to stay legal. legal max typically 2.421 GHz in public places 
  radio.setChannel(21); // 0-124 correspond to 2.4 GHz plus the channel number in units of MHz. ch21 = 2.421 GHz
  radio.setDataRate(RF24_250KBPS); // RF24_250KBPS, RF24_1MBPS, RF24_2MBPS
  radio.setPALevel(RF24_PA_MIN); // RF24_PA_MIN, RF24_PA_LOW, RF24_PA_HIGH, RF24_PA_MAX.
  // radio.setRetries(250, 3); // uncomment this line to reduce retry time and retry attempts.
  Serial.println("Channel:  " + String(radio.getChannel()));
  Serial.println("Data Rate:" + String(radio.getDataRate()));
  Serial.println("PA Level: " + String(radio.getPALevel()));

  // attachInterrupt(17, serialISR, FALLING); // define external interrupts
}

void loop() {
  // refresh ssd1306 128x64 display
  ui.update();

  if (Serial0.available()) {

    // Serial.println(Serial0.read());

    memset(payload.message, 0, 12);
    memset(geigerCounter.serialBuffer, 0, 12);

    dtostrf(Serial0.read(), 0, 0, geigerCounter.serialBuffer);

    if (strcmp(geigerCounter.serialBuffer, "200") == 0) {memcpy(payload.message, "X", 1); payload.payloadID = 1000;}
    else { memcpy(payload.message, geigerCounter.serialBuffer, sizeof(geigerCounter.serialBuffer)); payload.payloadID = 1111;}

    radio.write(&payload, sizeof(payload));

  }
}