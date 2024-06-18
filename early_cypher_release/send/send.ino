/*
Rad Sensor Node written by Benjamin Jack Cullen
Collect, display and send sensor data to the Rad Command Server.
*/

// ----------------------------------------------------------------------------------------------------------------------------
//                                                                                                                    LIBRARIES

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
//                                                                                                                      DEFINES
/*
memory limitations require counts log max.
on esp32 a maxcount of 100 should mean main loop time will be half the time of main loop time with max count 10240.
it may be preferrable to have a max count <=100 (cpm 100 considered unsafe to humans) if all you are interested in
is reacting to a precise cpm reading within the shortest time you can. if instead you are actually trying to get as
precise (arduino is not medical/military grade) a reading as you can at any level of activity then you may increase
max count from 10240 providing you beleive there is the memory and performance available on the MCU your building for.
*/
#define max_count       100
#define warning_level_0  99 // warn at this cpm 
#define CE_PIN           25 // radio can use tx
#define CSN_PIN          26 // radio can use rx
#define GEIGER_PIN       27
#define CIPHERBLOCKSIZE  32 // limited to 32 bytes inline with NRF24L01+ max payload bytes

// ----------------------------------------------------------------------------------------------------------------------------
//                                                                                                                      ALIASES

RF24 radio(CE_PIN, CSN_PIN);
SSD1306Wire display(0x3c, SDA, SCL);
OLEDDisplayUi ui(&display);
AESLib aesLib;

// ----------------------------------------------------------------------------------------------------------------------------
//                                                                                                                 RADIO STRUCT

struct RadioStruct {
  volatile bool broadcast = true;
  bool rf24_rx_report = false;
  uint8_t address[1024][6] = { "0Node", "1Node", "2Node", "3Node", "4Node", "5Node" };
};
RadioStruct radioData;

// ----------------------------------------------------------------------------------------------------------------------------
//                                                                                                        COMMAND SERVER STRUCT

// reaching message command requieres getting through security, consider nothing as secure
struct CommandServerStruct {
  char messageCommand[(unsigned long)(CIPHERBLOCKSIZE/2)];
  char messageValue[(unsigned long)(CIPHERBLOCKSIZE/2)];
};
CommandServerStruct commandserver;

// ----------------------------------------------------------------------------------------------------------------------------
//                                                                                                               PAYLOAD STRUCT

// a simple struct for wireless incoming/outgoing information
struct PayloadStruct {
  unsigned long payloadID;
  char message[CIPHERBLOCKSIZE];
};
PayloadStruct payload;


// ----------------------------------------------------------------------------------------------------------------------------
//                                                                                                        GEIGER COUNTER STRUCT

// geiger counter data
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
//                                                                                                                  TIME STRUCT

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
//                                                                                                   OLED FRAME: GEIGER COUNTER

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
FrameCallback frames[] = { GC_Measurements }; // array keeps function pointers to all frames are the single views that slide in
int frameCount = 1;

// ----------------------------------------------------------------------------------------------------------------------------
//                                                                                                 ADVANCED ENCRYPTION STANDARD 

