// Precision Geiger Counter written by Benjamin Jack Cullen

// Include the correct display library
// For a connection via I2C using Wire include
#include <Wire.h> // Only needed for Arduino 1.6.5 and earlier
#include <SSD1306Wire.h> // legacy include: `#include "SSD1306.h"`
#include <math.h>
#include <stdio.h>
#include <Arduino.h>
#include <SPI.h>
#include <HardwareSerial.h>
#include <printf.h>
#include <RF24.h>
#include <OLEDDisplayUi.h>
#include <stdlib.h>

// ----------------------------------------------------------------------------------------------------------------------
#include "AESLib.h"
#define BAUD 9600
AESLib aesLib;
#define INPUT_BUFFER_LIMIT (128 + 1) // designed for Arduino UNO, not stress-tested anymore (this works with message[129])
unsigned char cleartext[INPUT_BUFFER_LIMIT] = {0}; // THIS IS INPUT BUFFER (FOR TEXT)
unsigned char ciphertext[2*INPUT_BUFFER_LIMIT] = {0}; // THIS IS OUTPUT BUFFER (FOR BASE64-ENCODED ENCRYPTED DATA)
char message[56];
char credentials[18];
// AES Encryption Key (CHANGME)
byte aes_key[] = { 0x2B, 0x7E, 0x15, 0x16, 0x28, 0xAE, 0xD2, 0xA6, 0xAB, 0xF7, 0x15, 0x88, 0x09, 0xCF, 0x4F, 0x3C };
// General initialization vector (CHANGE) (you must use your own IV's in production for full security!!!)
byte aes_iv[N_BLOCK] = { 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA };

// Generate IV (once)
void aes_init() {
  aesLib.gen_iv(aes_iv);
  aesLib.set_paddingmode((paddingMode)0);
}
uint16_t encrypt_to_ciphertext(char * msg, uint16_t msgLen, byte iv[]) {
  // Serial.println("Calling encrypt (string)...");
  // aesLib.get_cipher64_length(msgLen);
  int cipherlength = aesLib.encrypt((byte*)msg, msgLen, (byte*)ciphertext, aes_key, sizeof(aes_key), iv);
                   // uint16_t encrypt(byte input[], uint16_t input_length, char * output, byte key[],int bits, byte my_iv[]);
  return cipherlength;
}
uint16_t decrypt_to_cleartext(byte msg[], uint16_t msgLen, byte iv[]) {
  // Serial.print("Calling decrypt...; ");
  uint16_t dec_bytes = aesLib.decrypt(msg, msgLen, (byte*)cleartext, aes_key, sizeof(aes_key), iv);
  // Serial.print("Decrypted bytes: "); Serial.println(dec_bytes);
  return dec_bytes;
}
/* non-blocking wait function */
void wait(unsigned long milliseconds) {
  unsigned long timeout = millis() + milliseconds;
  while (millis() < timeout) {
    yield();
  }
}
unsigned long loopcount = 0;
// Working IV buffer: Will be updated after encryption to follow up on next block.
// But we don't want/need that in this test, so we'll copy this over with enc_iv_to/enc_iv_from
// in each loop to keep the test at IV iteration 1. We could go further, but we'll get back to that later when needed.
// General initialization vector (same as in node-js example) (you must use your own IV's in production for full security!!!)
byte enc_iv[N_BLOCK] =      { 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA };
byte enc_iv_to[N_BLOCK]   = { 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA };
byte enc_iv_from[N_BLOCK] = { 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA };
// ----------------------------------------------------------------------------------------------------------------------

// memory limitations require counts log max.
// on esp32 a maxcount of 100 should mean main loop time will be half the time of main loop time with max count 10240.
// it may be preferrable to have a max count <=100 (cpm 100 considered unsafe to humans) if all you are interested in
// is reacting to a precise cpm reading within the shortest time you can. if instead you are actually trying to get as
// precise (arduino is not medical/military grade) a reading as you can at any level of activity then you may increase
// max count from 10240 providing you beleive there is the memory and performance available on the MCU your building for.
#define max_count        100
#define warning_level_0   99   // warn at this cpm
#define syncIntervalCPM    1   // how often transmit values outside of values being transmitted if values change

#define CE_PIN     25 // radio can use tx
#define CSN_PIN    26 // radio can use rx
#define GEIGER_PIN 27

// on esp32 if broadcast false then precision is to approximately 40 microseconds at around 35 cpm with max_count 100.
// on esp32 if broadcast true then precision is to approximately 700 microseconds at around 35 cpm with max_count 100.
volatile bool broadcast = true;

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

