/*
Rad Command Server written by Benjamin Jack Cullen
Collect and display sensor data received from remote Rad Sensor Node(s).
*/

// ----------------------------------------------------------------------------------------------------------------------------
//                                                                                                                    LIBRARIES

#include <RF24.h>          // RF24                                       http://nRF24.github.io/RF24
#include <SSD1306Wire.h>   // SSD1306Wire                                https://gitlab.com/alexpr0/ssd1306wire
#include <OLEDDisplayUi.h> // ESP8266 and ESP32 OLED driver for SSD1306  https://github.com/ThingPulse/esp8266-oled-ssd1306
#include <AESLib.h>        // AesLib                                     https://github.com/suculent/thinx-aes-lib

// ----------------------------------------------------------------------------------------------------------------------------
//                                                                                                                      DEFINES

#define CE_PIN              25 // radio can use tx
#define CSN_PIN             26 // radio can use rx
#define CIPHERBLOCKSIZE     32 // limited to 32 bytes inline with NRF24L01+ max payload bytes

// ----------------------------------------------------------------------------------------------------------------------------
//                                                                                                                       WIRING

SSD1306Wire   display(0x3c, SDA, SCL); // let SSD1306Wire wire up our SSD1306 on the i2C bus
OLEDDisplayUi ui(&display);            // plug display into OLEDDisplayUi
RF24          radio(CE_PIN, CSN_PIN);  // wire up RF24 TX and RX
AESLib        aesLib;

// ----------------------------------------------------------------------------------------------------------------------------

int REDLED_0 = 32; // rgb led:  32 red,  2 blue,  4 green.
int SOUND_0  = 33;

// ----------------------------------------------------------------------------------------------------------------------------
//                                                                                                                 RADIO STRUCT

struct RadioStruct {
  uint8_t       rx_bytes;
  uint8_t       rx_pipe;
  volatile bool broadcast        = true;
  bool          rf24_rx_report   = false;
  uint8_t       address[1024][6] = {"0Node", "1Node", "2Node", "3Node", "4Node", "5Node"};
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
  char          message[CIPHERBLOCKSIZE];
};
PayloadStruct payload;

// ----------------------------------------------------------------------------------------------------------------------------
//                                                                                                        GEIGER COUNTER STRUCT

struct GCStruct {
  signed long   CPM;           // stores counts per minute
  float         uSvh      = 0; // stores the micro-Sievert/hour for units of radiation dosing
  unsigned long maxPeriod = 5; // interval between sending demo command to sensor
  unsigned long gc_warn_0 = 100;
};
GCStruct geigerCounter;

// ----------------------------------------------------------------------------------------------------------------------------
//                                                                                                                  TIME STRUCT

struct TimeStruct {
  double        previousTimestamp;
  unsigned long mainLoopTimeTaken;
  unsigned long mainLoopTimeStart;
  double        subTime;
  double        subTimeDivided;
  double        interTimeDivided;
  unsigned long currentMilliSecond;
  unsigned long currentSecond;
  double        timestamp;
  double        interTime;
  double        previousTimestampSecond;
};
TimeStruct timeData;

// ----------------------------------------------------------------------------------------------------------------------------
//                                                                                                   OLED FRAME: GEIGER COUNTER