struct AESStruct {
  byte aes_key[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; // AES encryption key (use your own)
  byte aes_iv[16]  = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; // genreral initialization vector (use your own)
  byte enc_iv[16]  = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; // iv_block gets written to
  byte dec_iv[16]  = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; // iv_block gets written to
  char cleartext[(unsigned long)(CIPHERBLOCKSIZE/2)] = {0};              // half the size of ciphertext
  char ciphertext[CIPHERBLOCKSIZE] = {0};                                // twice the size of cleartext
  char fingerprint[(unsigned long)(CIPHERBLOCKSIZE/2)];                  // a recognizable tag 
  char tmp_cleartext[(unsigned long)(CIPHERBLOCKSIZE/2)];                // the same size of cleartext
  bool fingerprintAccepted = false;
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
//                                                                                                       FUNCTION: CURRENT TIME

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
//                                                                                                  FUNCTION: INTERMEDIATE TIME

double interCurrentTime() {
  timeData.interTime = (micros() - timeData.mainLoopTimeStart);
  timeData.interTimeDivided = timeData.interTime / 1000000;
  return timeData.timestamp+timeData.interTimeDivided;
}

// ----------------------------------------------------------------------------------------------------------------------------
//                                                                                                     FUNCTION: CIPHER RECEIVE

bool cipherReceive() {
  Serial.println("---------------------------------------------------------------------------");

  // ensure false
  aes.fingerprintAccepted = false;

  // read the payload into payload struct
  uint8_t bytes = radio.getPayloadSize();

  // check payload size within limit
  if (bytes <= sizeof(payload)) {
    
    // read the payload into the payload struct
    memset(payload.message, 0, sizeof(payload.message));
    radio.read(&payload, bytes); // fetch payload from FIFO

    // display raw payload
    Serial.println("[Receiving]");
    Serial.print("[payload.payloadID]      "); Serial.println(payload.payloadID);
    Serial.print("[payload.message]        "); Serial.println(payload.message); 
    Serial.print("[Bytes(payload.message)] "); Serial.println(strlen(payload.message));

    // deccrypt (does not matter if not encrypted because we are only interested in encrypted payloads. turn anything else to junk)
    decrypt((char*)payload.message, aes.dec_iv);

    // if accepted fingerprint 
    if ((strncmp(aes.cleartext, aes.fingerprint, strlen(aes.fingerprint)-1 )) == 0) {
      aes.fingerprintAccepted = true;

      // seperate fingerprint from the rest of the payload message ready for command parse
      memset(commandserver.messageCommand, 0, sizeof(commandserver.messageCommand));
      strncpy(commandserver.messageCommand, aes.cleartext + strlen(aes.fingerprint), strlen(aes.cleartext) - strlen(aes.fingerprint));
      Serial.print("[Command]                "); Serial.println(commandserver.messageCommand);
    }

    // display payload information after decryption
    Serial.print("[aes.cleartext]          "); Serial.println(aes.cleartext); 
    Serial.print("[Bytes(aes.cleartext)]   "); Serial.println(strlen(aes.cleartext));
    Serial.print("[fingerprint]            "); Serial.println(aes.fingerprintAccepted);
  }
  return aes.fingerprintAccepted;
}

// ----------------------------------------------------------------------------------------------------------------------------
//                                                                                                        FUNCTION: CIPHER SEND

void cipherSend() {
  Serial.println("---------------------------------------------------------------------------");

  // keep payloadID well withing its cast
  payload.payloadID++;
  // limit to an N digit number so we always know how many bytes we have left in the payload
  if (payload.payloadID > 9) {payload.payloadID = 0;}

  // display raw payload
  Serial.println("[Sending]");
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
  radioData.rf24_rx_report = false;
  radioData.rf24_rx_report = radio.write(&payload, sizeof(payload));
  Serial.print("[Payload Delivery]       "); Serial.println(radioData.rf24_rx_report);
  // uncomment to test immediate replay attack
  // delay(1000);
  // radio.write(&payload, sizeof(payload));
}

// ----------------------------------------------------------------------------------------------------------------------------
//                                                                                                            FUNCTION: CENTCOM

void centralCommand() {
  /*
  compare message command to known commands. we can only trust the central command as much as we can trust message command,
  which is the reason for encryption and inner message fingerprinting.
  add any commands intended to be received from any nodes here below:
  */
}

// ----------------------------------------------------------------------------------------------------------------------------
//                                                                                                     FUNCTION: GEIGER COUNTER

void radNodeSensor0() {
  
  // this setup is for measuring cpm by monitoring the radiationD-v1.0 (CAJOE) for impulses

  // set current timestamp to be used this loop same millisecond+- depending on loop speed.
  timeData.timestamp = currentTime();
  
  // set previous time each minute
  if ((timeData.timestamp - timeData.previousTimestamp) > geigerCounter.maxPeriod) {
    Serial.print("cycle expired: "); Serial.println(timeData.timestamp, sizeof(timeData.timestamp));
    timeData.previousTimestamp = timeData.timestamp;
    geigerCounter.warmup = false;
  }
  // check if impulse
  if (geigerCounter.impulse == true) {
    geigerCounter.impulse = false;
    if (radioData.broadcast == true) {
      // create transmission message
      memset(aes.cleartext, 0, sizeof(aes.cleartext));
      strcat(aes.cleartext, aes.fingerprint);
      strcat(aes.cleartext, "IMP");
      // set our writing pipe each time in case we write to different pipes another time
      radio.stopListening();
      radio.flush_tx();
      radio.openWritingPipe(radioData.address[0][0]); // always uses pipe 0
      // encrypt and send
      cipherSend();
    }
  }
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

  if (radioData.broadcast == true) {
    // todo: broadcast periodically outside of value change
    if (geigerCounter.CPM != geigerCounter.previousCPM) {
      geigerCounter.previousCPM = geigerCounter.CPM;
      timeData.previousTimestampSecond = timeData.timestamp;
      // create transmission message
      memset(geigerCounter.CPM_str, 0, sizeof(geigerCounter.CPM_str));
      dtostrf(geigerCounter.CPM, 0, 0, geigerCounter.CPM_str);
      memset(aes.cleartext, 0, sizeof(aes.cleartext));
      strcat(aes.cleartext, aes.fingerprint);
      strcat(aes.cleartext, "CPM");
      strcat(aes.cleartext, geigerCounter.CPM_str);
      // set our writing pipe each time in case we write to different pipes another time
      radio.stopListening();
      radio.flush_tx();
      radio.openWritingPipe(radioData.address[0][0]); // always uses pipe 0
      // encrypt and send
      cipherSend();
    }
  }
}

// ----------------------------------------------------------------------------------------------------------------------------
//                                                                                                              FUNCTION: SETUP

void setup() {

  // --------------------------------------------------------------------------------------------------------------------------
  //                                                                                                               SETUP SERIAL

  Serial.begin(115200);

  // --------------------------------------------------------------------------------------------------------------------------
  //                                                                                                              SETUP DISPLAY

  display.init();
  ui.setTargetFPS(60);
  ui.disableAllIndicators();
  ui.setFrames(frames, frameCount);
  display.flipScreenVertically();
  display.setContrast(255);
  display.setFont(ArialMT_Plain_10);
  display.cls();
  display.println("starting..");

  // --------------------------------------------------------------------------------------------------------------------------
  //                                                                                                                  SETUP AES

  aes_init();
  /*
  fingerprint is to better know if we decrypted anything correctly and can also be used for ID. consider the following wit
  NRF24L01+ max 32 bytes payload to understand a trade off between fingerprint strength and data size, understanding that
  the larger the fingerprint, the less the data and vis versa without compression, payload chunking etc.: 
                  1         +          3           +         12
          1byte (payloadID) + Nbytes (fingerprint) + remaining bytes (data)
  */
  strcpy(aes.fingerprint, "iD:");

  // --------------------------------------------------------------------------------------------------------------------------
  //                                                                                                                SETUP RADIO

  if (!radio.begin()) {
    Serial.println(F("radio hardware is not responding!!"));
    while (1) {}
  }
  radio.flush_rx();
  radio.flush_tx();
  radio.setPayloadSize(sizeof(payload)); // 2x int datatype occupy 8 bytes
  radio.stopListening();
  radio.setChannel(124);          // 0-124 correspond to 2.4 GHz plus the channel number in units of MHz (ch 21 = 2.421 GHz)
  radio.setDataRate(RF24_2MBPS);  // RF24_250KBPS, RF24_1MBPS, RF24_2MBPS
  radio.setPALevel(RF24_PA_HIGH); // RF24_PA_MIN, RF24_PA_LOW, RF24_PA_HIGH, RF24_PA_MAX.
  Serial.println("Channel:  " + String(radio.getChannel()));
  Serial.println("Data Rate:" + String(radio.getDataRate()));
  Serial.println("PA Level: " + String(radio.getPALevel()));
  // radio.startListening();

  // --------------------------------------------------------------------------------------------------------------------------
  //                                                                                                           SETUP INTERRUPTS

  attachInterrupt(GEIGER_PIN, tubeImpulseISR, FALLING);

  // --------------------------------------------------------------------------------------------------------------------------
}

// ----------------------------------------------------------------------------------------------------------------------------
//                                                                                                                    MAIN LOOP

void loop() {

  // store current time to measure this loop time so we know how quickly items are added/removed from counts arrays
  timeData.mainLoopTimeStart = micros();

  // default to rx each loop (optional because this is a sensor node and does not have to receive but it can)
  // uncomment to enable this node to receive commands (for security reasons you may desire your sensor node to only tx)
  radio.openReadingPipe(1, radioData.address[0][1]); // using pipe 0
  radio.startListening();
  // get payload
  uint8_t pipe;
  if (radio.available(&pipe)) { // is there a payload? get the pipe number that recieved it
    // go through security
    if (cipherReceive() == true) {
      // go to central command
      centralCommand();
    }
  }

  // get sensor information and send the results
  radNodeSensor0();

  // refresh ssd1306 128x64 display
  ui.update();

  // store time taken to complete
  timeData.mainLoopTimeTaken = micros() - timeData.mainLoopTimeStart;
}

// ----------------------------------------------------------------------------------------------------------------------------
//                                                                                         ISR FUNCTION: GEIGER COUNTER IMPULSE

void tubeImpulseISR() {
  geigerCounter.impulse = true;
  if (geigerCounter.countsIter < max_count) {geigerCounter.countsIter++;}
  else {geigerCounter.countsIter=0;}
  geigerCounter.countsArray[geigerCounter.countsIter] = timeData.timestamp;
}