#define maxPayloadSize 1000
struct PayloadStruct {
  unsigned long nodeID;
  unsigned long payloadID;
  unsigned char message[maxPayloadSize];
  // unsigned char message[2*INPUT_BUFFER_LIMIT] = {0};
};
PayloadStruct payload;

#define maxCPM_StrSize 10
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
  char CPM_str[maxCPM_StrSize];
  float uSvh = 0; // stores the micro-Sievert/hour for units of radiation dosing
  unsigned long maxPeriod = 60; // maximum logging period in seconds.
  unsigned long countsIter;
};
GCStruct geigerCounter;

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
  double previousTimestampSyncCPM; // a placeholder for a previous time (optionally used)
};
TimeStruct timeData;

// create a timestamp
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

// create an intermediary timestamp
double interCurrentTime() {
  timeData.interTime = (micros() - timeData.mainLoopTimeStart);
  timeData.interTimeDivided = timeData.interTime / 1000000;
  return timeData.timestamp+timeData.interTimeDivided;
}

// subprocedure for capturing events from Geiger Kit
void tubeImpulseISR() {
  geigerCounter.impulse = true;
  if (geigerCounter.countsIter < max_count) {geigerCounter.countsIter++;}
  else {geigerCounter.countsIter=0;}

  // add current timestamp (per loop) to timestamps in array
  geigerCounter.countsArray[geigerCounter.countsIter] = timeData.timestamp;

  // compare current (unique) timestamp to timestamps in array
  // geigerCounter.countsArray[geigerCounter.countsIter] = interCurrentTime();
}

void BGTubeImpulseISR() {
  geigerCounter.counts++;
  geigerCounter.impulse = true;
}

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
  ui.disableAllIndicators();

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

  // default credentials
  strcpy(credentials, "uname:pass:");

  // radio
  if (!radio.begin()) {
    Serial.println(F("radio hardware is not responding!!"));
    while (1) {}
  }
  radio.flush_rx();
  radio.flush_tx();
  radio.setPayloadSize(sizeof(payload)); // 2x int datatype occupy 8 bytes
  radio.openWritingPipe(address[0]); // always uses pipe 0
  radio.openReadingPipe(1, address[0]); // using pipe 1
  radio.stopListening();
  // configure the trancievers identically and be sure to stay legal. legal max typically 2.421 GHz in public places 
  radio.setChannel(124); // 0-124 correspond to 2.4 GHz plus the channel number in units of MHz. ch21 = 2.421 GHz
  radio.setDataRate(RF24_2MBPS); // RF24_250KBPS, RF24_1MBPS, RF24_2MBPS
  radio.setPALevel(RF24_PA_HIGH); // RF24_PA_MIN, RF24_PA_LOW, RF24_PA_HIGH, RF24_PA_MAX.
  // radio.setRetries(250, 3); // uncomment this line to reduce retry time and retry attempts.
  Serial.println("Channel:  " + String(radio.getChannel()));
  Serial.println("Data Rate:" + String(radio.getDataRate()));
  Serial.println("PA Level: " + String(radio.getPALevel()));

  attachInterrupt(GEIGER_PIN, tubeImpulseISR, FALLING); // define external interrupts
}

