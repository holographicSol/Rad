// Wriiten by Benjamin Jack Cullen
// This version estimates CPM by using my CPM Burst Guage. For precision CPM see Rad.
// Although CPM is estimated, estimated CPM from the burst guage is surprisingly accurate.

// Include the correct display library
// For a connection via I2C using Wire include
#include <Wire.h>  // Only needed for Arduino 1.6.5 and earlier
#include "SSD1306Wire.h" // legacy include: `#include "SSD1306.h"`
#include <math.h>
#include <stdio.h>
#include <Arduino.h>
#include <SPI.h>
#include <HardwareSerial.h>
#include "printf.h"
#include "RF24.h"
#include "OLEDDisplayUi.h"

SSD1306Wire display(0x3c, SDA, SCL);
OLEDDisplayUi ui ( &display );

unsigned long LOG_PERIOD = 15000; //Logging period in milliseconds, recommended value 15000-60000.
unsigned long MAX_PERIOD = 60000; //Maximum logging period without modifying this sketch. default 60000.
int PREVIOUS_LOG_PERIOD = 15000;

#define CE_PIN 25 // can use tx
#define CSN_PIN 26 // can use rx

#define GEIGER_PIN 27

RF24 radio(CE_PIN, CSN_PIN);

uint64_t address[6] = { 0x7878787878LL,
                        0xB3B4B5B6F1LL,
                        0xB3B4B5B6CDLL,
                        0xB3B4B5B6A3LL,
                        0xB3B4B5B60FLL,
                        0xB3B4B5B605LL };

struct PayloadStruct {
  unsigned long nodeID;
  unsigned long payloadID;
  char message[10];
};
PayloadStruct payload;

unsigned long mainiter = 0;   //variable for counting main loop iterations
unsigned long counts;         //variable for GM Tube events
unsigned long cpm;            //variable for CPM
float usvh = 0;
unsigned int multiplier;      //variable for calculation CPM in this sketch
unsigned long previousMillis; //variable for time measurement
unsigned long currentMillis;
unsigned long cpm_high;
unsigned long cpm_low;
unsigned int cpm_arr_max = 3;
unsigned int cpm_arr_itter = 0;
int cpms[6]={0,0,0,0,0,0};
float cpm_average;
float usvh_average;

char buff_counts[12];
char buff_cpm[12];
char buff_uSvh[12];
char buff_cpm_average[12];
char buff_uSvh_average[12];

void tube_impulse(){ //subprocedure for capturing events from Geiger Kit
  counts++;
  memset(payload.message, 0, 12);
  memcpy(payload.message, "X", 1);
  radio.write(&payload, sizeof(payload));
}

void GC_Measurements(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  display->drawString(0 , 0,  "LOG_PERIOD: "+String(LOG_PERIOD) );
  display->drawString(0 , 15, "CPM:    "+String(cpm_average) );
  display->drawString(0 , 25, "uSv/h:  "+String(usvh_average) );
  display->drawString(0 , 35, "[B] CPM:    "+String(buff_cpm) );
  display->drawString(0 , 45, "[B] uSv/h:  "+String(buff_uSvh) );
  detachInterrupt(digitalPinToInterrupt(GEIGER_PIN));  // for stability temporarily detatch the interrupt
  radio.write(&payload, sizeof(payload));
  attachInterrupt(GEIGER_PIN, tube_impulse, FALLING); // define external interrupts
}

// This array keeps function pointers to all frames
// frames are the single views that slide in
FrameCallback frames[] = { GC_Measurements };

// how many frames are there?
int frameCount = 1;

int getMaxRepeatingElement(int array[], int n)
{
    int i, j, maxElement, count;
     int maxCount = 0;
    /* Frequency of each element is counted and checked.If it's greater than the utmost count element we found till now, then it is updated accordingly  */
    for(i = 0; i< n; i++)   //For loop to hold each element
    {
        count = 1;
        for(j = i+1; j < n; j++)  //For loop to check for duplicate elements
        {
            if(array[j] == array[i])
            {
                count++;     //Increment  count
                /* If count of current element is more than
                maxCount, then update maxElement */
                if(count > maxCount)
                {
                    maxElement = array[j];
                }
            }
        }
    }
    return maxElement;
}

