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
#include <RTClib.h>
#include <stdlib.h>

// memory limitations require counts log max. default for esp32: 10240. this value dramatically effects performance of main loop time.
// larger buffer means higher max cpm reading, lower buffer means faster loop time but lower max cpm reading, at least on many MCU's this trade off is worth considering.
// on esp32 a maxcount of 100 should mean main loop time will be half the time of main loop time with max count 10240.
// it may be preferrable to have a max count <=100 (cpm 100 considered unsafe to humans) if all you are interested in is reacting to a precise cpm reading within the shortest time you can.
// if instead you are actually trying to get as precise (arduino is not medical/military grade) a reading as you can at any level of activity then you may increase max count from 10240
// providing you beleive there is the memory and performance available on the MCU your building for.
#define max_count 10240
#define CE_PIN 25 // radio can use tx
#define CSN_PIN 26 // radio can use rx
#define GEIGER_PIN 27

volatile bool state = LOW;

RTC_DS1307 rtc;
RF24 radio(CE_PIN, CSN_PIN);
SSD1306Wire display(0x3c, SDA, SCL);
OLEDDisplayUi ui(&display);

char serialBuffer[16];

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
  int countsArray[max_count]; // stores each impulse as timestamps
  int countsArrayTemp[max_count]; // temporarily stores timestamps from countsArray that have not yet expired
  bool impulse = false; // sets true each interrupt on geiger counter pin
  bool warmup = true; // sets false after first 60 seconds have passed
  unsigned long counts; // stores counts and resets to zero every minute
  unsigned long precisionCounts = 0; // stores how many elements are in counts array
  unsigned long CPM = 0; // stores cpm value according to precisionCounts (should always be equal to precisionCounts because we are not estimating)
  char CPM_str[12];
  float uSvh = 0; // stores the micro-Sievert/hour for units of radiation dosing
  unsigned long maxPeriod = 60; // maximum logging period in seconds.
  unsigned long CPM_BURST_GUAGE_LOG_PERIOD = 15000; // Logging period in milliseconds, recommended value 15000-60000.
  unsigned long CPM_BURST_GUAGE_MAX_PERIOD = 60000; // Maximum logging period without modifying this sketch. default 60000.
  unsigned long cpm_high;
  unsigned long cpm_low;
  unsigned long previousMillis; // variable for time measurement
  unsigned long currentMillis;
  unsigned int multiplier; // variable for calculation CPM
  unsigned int cpm_arr_max = 3;
  unsigned int cpm_arr_itter = 0;
  int cpms[6]={0,0,0,0,0,0};
  float cpm_average;
  int GCMODE = 2;
  signed long previousCounts;
  unsigned long countsIter;
};
GCStruct geigerCounter;

struct TimeStruct {
  double UNIX_MICRO_TIME_I;
  char UNIX_MICRO_TIME[20];
  char subTimeStr[20];
  char subTimeStrTmp[20];
  double currentTime; // a placeholder for a current time (optionally used)
  double previousTime; // a placeholder for a previous time (optionally used)
  unsigned long mainLoopTimeTaken; // necessary to count time less than a second (must be updated every loop of main)
  unsigned long mainLoopTimeStart; // necessary for loop time taken (must be recorded every loop of main)
  unsigned long subTime = 0;
  int previousSecond = 0;
  int currentSecond = 0;
  char subTimeStrTag[20] = ".";
};
TimeStruct timeData;

