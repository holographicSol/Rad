// Rad Receiver written by Benjamin Jack Cullen
// Collect and display sensor data received from a remote device.

// ----------------------------------------------------------------------------------------------------------------------------

#include <printf.h>
#include <SPI.h>
#include <RF24.h>
#include <SSD1306Wire.h>
#include <OLEDDisplayUi.h>
#include <AESLib.h>

// ----------------------------------------------------------------------------------------------------------------------------

#define max_count 100
#define CE_PIN 25 // radio can use tx
#define CSN_PIN 26 // radio can use rx
#define warning_level_0 99 // warn at this cpm 

// ----------------------------------------------------------------------------------------------------------------------------

SSD1306Wire display(0x3c, SDA, SCL);
OLEDDisplayUi ui ( &display );
RF24 radio(CE_PIN, CSN_PIN);
AESLib aesLib;

// ----------------------------------------------------------------------------------------------------------------------------

int led_red   = 32; // led 32 RED 2 BLUE 4 GREEN
int speaker_0 = 33; // geiger counter sound
// radio addresses
uint8_t address[][6] = { "0Node", "1Node", "2Node", "3Node", "4Node", "5Node"};
bool credentialsAccepted = false;

// ----------------------------------------------------------------------------------------------------------------------------

#define CIPHERBLOCKSIZE 32 // limited to 32 bytes inline with NRF24L01+ max payload bytes

struct AESStruct {
  byte aes_key[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; // AES encryption key (use your own)
  byte aes_iv[16]  = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; // genreral initialization vector (use your own)
  byte enc_iv[16]  = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; // iv_block gets written to
  byte dec_iv[16]  = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; // iv_block gets written to
  char cleartext[(unsigned long)(CIPHERBLOCKSIZE/2)] = {0};              // half the size of ciphertext
  char ciphertext[CIPHERBLOCKSIZE] = {0};                                // twice the size of cleartext
  char credentials[(unsigned long)(CIPHERBLOCKSIZE/2)];                  // a recognizable tag 
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

struct CommandServerStruct {
  char messageCommand[(unsigned long)(CIPHERBLOCKSIZE/2)];
  char messageValue[(unsigned long)(CIPHERBLOCKSIZE/2)];
};
CommandServerStruct commandserver;

// ----------------------------------------------------------------------------------------------------------------------------

struct PayloadStruct {
  unsigned long payloadID;
  char message[CIPHERBLOCKSIZE];
};
PayloadStruct payload;

// ----------------------------------------------------------------------------------------------------------------------------

// Geiger Counter
struct GCStruct {
  signed long CPM;
  float uSvh = 0; // stores the micro-Sievert/hour for units of radiation dosing
};
GCStruct geigerCounter;

// ----------------------------------------------------------------------------------------------------------------------------

// frame to be displayed on ssd1306 182x64
void GC_Measurements(OLEDDisplay* display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  if (geigerCounter.CPM >= 99) { display->drawString(display->getWidth()/2, 0, "WARNING");}
  display->drawString(display->getWidth()/2, 25, "cpm");
  display->drawString(display->getWidth()/2, 13, String(geigerCounter.CPM));
  display->drawString(display->getWidth()/2, display->getHeight()-10, "uSv/h");
  display->drawString(display->getWidth()/2, display->getHeight()-22, String(geigerCounter.uSvh));
}
FrameCallback frames[] = { GC_Measurements }; // keeps function pointers to all frames are the single views that slide in
int frameCount = 1;

// ----------------------------------------------------------------------------------------------------------------------------

void cipherReceive() {
  Serial.println("---------------------------------------------------------------------------");

  // ensure false
  credentialsAccepted = false;

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

    // if accepted credentials 
    if ((strncmp(aes.cleartext, aes.credentials, strlen(aes.credentials)-1 )) == 0) {
      credentialsAccepted = true;

      // seperate credentials from the rest of the payload message ready for command parse
      memset(commandserver.messageCommand, 0, sizeof(commandserver.messageCommand));
      strncpy(commandserver.messageCommand, aes.cleartext + strlen(aes.credentials), strlen(aes.cleartext) - strlen(aes.credentials));
      Serial.print("[Command]                "); Serial.println(commandserver.messageCommand);
    }

    // display payload information after decryption
    Serial.print("[aes.cleartext]          "); Serial.println(aes.cleartext); 
    Serial.print("[Bytes(aes.cleartext)]   "); Serial.println(strlen(aes.cleartext));
    Serial.print("[Credentials]            "); Serial.println(credentialsAccepted);
  }
}

// ----------------------------------------------------------------------------------------------------------------------------

void processCommand() {
  // compare message command to known commands.

  // impulse
  if (strncmp( commandserver.messageCommand, "IMP", 3) == 0) {
    digitalWrite(speaker_0, HIGH);
    digitalWrite(speaker_0, HIGH);
    digitalWrite(led_red, HIGH);
    delay(3);
    digitalWrite(led_red, LOW);
    digitalWrite(speaker_0, LOW);
  }

  // cpm
  else if (strncmp( commandserver.messageCommand, "CPM", 3) == 0) {
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
  // this is just a tag to let us know if we decrypted anything.
  // RF24 payload limited to 32bytes while encryption doubles
  // the size of our payload.message. this means we have a little
  // under 15 bytes for our unencrypted payload message plus an
  // extra byte or so for out payloadID.
  // payload chunking is always an option but it will be slower.
  // otherwise its a trade off between longer in message creds or
  // more meaningful data being transmitted.
  // 32 byte limitation:
  //                1           +          3           +         12
  // example: 1byte (payloadID) + Nbytes (credentials) + remaining bytes (data)
  // further deducting a command message of say 3 bytes leaves us with
  // for example if you wanted to transmit say a number then the 
  // max number of 9,999,999,999 billion would be that number without
  // simplifying the expression of say that number.
  strcpy(aes.credentials, "iD:");

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

    // go through 'security'
    cipherReceive();

    // process command
    if (credentialsAccepted == true) {
      processCommand();
    }
  }
}