void setup(){ //setup subprocedure

  // serial
  Serial.begin(115200);

  // display
  display.init();
  display.flipScreenVertically();
  ui.setTargetFPS(60);
  ui.setFrames(frames, frameCount);
  // ui.init();
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
    while (1) {}  // hold in infinite loop
  }
  radio.flush_rx();
  radio.flush_tx();
  radio.setPALevel(RF24_PA_LOW);  // RF24_PA_MAX is default.
  radio.setPayloadSize(sizeof(payload));  // 2x int datatype occupy 8 bytes
  Serial.println("Channel:  " + String(radio.getChannel()));
  Serial.println("Data Rate:" + String(radio.getDataRate()));
  Serial.println("PA Level: " + String(radio.getPALevel()));
  radio.openWritingPipe(address[1]);      // always uses pipe 0
  radio.openReadingPipe(1, address[0]);  // using pipe 1
  radio.stopListening();
  
  // setup completed
  Serial.println("");
  Serial.println("");
  Serial.println("Starting main operation..");
  Serial.println("");
  counts = 0;
  cpm = 0;
  multiplier = MAX_PERIOD / LOG_PERIOD; // calculating multiplier, depend on your log period
  attachInterrupt(GEIGER_PIN, tube_impulse, FALLING); //define external interrupts
}

void loop(){
  // The challenge: Guaging counts over larger intervals of time has the benefit of being able to actually take a measurement
  // of low radiation activity, the downside is that our output would be slow proportional to the interval of time, say 15-60 seconds.
  // This would result in output being updated once every 15-60 seconds, which is too slow to update output (on screens and clients).
  // Guaging counts over a small interval of time has the benefit updating our output faster but then on the downside, the guaging
  // time window would be too small to guage periods of low radiation activity.
  // The CPM Burst Guage: When activity is low, the guage time window is large so that there is enough time to measure low radiation
  // activity. As the activity increases, the time window decreases, inversely proportional to the activity increasing and decreasing.
  // More activity, faster updated ouptut, less activity, longer time window.
  // A high average (specifically) is taken from numerous results from the CPM Burst Guage and this high average is then used as the
  // final cpm. The speed at which the final cpm value is updated is directly proporsional to the speed at which CPM Burst Guage is
  // running at, which as mentioned is inversely proportional to the time window in which the CPM Burst Guage can count radiaion activity.
  // The final cpm value is updated on the OLED and sent to the client over RF.
  // When comparing results of this method to simply counting over exactly 60 seconds, I have found the final cpm value resulting from
  // the CPM Burst Guage to be accurately comparable but certainly not precise.

  ui.update();

  // cpm burst guage
  LOG_PERIOD = 15000;
  MAX_PERIOD = 60000;
  if (counts >= 1) {
    LOG_PERIOD = 15000 / counts;
    MAX_PERIOD = 60000 / counts;
    LOG_PERIOD = (unsigned long)(LOG_PERIOD);
    MAX_PERIOD = (unsigned long)(MAX_PERIOD);
  }
  
  // store highs and lows
  if (cpm > cpm_high) {cpm_high = cpm;};
  if ((cpm < cpm_low) && (cpm >= 1)) {cpm_low = cpm;};

  // ints and floats to strings
  memset(buff_counts, 0, 12); itoa(counts, buff_counts, 10);
  memset(buff_cpm, 0, 12); itoa(cpm, buff_cpm, 10);
  memset(buff_uSvh, 0, 12); dtostrf(usvh, 0, 4, buff_uSvh);
  memset(buff_uSvh_average, 0, 12); dtostrf(usvh_average, 0, 4, buff_uSvh_average);
  memset(buff_cpm_average, 0, 12); dtostrf(cpm_average, 0, 4, buff_cpm_average);

  memset(payload.message, 0, 12);
  memcpy(payload.message, buff_cpm_average, sizeof(buff_cpm_average));

  // check the variable time window
  currentMillis = millis();
  if(currentMillis - previousMillis > LOG_PERIOD){
    previousMillis = currentMillis;

    detachInterrupt(digitalPinToInterrupt(GEIGER_PIN));  // for stability temporarily detatch the interrupt

    multiplier = MAX_PERIOD / LOG_PERIOD; // calculating multiplier, depend on your log period
    multiplier = (unsigned int)(multiplier);

    cpm = counts * multiplier; /// multiply cpm by 0.003321969697 for geiger muller tube J305
    usvh = cpm * 0.00332;
    mainiter++;
    
    // variable time window has closed, store cpm_high in an array, if array maxed then store the sum average of the array as the estimated actual cpm: cpm_average
    int i;
    float sum = 0;
    if (cpm_arr_itter <= cpm_arr_max) {cpms[cpm_arr_itter]=cpm_high; Serial.println("[" + String(cpm_arr_itter) + "] " + String(cpms[cpm_arr_itter])); cpm_arr_itter++;}
    if (cpm_arr_itter == cpm_arr_max) {

      // average between lowest high and highest high (so far the more prefferable)
      for(i = 0; i < 3; i++){sum = sum + cpms[i];}
      cpm_average = sum/3.0;
      cpm_average = (long int)cpm_average;

      Serial.println("cpm_average: " + String(cpm_average));
      usvh_average = cpm_average * 0.00332;
      cpm_high=0; cpm_low=0; cpm_arr_itter = 0;
      
      }

    counts = 0;
    attachInterrupt(GEIGER_PIN, tube_impulse, FALLING); // define external interrupts
  }
}
