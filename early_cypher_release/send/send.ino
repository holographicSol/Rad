// Rad Sender written by Benjamin Jack Cullen
// Collect, display and send sensor data to a remote device.

// ----------------------------------------------------------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <printf.h>
#include <math.h>
#include <SPI.h>
#include <Arduino.h>
#include <Wire.h>
#include <RF24.h>
#include <SSD1306Wire.h>
#include <OLEDDisplayUi.h>
#include <AESLib.h>

// ----------------------------------------------------------------------------------------------------------------------------

// memory limitations require counts log max.
// on esp32 a maxcount of 100 should mean main loop time will be half the time of main loop time with max count 10240.
// it may be preferrable to have a max count <=100 (cpm 100 considered unsafe to humans) if all you are interested in
// is reacting to a precise cpm reading within the shortest time you can. if instead you are actually trying to get as
// precise (arduino is not medical/military grade) a reading as you can at any level of activity then you may increase
// max count from 10240 providing you beleive there is the memory and performance available on the MCU your building for.
#define max_count       100
#define warning_level_0  99 // warn at this cpm 
#define CE_PIN           25 // radio can use tx
#define CSN_PIN          26 // radio can use rx
#define GEIGER_PIN       27

// ----------------------------------------------------------------------------------------------------------------------------

RF24 radio(CE_PIN, CSN_PIN);
SSD1306Wire display(0x3c, SDA, SCL);
OLEDDisplayUi ui(&display);
AESLib aesLib;

// ----------------------------------------------------------------------------------------------------------------------------

// on esp32 if broadcast false then precision is to approximately 40 microseconds at around 35 cpm with max_count 100.
// on esp32 if broadcast true then precision is to approximately 700 microseconds at around 35 cpm with max_count 100.
volatile bool broadcast = true;
// Radio Addresses
uint8_t address[][6] = { "0Node", "1Node", "2Node", "3Node", "4Node", "5Node" };

// ----------------------------------------------------------------------------------------------------------------------------

