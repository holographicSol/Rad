// Precision Geiger Counter written by Benjamin Jack Cullen

#include "SSD1306Wire.h" // legacy include: `#include "SSD1306.h"`
#include <SPI.h>
#include "printf.h"
#include "RF24.h"
#include "OLEDDisplayUi.h"

#define CE_PIN 25 // radio can use tx
#define CSN_PIN 26 // radio can use rx

SSD1306Wire display(0x3c, SDA, SCL);
OLEDDisplayUi ui ( &display );
RF24 radio(CE_PIN, CSN_PIN);

int led_red = 32; // led 32 RED 2 BLUE 4 GREEN
int speaker_0 = 33; // geiger counter sound

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

// Geiger Counter
struct GCStruct {
  int countsArray[10240]; // stores each impulse as micros
  int countsArrayTemp[10240]; // temporarily stores micros from countsArray that have not yet expired
  bool impulse = false; // sets true each interrupt on geiger counter pin
  bool warmup = true; // sets false after first 60 seconds have passed
  unsigned long counts = 0; // stores counts and resets to zero every minute
  unsigned long precisionCounts = 0; // stores how many elements are in counts array
  unsigned long precisionCPM = 0; // stores cpm value according to precisionCounts (should always be equal to precisionCounts because we are not estimating)
  char precisionCPM_str[12];
  float precisioncUSVH = 0; // stores the micro-Sievert/hour for units of radiation dosing
  unsigned long maxPeriod = 60000000; //Maximum logging period in microseconds
  unsigned long currentMicrosMain; // stores current
  unsigned long previousMicrosMain; // stores previous
  unsigned long precisionMicros; // stores main loop time
  char precisionMicros_str[12]; // stores main loop time
};
GCStruct geigerCounter;

void GC_Measurements(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  display->drawString(0 , 0, "Remote");
  display->drawString(0 , 15, "CPM:    "+String(geigerCounter.precisionCPM) );
  display->drawString(0 , 25, "uSv/h:  "+String(geigerCounter.precisioncUSVH) );
}

// this array keeps function pointers to all frames are the single views that slide in
FrameCallback frames[] = { GC_Measurements };
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
  pinMode(led_red, OUTPUT);
  pinMode(speaker_0, OUTPUT);
  digitalWrite(speaker_0, LOW);
  digitalWrite(led_red, LOW); 

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
  radio.openWritingPipe(address[0]); // always uses pipe 0
  radio.openReadingPipe(1, address[1]); // using pipe 1
  radio.stopListening();
  radio.startListening(); // put radio in RX mode
  
} 

void loop() {

  // refresh ssd1306 128x64 display
  ui.update();

  // get payload
  uint8_t pipe;
  if (radio.available(&pipe)) { // is there a payload? get the pipe number that recieved it
    uint8_t bytes = radio.getPayloadSize(); // get the size of the payload
    radio.read(&payload, bytes); // fetch payload from FIFO
    // Serial.println(String("[ID ") + String(payload.payloadID) + "] " + payload.message);

    // counts
    if (payload.payloadID == 1000){
      digitalWrite(speaker_0, HIGH);
      digitalWrite(led_red, HIGH); // turn the LED on (HIGH is the voltage level)
      delay(3); // wait for a second
      digitalWrite(led_red, LOW);  // turn the LED off by making the voltage LOW
      digitalWrite(speaker_0, LOW);
    }

    // cpm
    else if (payload.payloadID == 1111){
      memset(geigerCounter.precisionCPM_str, 0, 12);
      memcpy(geigerCounter.precisionCPM_str, payload.message, 12);
      geigerCounter.precisionCPM = atoi(geigerCounter.precisionCPM_str);
      geigerCounter.precisioncUSVH = geigerCounter.precisionCPM * 0.003321969697;
    }
  }
}
