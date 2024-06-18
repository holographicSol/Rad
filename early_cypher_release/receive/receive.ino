// Rad Command Server written by Benjamin Jack Cullen
// Collect and display sensor data received from remote Rad Sensor Node(s).

// ----------------------------------------------------------------------------------------------------------------------------

#include <printf.h>
#include <SPI.h>
#include <RF24.h>
#include <SSD1306Wire.h>
#include <OLEDDisplayUi.h>
#include <AESLib.h>

// ----------------------------------------------------------------------------------------------------------------------------

#define gc_warning_level_0  99 // warn if remote geiger counter reaches this cpm
#define CE_PIN              25 // radio can use tx
#define CSN_PIN             26 // radio can use rx
#define CIPHERBLOCKSIZE     32 // limited to 32 bytes inline with NRF24L01+ max payload bytes

// ----------------------------------------------------------------------------------------------------------------------------

SSD1306Wire display(0x3c, SDA, SCL);
OLEDDisplayUi ui ( &display );
RF24 radio(CE_PIN, CSN_PIN);
AESLib aesLib;

// ----------------------------------------------------------------------------------------------------------------------------

int led_red   = 32; // rgb led, 32 red, 2 blue, 4 green. for remote geiger counter impulses
int speaker_0 = 33; // for remote geiger counter impulses
// radio addresses
uint8_t address[][6] = { "0Node", "1Node", "2Node", "3Node", "4Node", "5Node"};
bool fingerprintAccepted = false;

// ----------------------------------------------------------------------------------------------------------------------------

// reaching message command requieres getting through security, consider nothing as secure
struct CommandServerStruct {
  char messageCommand[(unsigned long)(CIPHERBLOCKSIZE/2)];
  char messageValue[(unsigned long)(CIPHERBLOCKSIZE/2)];
};
CommandServerStruct commandserver;

// ----------------------------------------------------------------------------------------------------------------------------

// a simple struct for wireless incoming/outgoing information
struct PayloadStruct {
  unsigned long payloadID;
  char message[CIPHERBLOCKSIZE];
};
PayloadStruct payload;

// ----------------------------------------------------------------------------------------------------------------------------

// remote geiger counter sensor
struct GCStruct {
  signed long CPM; // stores counts per minute
  float uSvh = 0;  // stores the micro-Sievert/hour for units of radiation dosing
};
GCStruct geigerCounter;

// ----------------------------------------------------------------------------------------------------------------------------

// frame to be displayed on ssd1306 182x64
void GC_Measurements(OLEDDisplay* display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  if (geigerCounter.CPM >= gc_warning_level_0) { display->drawString(display->getWidth()/2, 0, "WARNING");}
  display->drawString(display->getWidth()/2, 25, "cpm");
  display->drawString(display->getWidth()/2, 13, String(geigerCounter.CPM));
  display->drawString(display->getWidth()/2, display->getHeight()-10, "uSv/h");
  display->drawString(display->getWidth()/2, display->getHeight()-22, String(geigerCounter.uSvh));
}
FrameCallback frames[] = { GC_Measurements }; // array keeps function pointers to all frames are the single views that slide in
int frameCount = 1;

// ----------------------------------------------------------------------------------------------------------------------------