// concatinates unix time and 'subTime' to make timestamps. time resolution is predicated upon loop time and is not meant to be accurate, just unique compared to other times.
// ToDo: timestamp faster
double current_SUBSECOND_UNIXTIME() {

  // clear strings
  memset(timeData.subTimeStr, 0, sizeof(timeData.subTimeStr));
  memset(timeData.subTimeStrTmp, 0, sizeof(timeData.subTimeStrTmp));

  // get time now from rtc. this takes time to call so compile your own seconds/minutes/etc from micros if time is of concern. 
  DateTime time = rtc.now();

  // convert unix time integer to unix time string
  dtostrf((unsigned long)time.unixtime(), 0, 0, timeData.UNIX_MICRO_TIME);

  // each new second reset subTime to zero and multiplier back to one
  timeData.currentSecond = time.second();
  if (timeData.previousSecond != timeData.currentSecond) {
    timeData.previousSecond = timeData.currentSecond;
    timeData.subTime = 0;
  }
  // note that name subTime avoids need for refactoring making mainLoopTimeTaken units of time more flexible
  timeData.subTime+=(timeData.mainLoopTimeTaken);

  // convert subTime to string
  sprintf(timeData.subTimeStrTmp, "%d", timeData.subTime);

  // concatinate empty subTimeStr with tag
  strcat(timeData.subTimeStr, timeData.subTimeStrTag);

  // concatinate subTimeStr with subTime string temp
  strcat(timeData.subTimeStr, timeData.subTimeStrTmp);
  
  // concatinate unix time with subTime string
  strcat(timeData.UNIX_MICRO_TIME, timeData.subTimeStr);

  // make the string a double
  timeData.UNIX_MICRO_TIME_I = atof(timeData.UNIX_MICRO_TIME);
  return timeData.UNIX_MICRO_TIME_I;
}

// subprocedure for capturing events from Geiger Kit
void tubeImpulseISR() {
  geigerCounter.impulse = true;
  if (geigerCounter.countsIter < max_count) {geigerCounter.countsIter++;}
  else {geigerCounter.countsIter=0;}
  // add the impulse as a timestamp to array with index somewhere in range of max_count
  // if you have better performance/hardware and a 'lighter' call to retrieve more accurate time then uniquely timestamp each impulse below. but do not overload the ISR.
  geigerCounter.countsArray[geigerCounter.countsIter] = timeData.currentTime;
}

void BGTubeImpulseISR() {
  geigerCounter.counts++;
  geigerCounter.impulse = true;
}

// frame to be displayed on ssd1306 182x64
void GC_Measurements(OLEDDisplay* display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  if (geigerCounter.GCMODE == 2) {
    display->setTextAlignment(TEXT_ALIGN_CENTER);

    if (geigerCounter.CPM >= 99) { display->drawString(display->getWidth()/2, 0, "WARNING");}
    else {display->drawString(display->getWidth()/2, 0, String(timeData.mainLoopTimeTaken));}
    display->drawString(display->getWidth()/2, 25, "cpm");
    display->drawString(display->getWidth()/2, 13, String(geigerCounter.CPM));
    display->drawString(display->getWidth()/2, display->getHeight()-10, "uSv/h");
    display->drawString(display->getWidth()/2, display->getHeight()-22, String(geigerCounter.uSvh));
  }
  else if (geigerCounter.GCMODE == 3) {
    display->setTextAlignment(TEXT_ALIGN_CENTER);
    if (geigerCounter.CPM >= 99) { display->drawString(display->getWidth()/2, 0, "WARNING");}
    else {display->drawString(display->getWidth()/2, 0, String(timeData.mainLoopTimeTaken));}
    display->drawString(display->getWidth()/2, 25, "cpm");
    display->drawString(display->getWidth()/2, 13, String(geigerCounter.CPM));
    display->drawString(display->getWidth()/2, display->getHeight()-10, "uSv/h");
    display->drawString(display->getWidth()/2, display->getHeight()-22, String(geigerCounter.uSvh));
  }
}

// this array keeps function pointers to all frames are the single views that slide in
FrameCallback frames[] = { GC_Measurements };
int frameCount = 1;

void setup() {

  // serial
  Serial.begin(115200);

  // rtc
  if (! rtc.begin()) {
    Serial.println("Couldn't find RTC");
    Serial.flush();
    while (1) delay(10);
  }
  if (! rtc.isrunning()) {
    Serial.println("RTC is NOT running, let's set the time!");
  }
  rtc.adjust(DateTime(1970, 1, 1, 0, 0, 0)); // Y M D H MS. uncomment this thine during compile time only if the clock is not already set

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

  // radio
  if (!radio.begin()) {
    Serial.println(F("radio hardware is not responding!!"));
    while (1) {}
  }
  radio.flush_rx();
  radio.flush_tx();
  radio.setPALevel(RF24_PA_LOW); // RF24_PA_MAX is default.
  radio.setPayloadSize(sizeof(payload)); // 2x int datatype occupy 8 bytes
  Serial.println("Channel:  " + String(radio.getChannel()));
  Serial.println("Data Rate:" + String(radio.getDataRate()));
  Serial.println("PA Level: " + String(radio.getPALevel()));
  radio.openWritingPipe(address[1]); // always uses pipe 0
  radio.openReadingPipe(1, address[0]); // using pipe 1
  radio.stopListening();

  // attachInterrupt(GEIGER_PIN, tubeImpulseISR, FALLING); // define external interrupts
}


