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
//                                                                                                                   LED STRUCT

/*
organize any leds all into one place. the leds have not been too specifically named so they can be repurposed when necessary.
*/

struct LEDStruct {
  int R_LED_0 = 32;
  int G_LED_0 = 4;
  int B_LED_0 = 2;
};
LEDStruct ledData;

// ----------------------------------------------------------------------------------------------------------------------------
//                                                                                                                 SOUND STRUCT

/*
organize any sound modules into one place. they have not been too specifically named so they can be repurposed when necessary.
*/

struct SoundStruct {
  int SOUND_0  = 33;
};
SoundStruct soundData;

// ----------------------------------------------------------------------------------------------------------------------------
//                                                                                                                 RADIO STRUCT

/*
organize radio related data into one place. note that payload data although related, is intentionally a seperate struct because
the payload struct is actually intended to be sent and received in entirety through any entrances and exits to be more easily
digestable on either end, while this radio struct is simply for organizing various radio related data. 
*/

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

/*
incoming payloads if accepted are parsed before finally reaching message command. this is the last stop for payload values
trying to reach central command. for clarity, these values are intentionally seperate from other data structures and any values
reaching this struct should be properly tested first, for security reasons so that we can try to secure and own central command. 
*/

struct CommandServerStruct {
  char messageCommand[(unsigned long)(CIPHERBLOCKSIZE/2)];
  char messageValue[(unsigned long)(CIPHERBLOCKSIZE/2)];
};
CommandServerStruct commandserver;

// ----------------------------------------------------------------------------------------------------------------------------
//                                                                                                               PAYLOAD STRUCT

/*
the first and last stop for any incoming / outgoing data over RF24 and could be used for structuring data to be sent / received
by other means, for consistent data structuring in relation to incoming / outgoing traffic accross all entrances and exits. 
*/

struct PayloadStruct {
  unsigned long payloadID;
  char          message[CIPHERBLOCKSIZE];
};
PayloadStruct payload;

// ----------------------------------------------------------------------------------------------------------------------------
//                                                                                                           SENSOR ZERO STRUCT

/*
stores data related to a specific sensor
*/

struct SensorNodeZeroSctruct {
  int MAX_PERIOD = 3;
};
SensorNodeZeroSctruct sensorZeroData;

// ----------------------------------------------------------------------------------------------------------------------------
//                                                                                                                  TIME STRUCT

/*
stores information in regards to time and timestamps generated by functions that provide potentially subsecond timestamps we can
create and use quickly, rather than creating a subsecond timestamp from an RTC datetime timestamp which proved to be much slower.
this way we just generate a timestamp as we need it and if we need datetime for something then we can always use an RTC too.
*/

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
//                                                                                                SSD1306 FRAME: GEIGER COUNTER

/*
this method of writing to the SSD1306 as provided in the library example, refreshes the display very satisfactorily and is far
superior to clearing parts of the screen or indeed the whole screen manually (display.cls()) prior to writing to the display.
*/

void GC_Measurements(OLEDDisplay* display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  display->drawString(display->getWidth()/2, 0, "RAD COMMAND SERVER");
}
FrameCallback frames[] = { GC_Measurements }; // array keeps function pointers to all frames are the single views that slide in
int frameCount = 1;

// ----------------------------------------------------------------------------------------------------------------------------
//                                                                                                 ADVANCED ENCRYPTION STANDARD 

/*
we would like to have some confidence that the data we are receiving is the data we intend to be receiving, although nothing is
secure, we should implement some degree(s) of security to try and secure our systems. a good place to start is encryption. the
intention here is to obfuscate commands and the fingerprint with encryption so that when we decrypt a payload, we can check for
a known fingerprint inside and if there is then we can further process the payload, helping us try to secure the command center. 
*/

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
AESStruct aesData;

