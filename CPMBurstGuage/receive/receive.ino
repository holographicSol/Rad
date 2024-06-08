// Written by Benjamin Jack Cullen

#include "SSD1306Wire.h" // legacy include: `#include "SSD1306.h"`
#include <SPI.h>
#include "printf.h"
#include "RF24.h"
#include "OLEDDisplayUi.h"

#define CE_PIN 25
#define CSN_PIN 26

SSD1306Wire display(0x3c, SDA, SCL);
OLEDDisplayUi ui ( &display );
RF24 radio(CE_PIN, CSN_PIN);

int LOG_PERIOD = 15000; //Logging period in milliseconds, recommended value 15000-60000.
int MAX_PERIOD = 60000; //Maximum logging period without modifying this sketch. default 60000.
int PREVIOUS_LOG_PERIOD = 15000;

int gcled_count = 32;      // LED 32RED 2BLUE 4GREEN
int gcsound_count = 33;    // geiger counter sound

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

unsigned long counts;         //variable for GM Tube events
unsigned long cpm;            //variable for CPM
float usvh;
unsigned int multiplier;      //variable for calculation CPM in this sketch
unsigned long previousMillis; //variable for time measurement
unsigned long currentMillis;
unsigned long cpm_high;
unsigned long cpm_low;
unsigned int cpm_arr_max = 5;
unsigned int cpm_arr_itter = 0;
int cpms[6]={0,0,0,0,0,0};
unsigned long cpm_average = 0;
float usvh_average;

char buff_counts[12];
char buff_cpm[12];
char buff_uSvh[12];
char buff_cpm_average[12];

void GC_Measurements(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  display->drawString(0 , 15, "CPM:    "+String(cpm_average) );
  display->drawString(0 , 25, "uSv/h:  "+String(usvh_average) );
}

// This array keeps function pointers to all frames
// frames are the single views that slide in
FrameCallback frames[] = { GC_Measurements };

// how many frames are there?
int frameCount = 1;

void setup() {

  // serial
  Serial.begin(115200);
  while (!Serial) {
  }

  // display
  display.init();
  display.flipScreenVertically();
  ui.setTargetFPS(60);
  ui.setFrames(frames, frameCount);
  display.setContrast(255);
  display.setFont(ArialMT_Plain_10);
  display.cls();
  display.println("starting..");

  // geiger counter
  counts = 0;
  cpm = 0;
  multiplier = MAX_PERIOD / LOG_PERIOD; // calculating multiplier, depend on your log period
  pinMode(gcled_count, OUTPUT);
  pinMode(gcsound_count, OUTPUT);
  digitalWrite(gcsound_count, LOW);
  digitalWrite(gcled_count, LOW); 

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
  radio.openWritingPipe(address[0]);      // always uses pipe 0
  radio.openReadingPipe(1, address[1]);  // using pipe 1
  radio.stopListening();
  radio.startListening();  // put radio in RX mode
  
} 

void loop() {
  // This device is the RX node
  ui.update();

  // get payload
  uint8_t pipe;
  if (radio.available(&pipe)) {              // is there a payload? get the pipe number that recieved it
    uint8_t bytes = radio.getPayloadSize();  // get the size of the payload
    radio.read(&payload, bytes);             // fetch payload from FIFO

    // clicks
    if (strncmp(payload.message, "X", 1) == 0){
      digitalWrite(gcsound_count, HIGH);
      digitalWrite(gcled_count, HIGH);  // turn the LED on (HIGH is the voltage level)
      delay(3);                      // wait for a second
      digitalWrite(gcled_count, LOW);   // turn the LED off by making the voltage LOW
      digitalWrite(gcsound_count, LOW);
    }

    // counts
    else {
      memset(buff_cpm_average, 0, 12);
      memcpy(buff_cpm_average, payload.message, 12);
      cpm_average = atoi(buff_cpm_average);
      usvh_average = cpm_average * 0.003321969697;

  }
  }
delay(10);
}