void loop() {
  // store current time to measure this loop time so we know how quickly items are added/removed from counts arrays
  timeData.mainLoopTimeStart = micros();

  // set current timestamp to be used this loop as UNIXTIME + subsecond time. this is not indended for actual time like a wrist watch.
  // also set time once per loop unless you have the hardware/perfromance to set time for each impulse with a faster/lighter timestamping method that can sit in the ISR,
  // impulses in the same loop will have the same stamp which will not effect accuracy on spikes but when those impulses expire, they will expire in the same millisecond+- depending on loop speed.
  timeData.currentTime = current_SUBSECOND_UNIXTIME();

  // Serial.print("currentTime: "); Serial.println(timeData.currentTime, 12);

  // check if impulse
  if (geigerCounter.impulse == true) {
    geigerCounter.impulse = false;

    // transmit counts seperately from CPM, so that the receiver(s) can react to counts (with leds and sound) as they happen, as you would expect from a 'local' geiger counter.
    memset(payload.message, 0, 12);
    memcpy(payload.message, "X", 1);
    payload.payloadID = 1000;
    radio.write(&payload, sizeof(payload));
  }
  
  // use precision cpm counter (measures actual cpm to as higher resolution as it can per minute)
  if (geigerCounter.GCMODE == 2) {
    detachInterrupt(GEIGER_PIN);
    attachInterrupt(GEIGER_PIN, tubeImpulseISR, FALLING); // define external interrupts

    // set previous time each minute
    if ((timeData.currentTime - timeData.previousTime) > geigerCounter.maxPeriod) {
      Serial.print("cycle expired: "); Serial.println(timeData.currentTime, sizeof(timeData.currentTime));
      timeData.previousTime = timeData.currentTime;
      geigerCounter.warmup = false; // completed 60 second warmup required for precision
    }
    
    // step through the array and remove expired impulses by exluding them from our new array.
    geigerCounter.precisionCounts = 0;
    memset(geigerCounter.countsArrayTemp, 0, sizeof(geigerCounter.countsArrayTemp));
    for (int i = 0; i < max_count; i++) {
      if (geigerCounter.countsArray[i] >= 1) { // only entertain non zero elements
        // Serial.println(String(geigerCounter.countsArray[i]) + " REMOVING");

        // if you have better performance/hardware then get current time here before comparing time (see tubeImpulseISR. tubeImpulseISR is timestamp into array, here timestamp must leave array)
        if (((timeData.currentTime - (geigerCounter.countsArray[i])) > geigerCounter.maxPeriod)) {
          geigerCounter.countsArray[i] = 0;
          }
        else {
          // Serial.println(String(geigerCounter.countsArray[i]));
          geigerCounter.precisionCounts++; // non expired counters increment the precision counter
          geigerCounter.countsArrayTemp[i] = geigerCounter.countsArray[i]; // non expired counters go into the new temporary array
        }
      }
    }
    memset(geigerCounter.countsArray, 0, sizeof(geigerCounter.countsArray));
    memcpy(geigerCounter.countsArray, geigerCounter.countsArrayTemp, sizeof(geigerCounter.countsArray));

    // then calculate usv/h
    geigerCounter.CPM = geigerCounter.precisionCounts;
    geigerCounter.uSvh = geigerCounter.CPM * 0.00332;
  }
  
  // cpm burst guage (estimates cpm reactively with a dynamic time window in order to update values and peripherals responsively)
  // the impulse measurement time window increases and decreases inversely proportional to current counts. counting slow takes time to update values, count to fast and you cant measure low activity,
  // so the cpm burst guage does both, responsively and inversely proportional to counts. higher counts means smaller time window, lower counts meanse larger time window.
  else if (geigerCounter.GCMODE == 3) {
    detachInterrupt(GEIGER_PIN);
    attachInterrupt(GEIGER_PIN, BGTubeImpulseISR, FALLING); // define external interrupts
    // cpm burst guage
    geigerCounter.CPM_BURST_GUAGE_LOG_PERIOD = 15000;
    geigerCounter.CPM_BURST_GUAGE_MAX_PERIOD = 60000;
    if (geigerCounter.counts >= 1) {
      geigerCounter.CPM_BURST_GUAGE_LOG_PERIOD = 15000 / geigerCounter.counts;
      geigerCounter.CPM_BURST_GUAGE_MAX_PERIOD = 60000 / geigerCounter.counts;
      geigerCounter.CPM_BURST_GUAGE_LOG_PERIOD = (unsigned long)(geigerCounter.CPM_BURST_GUAGE_LOG_PERIOD);
      geigerCounter.CPM_BURST_GUAGE_MAX_PERIOD = (unsigned long)(geigerCounter.CPM_BURST_GUAGE_MAX_PERIOD);
    }
    // store highs and lows
    if (geigerCounter.CPM > geigerCounter.cpm_high) {geigerCounter.cpm_high = geigerCounter.CPM;};
    if ((geigerCounter.CPM < geigerCounter.cpm_low) && (geigerCounter.CPM >= 1)) {geigerCounter.cpm_low = geigerCounter.CPM;};
    // check the variable time window
    geigerCounter.currentMillis = millis();
    if(geigerCounter.currentMillis - geigerCounter.previousMillis > geigerCounter.CPM_BURST_GUAGE_LOG_PERIOD){
      geigerCounter.previousMillis = geigerCounter.currentMillis;
      geigerCounter.multiplier = geigerCounter.CPM_BURST_GUAGE_MAX_PERIOD / geigerCounter.CPM_BURST_GUAGE_LOG_PERIOD; // calculating multiplier, depend on your log period
      geigerCounter.multiplier = (unsigned int)(geigerCounter.multiplier);
      geigerCounter.CPM = geigerCounter.counts * geigerCounter.multiplier;
      geigerCounter.uSvh = geigerCounter.CPM * 0.00332; // multiply cpm by 0.003321969697 for geiger muller tube J305
      int i;
      float sum = 0;
      if (geigerCounter.cpm_arr_itter <= geigerCounter.cpm_arr_max) {geigerCounter.cpms[geigerCounter.cpm_arr_itter]=geigerCounter.cpm_high; Serial.println("[" + String(geigerCounter.cpm_arr_itter) + "] " + String(geigerCounter.cpms[geigerCounter.cpm_arr_itter])); geigerCounter.cpm_arr_itter++;}
      if (geigerCounter.cpm_arr_itter == geigerCounter.cpm_arr_max) {
        // average between lowest high and highest high (so far the more prefferable)
        for(i = 0; i < 3; i++) {sum = sum + geigerCounter.cpms[i];}
        geigerCounter.cpm_average = sum/3.0;
        geigerCounter.cpm_average = (long int)geigerCounter.cpm_average;
        Serial.println("cpm_average: " + String(geigerCounter.cpm_average));
        geigerCounter.uSvh = geigerCounter.cpm_average * 0.00332;
        geigerCounter.cpm_high=0; geigerCounter.cpm_low=0; geigerCounter.cpm_arr_itter = 0;
      }
    geigerCounter.counts = 0;
    }
  }

  // refresh ssd1306 128x64 display
  ui.update();

  // transmit the results
  memset(payload.message, 0, 12);
  memset(geigerCounter.CPM_str, 0, 12);
  dtostrf(geigerCounter.CPM, 0, 4, geigerCounter.CPM_str);
  memcpy(payload.message, geigerCounter.CPM_str, sizeof(geigerCounter.CPM_str));
  payload.payloadID = 1111;
  radio.write(&payload, sizeof(payload));

  // store time taken to complete
  timeData.mainLoopTimeTaken = micros() - timeData.mainLoopTimeStart;
  // Serial.println("timeData.mainLoopTimeTaken:" + String(timeData.mainLoopTimeTaken));
  // delay(1000);
}