struct AESStruct {
  byte aes_key[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; // AES encryption key (use your own)
  byte aes_iv[16]  = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; // genreral initialization vector (use your own)
  byte enc_iv[16]  = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; // iv_block gets written to
  byte dec_iv[16]  = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; // iv_block gets written to
  char cleartext[256] = {0};
  char ciphertext[512] = {0};
  char credentials[16];
  char tmp_cleartext[256];
  uint16_t plain_len;
  uint16_t msgLen;
};
AESStruct aes;

void aes_init() {
  aesLib.gen_iv(aes.aes_iv);
  aesLib.set_paddingmode((paddingMode)0);
}
void encrypt(char * msg, byte iv[]) {
  aes.msgLen = strlen(msg);
  memset(aes.ciphertext, 0, sizeof(aes.ciphertext));
  aesLib.encrypt64((const byte*)msg, aes.msgLen, aes.ciphertext, aes.aes_key, sizeof(aes.aes_key), iv);
}
void decrypt(char * msg, byte iv[]) {
  aes.msgLen = strlen(msg);
  memset(aes.cleartext, 0, sizeof(aes.cleartext));
  char tmp_cleartext[256];
  aes.plain_len = aesLib.decrypt64(msg, aes.msgLen, (byte*)aes.tmp_cleartext, aes.aes_key, sizeof(aes.aes_key), iv);
  strncpy(aes.cleartext, aes.tmp_cleartext, aes.plain_len);
}

// ----------------------------------------------------------------------------------------------------------------------------

struct CommandServerStruct {
  char messageCommand[16];
  char messageValue[16];
};
CommandServerStruct commandserver;

// ----------------------------------------------------------------------------------------------------------------------------

struct PayloadStruct {
  unsigned long nodeID;
  unsigned long payloadID;
  char message[1000];
};
PayloadStruct payload;

// ----------------------------------------------------------------------------------------------------------------------------

// Geiger Counter
struct GCStruct {
  double countsArray[max_count]; // stores each impulse as timestamps
  double countsArrayTemp[max_count]; // temporarily stores timestamps
  bool impulse = false; // sets true each interrupt on geiger counter pin
  bool warmup = true; // sets false after first 60 seconds have passed
  unsigned long counts; // stores counts and resets to zero every minute
  unsigned long precisionCounts; // stores how many elements are in counts array
  unsigned long CPM;
  unsigned long previousCPM;
  char CPM_str[16];
  float uSvh = 0; // stores the micro-Sievert/hour for units of radiation dosing
  unsigned long maxPeriod = 60; // maximum logging period in seconds.
  unsigned long countsIter;
};
GCStruct geigerCounter;

// ----------------------------------------------------------------------------------------------------------------------------

struct TimeStruct {
  double previousTimestamp; // a placeholder for a previous time (optionally used)
  unsigned long mainLoopTimeTaken; // necessary to count time less than a second (must be updated every loop of main)
  unsigned long mainLoopTimeStart; // necessary for loop time taken (must be recorded every loop of main)
  double subTime;
  double subTimeDivided;
  double interTimeDivided;
  unsigned long currentMilliSecond;
  unsigned long currentSecond;
  double timestamp;
  double interTime;
  double previousTimestampSecond; // a placeholder for a previous time (optionally used)
};
TimeStruct timeData;

// ----------------------------------------------------------------------------------------------------------------------------

// frame to be displayed on ssd1306 182x64
void GC_Measurements(OLEDDisplay* display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  if (geigerCounter.CPM >= 99) { display->drawString(display->getWidth()/2, 0, "WARNING");}
  else {display->drawString(display->getWidth()/2, 0, String(timeData.mainLoopTimeTaken));}
  display->drawString(display->getWidth()/2, 25, "cpm");
  display->drawString(display->getWidth()/2, 13, String(geigerCounter.CPM));
  display->drawString(display->getWidth()/2, display->getHeight()-10, "uSv/h");
  display->drawString(display->getWidth()/2, display->getHeight()-22, String(geigerCounter.uSvh));
}
// this array keeps function pointers to all frames are the single views that slide in
FrameCallback frames[] = { GC_Measurements };
int frameCount = 1;

// ----------------------------------------------------------------------------------------------------------------------------

double currentTime() {
  if (timeData.subTime >= 1000) {
      timeData.subTime = 0;
      timeData.currentMilliSecond++;
      if (timeData.currentMilliSecond >= 1000) {
          timeData.currentMilliSecond = 0;
          timeData.currentSecond++;
      }
  }
  timeData.subTime += (timeData.mainLoopTimeTaken);
  timeData.subTimeDivided = timeData.subTime / 1000000;
  return timeData.currentSecond+timeData.subTimeDivided;
}

// ----------------------------------------------------------------------------------------------------------------------------

double interCurrentTime() {
  timeData.interTime = (micros() - timeData.mainLoopTimeStart);
  timeData.interTimeDivided = timeData.interTime / 1000000;
  return timeData.timestamp+timeData.interTimeDivided;
}

// ----------------------------------------------------------------------------------------------------------------------------

void tubeImpulseISR() {
  geigerCounter.impulse = true;
  if (geigerCounter.countsIter < max_count) {geigerCounter.countsIter++;}
  else {geigerCounter.countsIter=0;}
  geigerCounter.countsArray[geigerCounter.countsIter] = timeData.timestamp;
}

// ----------------------------------------------------------------------------------------------------------------------------

void cipherSend() {
  Serial.println("---------------------------------------------------------------------------");

  if (payload.payloadID > 1000) {payload.payloadID = 0}

  // display raw payload
  Serial.print("[NodeID]                 "); Serial.println(payload.nodeID);
  Serial.print("[payload.payloadID]      "); Serial.println(payload.payloadID);
  Serial.print("[aes.cleartext]          "); Serial.println(aes.cleartext); 
  Serial.print("[Bytes(aes.cleartext)]   "); Serial.println(strlen(aes.cleartext));

  // encrypt and load the encrypted data into the payload
  encrypt((char*)aes.cleartext, aes.enc_iv);
  memset(payload.message, 0, sizeof(payload.message));
  memcpy(payload.message, aes.ciphertext, sizeof(aes.ciphertext));

  // display payload information after encryption
  Serial.print("[payload.message]        "); Serial.println(payload.message); 
  Serial.print("[Bytes(aes.ciphertext)]  "); Serial.println(strlen(aes.ciphertext));

  // send
  Serial.print("[Bytes(payload.message)] "); Serial.println(strlen(payload.message));
  radio.write(&payload, sizeof(payload));
  payload.payloadID++;
  
  // uncomment to test immediate replay attack
  // delay(1000);
  // radio.write(&payload, sizeof(payload));
  
}

// ----------------------------------------------------------------------------------------------------------------------------

void setup() {

  // ------------------------------------------------------------

  // setup serial
  Serial.begin(115200);

  // ------------------------------------------------------------

  // setup display
  display.init();
  ui.setTargetFPS(60);
  ui.disableAllIndicators();
  ui.setFrames(frames, frameCount);
  display.flipScreenVertically();
  display.setContrast(255);
  display.setFont(ArialMT_Plain_10);
  display.cls();
  display.println("starting..");

  // ------------------------------------------------------------

  // setup aes
  aes_init();
  strcpy(aes.credentials, "user:pass:");

  // ------------------------------------------------------------

  // setup radio
  if (!radio.begin()) {
    Serial.println(F("radio hardware is not responding!!"));
    while (1) {}
  }
  radio.flush_rx();
  radio.flush_tx();
  radio.setPayloadSize(sizeof(payload)); // 2x int datatype occupy 8 bytes
  radio.openWritingPipe(address[0][0]); // always uses pipe 0
  radio.openReadingPipe(1, address[0][1]); // using pipe 1
  radio.setChannel(124);          // 0-124 correspond to 2.4 GHz plus the channel number in units of MHz (ch 21 = 2.421 GHz)
  radio.setDataRate(RF24_2MBPS);  // RF24_250KBPS, RF24_1MBPS, RF24_2MBPS
  radio.setPALevel(RF24_PA_HIGH); // RF24_PA_MIN, RF24_PA_LOW, RF24_PA_HIGH, RF24_PA_MAX.
  payload.nodeID = address[0][1];
  Serial.println("Channel:  " + String(radio.getChannel()));
  Serial.println("Data Rate:" + String(radio.getDataRate()));
  Serial.println("PA Level: " + String(radio.getPALevel()));

  // ------------------------------------------------------------

  // setup interrupts
  attachInterrupt(GEIGER_PIN, tubeImpulseISR, FALLING);

  // ------------------------------------------------------------
}

// ----------------------------------------------------------------------------------------------------------------------

void loop() {

  // ------------------------------------------------------------

  // store current time to measure this loop time so we know how quickly items are added/removed from counts arrays
  timeData.mainLoopTimeStart = micros();

  // set current timestamp to be used this loop same millisecond+- depending on loop speed.
  timeData.timestamp = currentTime();
  
  // set previous time each minute
  if ((timeData.timestamp - timeData.previousTimestamp) > geigerCounter.maxPeriod) {
    Serial.print("cycle expired: "); Serial.println(timeData.timestamp, sizeof(timeData.timestamp));
    timeData.previousTimestamp = timeData.timestamp;
    geigerCounter.warmup = false;
  }

  // ------------------------------------------------------------

  // check if impulse
  if (geigerCounter.impulse == true) {
    geigerCounter.impulse = false;
    if (broadcast == true) {
      // create transmission message
      memset(aes.cleartext, 0, sizeof(aes.cleartext));
      strcat(aes.cleartext, aes.credentials);
      strcat(aes.cleartext, "IMP");
      // encrypt and send
      cipherSend();
    }
  }

  // ------------------------------------------------------------
  
  // step through the array and remove expired impulses by exluding them from our new array.
  geigerCounter.precisionCounts = 0;
  memset(geigerCounter.countsArrayTemp, 0, sizeof(geigerCounter.countsArrayTemp));
  for (int i = 0; i < max_count; i++) {
    if (geigerCounter.countsArray[i] >= 1) { // only entertain non zero elements
    
      // compare current timestamp (per loop) to timestamps in array
      if (((timeData.timestamp - (geigerCounter.countsArray[i])) > geigerCounter.maxPeriod)) {
        geigerCounter.countsArray[i] = 0;
        }
      else {
        geigerCounter.precisionCounts++; // non expired counters increment the precision counter
        geigerCounter.countsArrayTemp[i] = geigerCounter.countsArray[i]; // store non expired
      }
    }
  }
  memset(geigerCounter.countsArray, 0, sizeof(geigerCounter.countsArray));
  memcpy(geigerCounter.countsArray, geigerCounter.countsArrayTemp, sizeof(geigerCounter.countsArray));

  // then calculate usv/h
  geigerCounter.CPM = geigerCounter.precisionCounts;
  geigerCounter.uSvh = geigerCounter.CPM * 0.00332;

  // ------------------------------------------------------------

  if (broadcast == true) {
    // todo: broadcast periodically outside of value change
    if (geigerCounter.CPM != geigerCounter.previousCPM) {
      geigerCounter.previousCPM = geigerCounter.CPM;
      timeData.previousTimestampSecond = timeData.timestamp;
      // create transmission message
      memset(geigerCounter.CPM_str, 0, sizeof(geigerCounter.CPM_str));
      dtostrf(geigerCounter.CPM, 0, 0, geigerCounter.CPM_str);
      memset(aes.cleartext, 0, sizeof(aes.cleartext));
      strcat(aes.cleartext, aes.credentials);
      strcat(aes.cleartext, "CPM");
      strcat(aes.cleartext, geigerCounter.CPM_str);
      // encrypt and send
      cipherSend();
    }
  }

  // ------------------------------------------------------------

  // refresh ssd1306 128x64 display
  ui.update();

  // store time taken to complete
  timeData.mainLoopTimeTaken = micros() - timeData.mainLoopTimeStart;
}