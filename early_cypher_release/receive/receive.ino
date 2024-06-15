// Rad Receiver written by Benjamin Jack Cullen
// Collect and display sensor data received from a remote device.

#include <printf.h>
#include <SPI.h>
#include <RF24.h>
#include <SSD1306Wire.h>
#include <OLEDDisplayUi.h>

// ----------------------------------------------------------------------------------------------------------------------
#include "AESLib.h"
AESLib aesLib;

char cleartext[256];
char ciphertext[512];
char credentials[16];
char messageCommand[16];
char messageValue[16];
int msgLen;

String encrypted;
String decrypted;

byte aes_key[] =  { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; // AES encryption key (use your own)
byte aes_iv[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; // genreral initialization vector (use your own)
byte enc_iv[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; // iv_block gets written to
byte dec_iv[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; // iv_block gets written to

void aes_init() {
  aesLib.gen_iv(aes_iv);
  aesLib.set_paddingmode((paddingMode)0);
}

String encrypt(char * msg, byte iv[]) {
  msgLen = strlen(msg);
  char encrypted[2 * msgLen];
  aesLib.encrypt64((byte*)msg, msgLen, encrypted, aes_key, sizeof(aes_key), iv);
  return String(encrypted);
}

String decrypt(char * msg, byte iv[]) {
  msgLen = strlen(msg);
  char decrypted[msgLen]; // half may be enough
  aesLib.decrypt64(msg, msgLen, (byte*)decrypted, aes_key, sizeof(aes_key), iv);
  return String(decrypted);
}
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
  char message[maxPayloadSize];
  // unsigned char message[2*INPUT_BUFFER_LIMIT] = {0};
};
PayloadStruct payload;

// Geiger Counter
struct GCStruct {
  signed long CPM;
  char CPM_str[16];
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
  aes_init();
  strcpy(credentials, "user:pass:");

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

    // read the payload into payload struct
    uint8_t bytes = radio.getPayloadSize(); // get the size of the payload
    memset(payload.message, 0, sizeof(payload.message));
    radio.read(&payload, bytes); // fetch payload from FIFO

    // -----------------------------------------------------------------------------------------------------------------------------------------
    // decryption. we only want to display values and run functions that we are sending to ourselves from the remote sensor. we would like to
    // have more confidence that our values are legitimate, so before doing anything with the message, first send the message through the decipher
    // function and if the message is not garbage afterwards AND it somehow has the correct creds then further scrutinize the message for commands. 
    //
    Serial.println("---------------------------------------------------------------------------");
    Serial.print("[ID] "); Serial.print(payload.payloadID); Serial.print(" [payload.message] "); Serial.println(payload.message); 
    // assume deccrypt
    decrypted = decrypt(payload.message, dec_iv);
    Serial.print("[ID] "); Serial.print(payload.payloadID); Serial.print(" [payload.message] "); Serial.println(decrypted);
    // convert to char array
    memset(cleartext, 0, sizeof(cleartext));
    decrypted.toCharArray(cleartext, sizeof(cleartext));
    // now check for correct credentials
    if ((strncmp(cleartext, credentials, strlen(credentials)-1 )) == 0) {
      Serial.println("[ACCEPTED]");
      // -----------------------------------------------------------------------------------------------------------------------------------------

      // if credentials then seperate credentials from the rest of the payload message and parse for commands
      memset(messageCommand, 0, sizeof(messageCommand));
      strncpy(messageCommand, cleartext + strlen(credentials), strlen(cleartext) - strlen(credentials));
      Serial.print("[COMMAND] "); Serial.println(messageCommand);

      // impulse
      if (strncmp( messageCommand, "IMP", 3) == 0) {
        digitalWrite(speaker_0, HIGH);
        digitalWrite(speaker_0, HIGH);
        digitalWrite(led_red, HIGH);
        delay(3);
        digitalWrite(led_red, LOW);
        digitalWrite(speaker_0, LOW);
      }

      // cpm
      else if (strncmp( messageCommand, "CPM", 3) == 0) {
        memset(messageValue, 0, sizeof(messageValue));
        strncpy(messageValue, messageCommand + 3, strlen(messageCommand) - 3);
        memset(geigerCounter.CPM_str, 0, sizeof(geigerCounter.CPM_str));
        memcpy(geigerCounter.CPM_str, messageValue, sizeof(geigerCounter.CPM_str));
        geigerCounter.CPM = atoi(geigerCounter.CPM_str);
        geigerCounter.uSvh = geigerCounter.CPM * 0.00332;
      }

    }
    else {
      Serial.println("[DENIED]");
      }
  
  }
}