void cipherSend() {
  // ----------------------------------------------------------------------------------------------------------------------
  // Serial.println("---------------------------------------------------------------------------");
  // Serial.print("message length: "); Serial.println(sizeof(message));
  sprintf((char*)cleartext, "%s", message); // must not exceed INPUT_BUFFER_LIMIT bytes; may contain a newline
  // iv_block gets written to, provide own fresh copy... so each iteration of encryption will be the same.
  uint16_t msgLen = sizeof(message);
  memcpy(enc_iv, enc_iv_to, sizeof(enc_iv_to));
  uint16_t encLen = encrypt_to_ciphertext((char*)cleartext, msgLen, enc_iv);
  // Serial.print("Encrypted length = "); Serial.println(encLen );
  // ----------------------------------------------------------------------------------------------------------------------
  // unsigned char base64decoded[50] = {0};
  // // base64_decode((char*)base64decoded, (char*)ciphertext, encLen);
  // base64_decode((char*)base64decoded, (char*)ciphertext, 32);
  // memcpy(enc_iv, enc_iv_from, sizeof(enc_iv_from));
  // uint16_t decLen = decrypt_to_cleartext(base64decoded, strlen((char*)base64decoded), enc_iv);
  // Serial.print("Decrypted cleartext of length: "); Serial.println(decLen);
  // Serial.print("Decrypted cleartext: "); Serial.println((char*)cleartext);
  // ----------------------------------------------------------------------------------------------------------------------
  memset(payload.message, 0, maxPayloadSize);
  memcpy(payload.message, ciphertext, sizeof(ciphertext));
  payload.payloadID = 1000;
  // Serial.print(String("[ID ") + String(payload.payloadID) + "] "); Serial.print("message: "); Serial.println((char*)payload.message);
  // ----------------------------------------------------------------------------------------------------------------------
  // unsigned char base64decoded2[50] = {0};
  // // base64_decode((char*)base64decoded2, (char*)ciphertext, encLen);
  // base64_decode((char*)base64decoded2, (char*)payload.message, 32);
  // memcpy(enc_iv, enc_iv_from, sizeof(enc_iv_from));
  // uint16_t decLen2 = decrypt_to_cleartext(base64decoded2, strlen((char*)base64decoded2), enc_iv);
  // Serial.print("Decrypted payload.message of length: "); Serial.println(decLen2);
  // Serial.print("Decrypted payload.message: "); Serial.println((char*)cleartext);
  // ----------------------------------------------------------------------------------------------------------------------
  // transmit counts seperately from CPM, so that the receiver(s) can react to counts (with leds and sound) as they happen
  radio.write(&payload, sizeof(payload));
}

void loop() {
  // store current time to measure this loop time so we know how quickly items are added/removed from counts arrays
  timeData.mainLoopTimeStart = micros();

  // set current timestamp to be used this loop same millisecond+- depending on loop speed.
  timeData.timestamp = currentTime();

  // Serial.print("currentTime: "); Serial.println(timeData.currentTime, 12);

  // check if impulse
  if (geigerCounter.impulse == true) {
    geigerCounter.impulse = false;
    if (broadcast == true) {
      // create the message to be broadcast
      memset(message, 0, 56);
      strcat(message, credentials);
      strcat(message, "IMP");
      cipherSend();
    }
  }
  
  // set previous time each minute
  if ((timeData.timestamp - timeData.previousTimestamp) > geigerCounter.maxPeriod) {
    Serial.print("cycle expired: "); Serial.println(timeData.timestamp, sizeof(timeData.timestamp));
    timeData.previousTimestamp = timeData.timestamp;
    geigerCounter.warmup = false; // completed 60 second warmup required for precision
    // delay(3000);
  }
  
  // step through the array and remove expired impulses by exluding them from our new array.
  geigerCounter.precisionCounts = 0;
  memset(geigerCounter.countsArrayTemp, 0, sizeof(geigerCounter.countsArrayTemp));
  for (int i = 0; i < max_count; i++) {
    if (geigerCounter.countsArray[i] >= 1) { // only entertain non zero elements
    
      // compare current timestamp (per loop) to timestamps in array
      if (((timeData.timestamp - (geigerCounter.countsArray[i])) > geigerCounter.maxPeriod)) {
      // compare current (unique) timestamp to timestamps in array
      // if (((interCurrentTime() - (geigerCounter.countsArray[i])) > geigerCounter.maxPeriod)) {
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

  // refresh ssd1306 128x64 display
  ui.update();

  if (broadcast == true) {

    // todo: broadcast efficiently by only transmitting cpm when cpm changes and by providing a periodic
    // transmission for syncronization between devices accross dropped packets
    //
    if ((geigerCounter.CPM != geigerCounter.previousCPM) || ( timeData.timestamp > (timeData.previousTimestampSyncCPM+syncIntervalCPM) )) {
      geigerCounter.previousCPM = geigerCounter.CPM;
      timeData.previousTimestampSyncCPM = timeData.timestamp;
      // create the message to be broadcast
      memset(geigerCounter.CPM_str, 0, maxCPM_StrSize);
      dtostrf(geigerCounter.CPM, 0, 0, geigerCounter.CPM_str);
      memset(message, 0, 56);
      strcat(message, credentials);
      strcat(message, "CPM");
      strcat(message, geigerCounter.CPM_str);
      cipherSend();
    }
  }

  // store time taken to complete
  timeData.mainLoopTimeTaken = micros() - timeData.mainLoopTimeStart;
  // Serial.println("timeData.mainLoopTimeTaken:" + String(timeData.mainLoopTimeTaken));
}