// frame to be displayed on ssd1306 182x64
void GC_Measurements(OLEDDisplay* display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  if (geigerCounter.CPM >= geigerCounter.gc_warn_0) { display->drawString(display->getWidth()/2, 0, "WARNING");}
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
  byte     aes_key[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; // AES encryption key (use your own)
  byte     aes_iv[16]  = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; // genreral initialization vector (use your own)
  byte     enc_iv[16]  = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; // iv_block gets written to
  byte     dec_iv[16]  = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; // iv_block gets written to
  char     cleartext[(unsigned long)(CIPHERBLOCKSIZE/2)] = {0};              // half the size of ciphertext
  char     ciphertext[CIPHERBLOCKSIZE] = {0};                                // twice the size of cleartext
  char     fingerprint[(unsigned long)(CIPHERBLOCKSIZE/2)];                  // a recognizable tag 
  char     tmp_cleartext[(unsigned long)(CIPHERBLOCKSIZE/2)];                // the same size of cleartext
  bool     fingerprintAccepted = false;
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
  radioData.rx_bytes = radio.getPayloadSize();

  // check payload size within limit
  if (radioData.rx_bytes <= sizeof(payload)) {
    
    // read the payload into the payload struct
    memset(payload.message, 0, sizeof(payload.message));
    radio.read(&payload, radioData.rx_bytes); // fetch payload from FIFO

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

  // geiger counter impulse
  if (strncmp( commandserver.messageCommand, "IMP", 3) == 0) {
    digitalWrite(SOUND_0, HIGH);
    digitalWrite(SOUND_0, HIGH);
    digitalWrite(REDLED_0, HIGH);
    delay(3);
    digitalWrite(REDLED_0, LOW);
    digitalWrite(SOUND_0, LOW);
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
//                                                                                                     FUNCTION: GEIGER COUNTER

void radNodeSensor0() {
  // a demo function to remote control / send data to a sensor node.

  // set current timestamp to be used this loop same millisecond+- depending on loop speed.
  timeData.timestamp = currentTime();

  // set previous time each minute
  if ((timeData.timestamp - timeData.previousTimestamp) > geigerCounter.maxPeriod) {
    Serial.print("cycle expired: "); Serial.println(timeData.timestamp, sizeof(timeData.timestamp));
    timeData.previousTimestamp = timeData.timestamp;

    // create transmission message
    memset(aes.cleartext, 0, sizeof(aes.cleartext));
    strcat(aes.cleartext, aes.fingerprint);
    strcat(aes.cleartext, "DATA");
    // set our writing pipe each time in case we write to different pipes another time
    radio.stopListening();
    radio.flush_tx();
    radio.openWritingPipe(radioData.address[0][1]);
    // encrypt and send
    cipherSend();
  }
}

// ----------------------------------------------------------------------------------------------------------------------------
//                                                                                                              FUNCTION: SETUP

void setup() {

  // --------------------------------------------------------------------------------------------------------------------------
  //                                                                                                              SETUP SERIAL

  Serial.begin(115200);
  while (!Serial) {
  }

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
  //                                                                                                       SETUP GEIGER COUNTER

  pinMode(REDLED_0, OUTPUT);
  pinMode(SOUND_0, OUTPUT);
  digitalWrite(SOUND_0, LOW);
  digitalWrite(REDLED_0, LOW);

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
  radio.setPayloadSize(sizeof(payload));   // 2x int datatype occupy 8 bytes
  radio.stopListening();
  radio.setChannel(124);          // 0-124 correspond to 2.4 GHz plus the channel number in units of MHz (ch 21 = 2.421 GHz)
  radio.setDataRate(RF24_2MBPS);  // RF24_250KBPS, RF24_1MBPS, RF24_2MBPS
  radio.setPALevel(RF24_PA_HIGH); // RF24_PA_MIN, RF24_PA_LOW, RF24_PA_HIGH, RF24_PA_MAX.
  Serial.println("Channel:  " + String(radio.getChannel()));
  Serial.println("Data Rate:" + String(radio.getDataRate()));
  Serial.println("PA Level: " + String(radio.getPALevel()));
  radio.startListening();

  // --------------------------------------------------------------------------------------------------------------------------
}

// ----------------------------------------------------------------------------------------------------------------------------
//                                                                                                                    MAIN LOOP

void loop() {

  // store current time to measure this loop time so we know how quickly items are added/removed from counts arrays
  timeData.mainLoopTimeStart = micros();

  // default to rx each loop
  radio.openReadingPipe(1, radioData.address[0][0]);
  radio.startListening();
  // get payload
  if (radio.available(&radioData.rx_pipe)) { // is there a payload? get the pipe number that recieved it
    // go through security
    if (cipherReceive() == true) {
      // go to central command
      centralCommand();
    }
  }

  // optionally send to a sensor node (requires sensor node is enabled to receive) uncomment to transmit to a sensor node (demo)
  radNodeSensor0();

  // refresh ssd1306 128x64 display
  ui.update();

  // store time taken to complete
  timeData.mainLoopTimeTaken = micros() - timeData.mainLoopTimeStart;
}
