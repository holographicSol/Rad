// Precision Geiger Counter written by Benjamin Jack Cullen

#include "SSD1306Wire.h" // legacy include: `#include "SSD1306.h"`
#include <SPI.h>
#include "printf.h"
#include "RF24.h"
#include "OLEDDisplayUi.h"

// ----------------------------------------------------------------------------------------------------------------------
#include "AESLib.h"
#define BAUD 9600
AESLib aesLib;
#define INPUT_BUFFER_LIMIT (128 + 1) // designed for Arduino UNO, not stress-tested anymore (this works with message[129])
unsigned char cleartext[INPUT_BUFFER_LIMIT] = {0}; // THIS IS INPUT BUFFER (FOR TEXT)
unsigned char ciphertext[2*INPUT_BUFFER_LIMIT] = {0}; // THIS IS OUTPUT BUFFER (FOR BASE64-ENCODED ENCRYPTED DATA)
char credentials[18];
char message[56];
// AES Encryption Key (CHANGME)
byte aes_key[] = { 0x2B, 0x7E, 0x15, 0x16, 0x28, 0xAE, 0xD2, 0xA6, 0xAB, 0xF7, 0x15, 0x88, 0x09, 0xCF, 0x4F, 0x3C };
// General initialization vector (CHANGME) (you must use your own IV's in production for full security!!!)
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

#define max_count 100
#define CE_PIN 25 // radio can use tx
#define CSN_PIN 26 // radio can use rx
#define warning_level_0 99 // warn at this cpm 

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

#define maxPayloadSize 1000
struct PayloadStruct {
  unsigned long nodeID;
  unsigned long payloadID;
  unsigned char message[maxPayloadSize];
  // unsigned char message[2*INPUT_BUFFER_LIMIT] = {0};
};
PayloadStruct payload;

#define maxCPM_StrSize maxPayloadSize
// Geiger Counter
struct GCStruct {
  unsigned long CPM;
  char CPM_str[maxCPM_StrSize];
  float uSvh = 0; // stores the micro-Sievert/hour for units of radiation dosing
};
GCStruct geigerCounter;

// frame to be displayed on ssd1306 182x64
void GC_Measurements(OLEDDisplay* display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  if (geigerCounter.CPM >= 99) { display->drawString(display->getWidth()/2, 0, "WARNING");}
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
  ui.disableAllIndicators();

  // geiger counter
  pinMode(led_red, OUTPUT);
  pinMode(speaker_0, OUTPUT);
  digitalWrite(speaker_0, LOW);
  digitalWrite(led_red, LOW);

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
  radio.openWritingPipe(address[1]); // always uses pipe 0
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
  radio.startListening();
}

void loop() {

  // refresh ssd1306 128x64 display
  ui.update();

  // get payload
  uint8_t pipe;
  if (radio.available(&pipe)) { // is there a payload? get the pipe number that recieved it
    memset(payload.message, 0, sizeof(payload.message));
    memset(cleartext, 0, sizeof(cleartext));

    // read the payload into payload struct
    uint8_t bytes = radio.getPayloadSize(); // get the size of the payload
    radio.read(&payload, bytes); // fetch payload from FIFO

    // -----------------------------------------------------------------------------------------------------------------------------------------
    // decryption. we only want to display values and run functions that we are sending to ourselves from the remote sensor. we would like to
    // have more confidence that our values are legitimate, so before doing anything with the message, first send the message through the decipher
    // function and if the message is not garbage afterwards AND it somehow has the correct creds then further scrutinize the message for commands. 
    //
    // Serial.println("---------------------------------------------------------------------------");
    // Serial.print(String("[ID ") + String(payload.payloadID) + "] "); Serial.print("message: "); Serial.println((char*)payload.message);    
    //   
    // assume decrypt. force all incoming traffic through this before parsing the message for commands
    unsigned char base64decoded[50] = {0};
    base64_decode((char*)base64decoded, (char*)payload.message, 32);
    memcpy(enc_iv, enc_iv_from, sizeof(enc_iv_from));
    uint16_t decLen = decrypt_to_cleartext(payload.message, strlen((char*)payload.message), enc_iv);
    // Serial.print("Decrypted cleartext of length: "); Serial.println(decLen);
    // Serial.print("Decrypted cleartext: "); Serial.println((char*)cleartext);
    //
    // does decyphered text have correct credentials?
    if (strncmp( (char*)cleartext, credentials, strlen(credentials)-1 ) == 0) {
      // Serial.println("-- access granted. credetials authenticated.");
      //
      // -----------------------------------------------------------------------------------------------------------------------------------------

      // if so then seperate credentials from the rest of the payload message and parse for commands
      memset(message, 0, 56);
      strncpy(message, (char*)cleartext + strlen(credentials), strlen((char*)cleartext) - strlen(credentials));
      // Serial.print("-- message: "); Serial.println(message);

      // impulse
      if (strcmp( message, "IMP") == 0) {
        digitalWrite(speaker_0, HIGH);
        digitalWrite(speaker_0, HIGH);
        digitalWrite(led_red, HIGH); // turn the LED on (HIGH is the voltage level)
        delay(3);
        digitalWrite(led_red, LOW);  // turn the LED off by making the voltage LOW
        digitalWrite(speaker_0, LOW);
      }

      // cpm
      else if (strncmp( message, "CPM", 3) == 0) {
        char var[32];
        strncpy(var, message + 3, strlen(message) - 3);
        memset(geigerCounter.CPM_str, 0, maxCPM_StrSize);
        memcpy(geigerCounter.CPM_str, var, maxCPM_StrSize);
        geigerCounter.CPM = atoi(geigerCounter.CPM_str);
        geigerCounter.uSvh = geigerCounter.CPM * 0.00332;
        // Serial.print("CPM: "); Serial.println(chanbuf);
      }
    }
    else {
      // Serial.println("-- access denied. unauthorized credentials.");
      }
  }
}