void aes_init() {
  aesLib.gen_iv(aesData.aes_iv);
  aesLib.set_paddingmode((paddingMode)0);
}
void encrypt(char * msg, byte iv[]) {
  aesData.msgLen = strlen(msg);
  memset(aesData.ciphertext, 0, sizeof(aesData.ciphertext));
  aesLib.encrypt64((const byte*)msg, aesData.msgLen, aesData.ciphertext, aesData.aes_key, sizeof(aesData.aes_key), iv);
}
void decrypt(char * msg, byte iv[]) {
  aesData.msgLen = strlen(msg);
  memset(aesData.cleartext, 0, sizeof(aesData.cleartext));
  aesData.plain_len = aesLib.decrypt64(msg, aesData.msgLen, (byte*)aesData.tmp_cleartext, aesData.aes_key, sizeof(aesData.aes_key), iv);
  strncpy(aesData.cleartext, aesData.tmp_cleartext, aesData.plain_len);
}

// ----------------------------------------------------------------------------------------------------------------------------
//                                                                                                       FUNCTION: CURRENT TIME

/*
this function intends to return a timestamp. note that the timestamp returned is not intended to be datetime but instead an
accumulation of micros from the point at which the main loop stated running. this proved to be faster than requesting datetime
as a timestamp from an rtc in which would also need to be appended with some unit of sub second time to get the most out of it.  
*/

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

/*
a time between times function used to create timestamps related to current time, without setting current time as intertime.
*/

double interCurrentTime() {
  timeData.interTime = (micros() - timeData.mainLoopTimeStart);
  timeData.interTimeDivided = timeData.interTime / 1000000;
  return timeData.timestamp+timeData.interTimeDivided;
}

// ----------------------------------------------------------------------------------------------------------------------------
//                                                                                                     FUNCTION: CIPHER RECEIVE

/*
ultimately an attempt to filter out any unwanted traffic from our intended traffic so that only our intended traffic should
reach the command center. nothing is secure but we should implement some degree(s) of security to attempt to be more secure.
*/

bool cipherReceive() {
  Serial.println("------------------------------------------------------------------------------------------------------------");

  // ensure false
  aesData.fingerprintAccepted = false;

  // read the payload into payload struct
  radioData.rx_bytes = radio.getPayloadSize();

  // check payload size within limit
  if (radioData.rx_bytes <= sizeof(payload)) {
    
    // read the payload into the payload struct
    memset(payload.message, 0, sizeof(payload.message));
    radio.read(&payload, radioData.rx_bytes); // fetch payload from FIFO

    // display raw payload
    Serial.println("[Receiving]");
    Serial.print("[payload.payloadID]          "); Serial.println(payload.payloadID);
    Serial.print("[payload.message]            "); Serial.println(payload.message); 
    Serial.print("[Bytes(payload.message)]     "); Serial.println(strlen(payload.message));

    // deccrypt (does not matter if not encrypted because we are only interested in encrypted payloads. turn anything else to junk)
    decrypt((char*)payload.message, aesData.dec_iv);

    // if accepted fingerprint 
    if ((strncmp(aesData.cleartext, aesData.fingerprint, strlen(aesData.fingerprint)-1 )) == 0) {
      aesData.fingerprintAccepted = true;

      // seperate fingerprint from the rest of the payload message ready for command parse
      memset(commandserver.messageCommand, 0, sizeof(commandserver.messageCommand));
      strncpy(commandserver.messageCommand, aesData.cleartext + strlen(aesData.fingerprint), strlen(aesData.cleartext) - strlen(aesData.fingerprint));
      Serial.print("[Command]                    "); Serial.println(commandserver.messageCommand);
    }

    // display payload information after decryption
    Serial.print("[aesData.cleartext]          "); Serial.println(aesData.cleartext); 
    Serial.print("[Bytes(aesData.cleartext)]   "); Serial.println(strlen(aesData.cleartext));
    Serial.print("[fingerprint]                "); Serial.println(aesData.fingerprintAccepted);
  }
  return aesData.fingerprintAccepted;
}

// ----------------------------------------------------------------------------------------------------------------------------
//                                                                                                        FUNCTION: CIPHER SEND

/*
before calling cipher send, first populate aesData.cleartext with data you wish to encrypt and send, cipherSend will do the rest.
be sure to also stopListening and specify the desired writing pipe and address. the proceedure is as follows to cipher send:

1: create transmission message
  memset(aesData.cleartext, 0, sizeof(aesData.cleartext));
  strcat(aesData.cleartext, aesData.fingerprint);
  strcat(aesData.cleartext, "DATA");

2: set desired writing pipe and address
  radio.stopListening();
  radio.flush_tx();
  radio.openWritingPipe(radioData.address[0][1]);

3: encrypt and send by simply calling cipher send
  cipherSend();
*/