struct AESStruct {
  byte aes_key[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; // AES encryption key (use your own)
  byte aes_iv[16]  = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; // genreral initialization vector (use your own)
  byte enc_iv[16]  = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; // iv_block gets written to
  byte dec_iv[16]  = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; // iv_block gets written to
  char cleartext[(unsigned long)(CIPHERBLOCKSIZE/2)] = {0};              // half the size of ciphertext
  char ciphertext[CIPHERBLOCKSIZE] = {0};                                // twice the size of cleartext
  char fingerprint[(unsigned long)(CIPHERBLOCKSIZE/2)];                  // a recognizable tag 
  char tmp_cleartext[(unsigned long)(CIPHERBLOCKSIZE/2)];                // the same size of cleartext
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
  aes.plain_len = aesLib.decrypt64(msg, aes.msgLen, (byte*)aes.tmp_cleartext, aes.aes_key, sizeof(aes.aes_key), iv);
  strncpy(aes.cleartext, aes.tmp_cleartext, aes.plain_len);
}

// ----------------------------------------------------------------------------------------------------------------------------

bool cipherReceive() {
  Serial.println("---------------------------------------------------------------------------");

  // ensure false
  fingerprintAccepted = false;

  // read the payload into payload struct
  uint8_t bytes = radio.getPayloadSize();

  // check payload size within limit
  if (bytes <= sizeof(payload)) {
    
    // read the payload into the payload struct
    memset(payload.message, 0, sizeof(payload.message));
    radio.read(&payload, bytes); // fetch payload from FIFO

    // display raw payload
    Serial.print("[payload.payloadID]      "); Serial.println(payload.payloadID);
    Serial.print("[payload.message]        "); Serial.println(payload.message); 
    Serial.print("[Bytes(payload.message)] "); Serial.println(strlen(payload.message));

    // deccrypt (does not matter if not encrypted because we are only interested in encrypted payloads. turn anything else to junk)
    decrypt((char*)payload.message, aes.dec_iv);

    // if accepted fingerprint 
    if ((strncmp(aes.cleartext, aes.fingerprint, strlen(aes.fingerprint)-1 )) == 0) {
      fingerprintAccepted = true;

      // seperate fingerprint from the rest of the payload message ready for command parse
      memset(commandserver.messageCommand, 0, sizeof(commandserver.messageCommand));
      strncpy(commandserver.messageCommand, aes.cleartext + strlen(aes.fingerprint), strlen(aes.cleartext) - strlen(aes.fingerprint));
      Serial.print("[Command]                "); Serial.println(commandserver.messageCommand);
    }

    // display payload information after decryption
    Serial.print("[aes.cleartext]          "); Serial.println(aes.cleartext); 
    Serial.print("[Bytes(aes.cleartext)]   "); Serial.println(strlen(aes.cleartext));
    Serial.print("[fingerprint]            "); Serial.println(fingerprintAccepted);
  }
  return fingerprintAccepted;
}

// ----------------------------------------------------------------------------------------------------------------------------

void centralCommand() {
  // compare message command to known commands. we can only trust the central command as much as we can trust message command,
  // which is the reason for encryption and inner message fingerprinting.
  // add any commands intended to be received from any nodes here below: 

  // geiger counter impulse
  if (strncmp( commandserver.messageCommand, "IMP", 3) == 0) {
    digitalWrite(speaker_0, HIGH);
    digitalWrite(speaker_0, HIGH);
    digitalWrite(led_red, HIGH);
    delay(3);
    digitalWrite(led_red, LOW);
    digitalWrite(speaker_0, LOW);
  }

  // geiger counter cpm
  else if (strncmp( commandserver.messageCommand, "CPM", 3) == 0) {
    memset(commandserver.messageValue, 0, sizeof(commandserver.messageValue));
    strncpy(commandserver.messageValue, commandserver.messageCommand + 3, strlen(commandserver.messageCommand) - 3);
    geigerCounter.CPM = atoi(commandserver.messageValue);
    geigerCounter.uSvh = geigerCounter.CPM * 0.00332;
  }
}

// ----------------------------------------------------------------------------------------------------------------------------

void setup() {

  // ------------------------------------------------------------

  // setup serial
  Serial.begin(115200);
  while (!Serial) {
  }

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

  // setup geiger counter
  pinMode(led_red, OUTPUT);
  pinMode(speaker_0, OUTPUT);
  digitalWrite(speaker_0, LOW);
  digitalWrite(led_red, LOW);

  // ------------------------------------------------------------

  // setup aes
  aes_init();
  /*
  fingerprint is to better know if we decrypted anything correctly and can also be used for ID. consider the following wit
  NRF24L01+ max 32 bytes payload to understand a trade off between fingerprint strength and data size, understanding that
  the larger the fingerprint, the less the data and vis versa without compression, payload chunking etc.: 
                  1         +          3           +         12
          1byte (payloadID) + Nbytes (fingerprint) + remaining bytes (data)
  */
  strcpy(aes.fingerprint, "iD:");

  // ------------------------------------------------------------

  // setup radio
  if (!radio.begin()) {
    Serial.println(F("radio hardware is not responding!!"));
    while (1) {}
  }
  radio.flush_rx();
  radio.flush_tx();
  radio.setPayloadSize(sizeof(payload));   // 2x int datatype occupy 8 bytes
  radio.openWritingPipe(address[0][1]);    // always uses pipe 1
  radio.openReadingPipe(1, address[0][0]); // using pipe 0
  radio.stopListening();
  radio.setChannel(124);          // 0-124 correspond to 2.4 GHz plus the channel number in units of MHz (ch 21 = 2.421 GHz)
  radio.setDataRate(RF24_2MBPS);  // RF24_250KBPS, RF24_1MBPS, RF24_2MBPS
  radio.setPALevel(RF24_PA_HIGH); // RF24_PA_MIN, RF24_PA_LOW, RF24_PA_HIGH, RF24_PA_MAX.
  Serial.println("Channel:  " + String(radio.getChannel()));
  Serial.println("Data Rate:" + String(radio.getDataRate()));
  Serial.println("PA Level: " + String(radio.getPALevel()));
  radio.startListening();

  // ------------------------------------------------------------
}

// ----------------------------------------------------------------------------------------------------------------------------

void loop() {

  // refresh ssd1306 128x64 display
  ui.update();

  // get payload
  uint8_t pipe;
  if (radio.available(&pipe)) { // is there a payload? get the pipe number that recieved it

    // go through security
    if (cipherReceive() == true) {

      // go to central command
      centralCommand();
    }
  }
}