void cipherSend() {
  Serial.println("------------------------------------------------------------------------------------------------------------");

  // keep payloadID well withing its cast
  payload.payloadID++;
  // limit to an N digit number so we always know how many bytes we have left in the payload
  if (payload.payloadID > 9) {payload.payloadID = 0;}

  // display raw payload
  Serial.println("[Sending]");
  Serial.print("[payload.payloadID]          "); Serial.println(payload.payloadID);
  Serial.print("[aesData.cleartext]          "); Serial.println(aesData.cleartext);
  Serial.print("[Bytes(aesData.cleartext)]   "); Serial.println(strlen(aesData.cleartext));
  // encrypt and load the encrypted data into the payload
  encrypt((char*)aesData.cleartext, aesData.enc_iv);
  memset(payload.message, 0, sizeof(payload.message));
  memcpy(payload.message, aesData.ciphertext, sizeof(aesData.ciphertext));

  // display payload information after encryption
  Serial.print("[payload.message]            "); Serial.println(payload.message); 
  Serial.print("[Bytes(aesData.ciphertext)]  "); Serial.println(strlen(aesData.ciphertext));

  // send
  Serial.print("[Bytes(payload.message)]     "); Serial.println(strlen(payload.message));
  radioData.rf24_rx_report = false;
  radioData.rf24_rx_report = radio.write(&payload, sizeof(payload));
  Serial.print("[Payload Delivery]           "); Serial.println(radioData.rf24_rx_report);
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
//                                                                                                     FUNCTION: COMMAND A NODE

/*
send commands to a remote sensor node. naturally we wish to receive information from our remote sensor node and this function
demonstrates that we are also setup to transmit to those nodes in the same way they can transmit to us. it may not always be
desirable to have a sensor node setup to receive, however we would like this setup to provide that capability by simply
uncommenting the receiver block in the sensor nodes main loop so that we can easily make that sensor remotely responsive if the
given sensor node being built requires being remote controlled for example.
*/

void sensorZero() {

  // example transmission to a sensor node:

  // set current timestamp to be used this loop same millisecond+- depending on loop speed.
  timeData.timestamp = currentTime();

  // set previous time each minute
  if ((timeData.timestamp - timeData.previousTimestamp) > sensorZeroData.MAX_PERIOD) {
    Serial.print("cycle expired: "); Serial.println(timeData.timestamp, sizeof(timeData.timestamp));
    timeData.previousTimestamp = timeData.timestamp;

    // create transmission message
    memset(aesData.cleartext, 0, sizeof(aesData.cleartext));
    strcat(aesData.cleartext, aesData.fingerprint);
    strcat(aesData.cleartext, "DATA");
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

  pinMode(ledData.R_LED_0, OUTPUT);
  pinMode(soundData.SOUND_0, OUTPUT);
  digitalWrite(soundData.SOUND_0, LOW);
  digitalWrite(ledData.R_LED_0, LOW);

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
  strcpy(aesData.fingerprint, "iD:");

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

  // store current time to measure this loop time
  timeData.mainLoopTimeStart = micros();

  // ----------------------------------------------------------------------------------------------------------------------------
  //                                                                                     COLLECT AND PROCESS RECEIVED SENSOR DATA

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

  // ----------------------------------------------------------------------------------------------------------------------------
  //                                                                                              OPTIONAL RESPONSIVE SENSOR NODE

  /*
  optional because a sensor node does not have to receive but it can. for security reasons you may desire your sensor node to
  only tx, however if it is required that a sensor node be remotely responsive then that can easily be achieved in this setup by
  uncommenting the following function call or making your own function to call:
  */

  sensorZero(); // uncomment to call transmit to sensor node demo function

  // ----------------------------------------------------------------------------------------------------------------------------

  // refresh SSD1306 128x64 display
  ui.update();

  // store time taken to complete
  timeData.mainLoopTimeTaken = micros() - timeData.mainLoopTimeStart;
}
