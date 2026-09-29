/*
  Автоматична система поливу 5 секторів - Оновлена версія 2026
  Arduino Nano + 5x Soil Moisture + 5x Overflow + Servo + Relay + HTU21D
*/

#include <Wire.h>
#include <Servo.h>
#include "SparkFunHTU21D.h"

HTU21D myHumidity;
Servo myservo;

const int servopin = 12;
const int PIN_RELAY_wp1 = 2;
const int ReceiverOverflowPin = 3;

// Датчики переливу (HIGH = є вода / перелив)
const int PIN_WATER[6] = {0, 11, 10, 9, 8, 7};   

// Датчики ґрунту
const int PIN_SOIL[6] = {0, A0, A1, A2, A3, A6};

// =================== НАЛАШТУВАННЯ ===================
byte triggerAUTO[6] = {0, 35, 35, 35, 35, 35};     // % для кожного сектора
unsigned long timer_WATERING_AUTO = 10000;         // час поливу (змінюється: timer=15000)
unsigned long timer_STANDBY = 600000;              // 10 хвилин
unsigned long delay_SERVICE = 1800000;             // 30 хвилин

const int delay_servo_BF = 1000;
const int delay_servo_AF = 3000;

byte servo_position[6] = {0, 25, 60, 95, 135, 180};

// Калібрування
int sensor_MIN[6] = {0, 380, 258, 261, 274, 300};
int sensor_MAX[6] = {0, 592, 476, 498, 485, 500};

// Змінні
int RAW_val_w[6];
int mstLvl[6];
int Val_PERCENTAGE[6];

bool isServiceMode = false;
bool isWateringNow = false;

unsigned long lastStatusMillis = 0;
const unsigned long statusInterval_Standby = 60000;    // 60 сек
const unsigned long statusInterval_Watering = 2000;    // 2 сек

// =====================================================

void setup() {
  Serial.begin(9600);
  
  for(int i=1; i<=5; i++) {
    pinMode(PIN_WATER[i], INPUT_PULLUP);
  }
  pinMode(PIN_RELAY_wp1, OUTPUT);
  pinMode(ReceiverOverflowPin, INPUT);
  
  myservo.attach(servopin);
  digitalWrite(PIN_RELAY_wp1, LOW);
  
  myHumidity.begin();

  Serial.println(F("\n¯|_(ツ)_/¯---SYSTEM STARTED---¯|_(ツ)_/¯"));
  Serial.println(F("SWS4005 GROK EDIT.h"));

  printHelp();
  
  MEASUREMOISTURE();
  CALIBRATION();
  selfTest();
  SERMON_HUM_TEMP();
  SERMON_SERVO();
  SERMON_RUNTIME();
  SERMON_RAW();
}

void loop() {
  handleSerialCommands();
  
  if (isServiceMode) return;

  static unsigned long lastCheck = 0;
  unsigned long interval = isWateringNow ? 2000UL : 600000UL;
  
  if (millis() - lastCheck >= interval) {
    lastCheck = millis();
    
    MEASUREMOISTURE();
    CALIBRATION();
    
    if (!isWateringNow) {
      WATERING_AUTO();
    }
    
    // Періодичний вивід статусу
    if (millis() - lastStatusMillis >= (isWateringNow ? statusInterval_Watering : statusInterval_Standby)) {
      lastStatusMillis = millis();
      SERMON_STANDBY();
    }
  }
}

// ===================== SERIAL =====================
void handleSerialCommands() {
  static String command = "";
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (command.length() > 0) {
        processCommand(command);
        command = "";
      }
    } else {
      command += c;
    }
  }
}

void processCommand(String cmd) {
  cmd.trim();
  cmd.toLowerCase();

  if (cmd == "help") {
    printHelp();
  }
  else if (cmd == "a" || cmd == "") {
    isServiceMode = false;
    MEASUREMOISTURE();
    CALIBRATION();
    WATERING_AUTO();
  }
  else if (cmd == "s") {
    enterServiceMode();
  }
  else if (cmd == "0") {
    emergencyStop();
  }
  else if (cmd.startsWith("trigger")) {
    setTrigger(cmd);
  }
  else if (cmd.startsWith("timer=")) {
    timer_WATERING_AUTO = cmd.substring(6).toInt();
    Serial.print(F("New watering time: ")); 
    Serial.print(timer_WATERING_AUTO); 
    Serial.println(F(" ms"));
  }
  else if (cmd.length() == 1 && isDigit(cmd[0])) {
    int sec = cmd.toInt();
    if (sec >= 1 && sec <= 5) manualWatering(sec);
  }
  else if (cmd.startsWith("off")) {
    int sec = cmd.substring(3).toInt();
    if (sec >= 1 && sec <= 5) {
      digitalWrite(PIN_RELAY_wp1, LOW);
      Serial.print(F("\nSYSTEM_MODE_:_MAN")); Serial.print(sec);
      Serial.print(F("\nSECTOR_")); Serial.print(sec); Serial.println(F("_WATERING_CANCELLED"));
      SERMON_pumpState();
    }
  }
}

void printHelp() {
  Serial.println(F("\nCommands:"));
  Serial.println(F("1-5     → Manual watering"));
  Serial.println(F("off1-off5 → Stop sector"));
  Serial.println(F("a       → Auto mode"));
  Serial.println(F("s       → Service pause"));
  Serial.println(F("0       → Emergency stop"));
  Serial.println(F("triggerX=Y → ex: trigger1=30"));
  Serial.println(F("timer=XXXX → watering time in ms"));
}

// ===================== AUTO =====================
void WATERING_AUTO() {
  if (digitalRead(ReceiverOverflowPin) == HIGH) {
    handleReceiverFull();
    return;
  }

  int bestSector = -1;
  int lowestMoisture = 101;

  for (int i = 1; i <= 5; i++) {
    if (Val_PERCENTAGE[i] < triggerAUTO[i] && RAW_val_w[i] == LOW) {
      if (Val_PERCENTAGE[i] < lowestMoisture) {
        lowestMoisture = Val_PERCENTAGE[i];
        bestSector = i;
      }
    }
  }

  if (bestSector != -1) {
    waterSector(bestSector);
  } else {
    digitalWrite(PIN_RELAY_wp1, LOW);
  }
}

void waterSector(uint8_t sector) {
  if (sector < 1 || sector > 5) return;

  isWateringNow = true;
  Serial.print(F("\nSYSTEM_MODE_:_AUTOMATIC")); Serial.print(sector);
  
  digitalWrite(PIN_RELAY_wp1, LOW);
  delay(delay_servo_BF);
  myservo.write(servo_position[sector]);
  delay(delay_servo_AF);
  
  digitalWrite(PIN_RELAY_wp1, HIGH);
  SERMON_pumpState();

  unsigned long startWater = millis();
  while (millis() - startWater < timer_WATERING_AUTO) {
    if (digitalRead(PIN_WATER[sector]) == HIGH || digitalRead(ReceiverOverflowPin) == HIGH) {
      Serial.print(F(" - OVERFLOW detected!"));
      break;
    }
    delay(200);
  }

  digitalWrite(PIN_RELAY_wp1, LOW);
  isWateringNow = false;

  MEASUREMOISTURE();
  CALIBRATION();
  SERMON_sector(sector);
}

// ===================== MANUAL =====================
void manualWatering(uint8_t sector) {
  if (digitalRead(ReceiverOverflowPin) == HIGH || RAW_val_w[sector] == HIGH) return;
  waterSector(sector);
}

// ===================== SERVICE =====================
void enterServiceMode() {
  digitalWrite(PIN_RELAY_wp1, LOW);
  isServiceMode = true;
  Serial.print(F("\nPAUSED_FOR_SERVICE_ON: ")); 
  Serial.print(delay_SERVICE / 60000); 
  Serial.println(F(" min"));
  delay(delay_SERVICE);
  isServiceMode = false;
  Serial.println(F("SERVICE_MODE_ENDED"));
}

void emergencyStop() {
  digitalWrite(PIN_RELAY_wp1, LOW);
  Serial.println(F("\nPUMP_EMERGENCY_STOP"));
  SERMON_pumpState();
}

void handleReceiverFull() {
  Serial.print(F("\n!!!RECEIVER_FULL--PUMP_WAITING:"));
  Serial.print(delay_SERVICE/1000); Serial.println(F("s!!!"));
  digitalWrite(PIN_RELAY_wp1, LOW);
  SERMON_pumpState();
  delay(5000);
}

// ===================== SENSORS =====================
void MEASUREMOISTURE() {
  for(int i=1; i<=5; i++) {
    RAW_val_w[i] = digitalRead(PIN_WATER[i]);
  }

  const byte readings = 50;
  for(int i=1; i<=5; i++) {
    long sum = 0;
    for(byte j=0; j<readings; j++) {
      sum += analogRead(PIN_SOIL[i]);
      delay(10);
    }
    mstLvl[i] = sum / readings;
  }
}

void CALIBRATION() {
  for(int i=1; i<=5; i++) {
    Val_PERCENTAGE[i] = constrain(mstLvl[i], sensor_MIN[i], sensor_MAX[i]);
    Val_PERCENTAGE[i] = map(Val_PERCENTAGE[i], sensor_MIN[i], sensor_MAX[i], 100, 0);
  }
}

// ===================== SERMON FUNCTIONS (оригінальні) =====================
void SERMON_sector(uint8_t s) {
  switch(s) {
    case 1: SERMON1(); break;
    case 2: SERMON2(); break;
    case 3: SERMON3(); break;
    case 4: SERMON4(); break;
    case 5: SERMON5(); break;
  }
}

void SERMON_pumpState(){
  if (digitalRead(PIN_RELAY_wp1) == LOW) {
    Serial.print(F("\t(PUMP_STANDBY)"));
  } else {
    Serial.print(F("\t(PUMP_ACTIVE)"));
  }
}

void selfTest(){
  Serial.print(F("\n"));
  for(int i=1; i<=5; i++){
    if((mstLvl[i] > sensor_MAX[i]+20) || (mstLvl[i] < sensor_MIN[i]-20)){
      Serial.print(F("CHK_SOIL_S")); Serial.print(i); Serial.print(F("!!!\t"));
    } else {
      Serial.print(F("SOIL_S")); Serial.print(i); Serial.print(F("_OK\t"));
    }
  }
}

void SERMON_HUM_TEMP(){
  float humd = myHumidity.readHumidity();
  float temp = myHumidity.readTemperature();
  Serial.print(F("\nAIR_Temp:")); Serial.print(temp,1); Serial.print(F("°C"));
  Serial.print(F("\tAIR_Hum:")); Serial.print(humd,1); Serial.print(F("%"));
}

void SERMON_RUNTIME(){
  uint32_t sec = millis() / 1000ul; 
  int h = sec / 3600ul;
  int m = (sec % 3600ul) / 60ul;
  int s = sec % 60ul;
  Serial.print(F("\tRunTime:")); Serial.print(h); Serial.print(F("h"));
  Serial.print(m); Serial.print(F("m")); Serial.print(s); Serial.print(F("s"));
}

void SERMON_SERVO(){
  Serial.print(F("\tSERVO_is_Positioned_at:")); 
  Serial.print(myservo.read()); Serial.print(F("°")); 
}

void SERMON_RAW(){
  Serial.print(F("\nA0(S1):")); Serial.print(mstLvl[1]);
  Serial.print(F("\tA1(S2):")); Serial.print(mstLvl[2]);
  Serial.print(F("\tA2(S3):")); Serial.print(mstLvl[3]);
  Serial.print(F("\tA3(S4):")); Serial.print(mstLvl[4]);
  Serial.print(F("\tA6(S5):")); Serial.print(mstLvl[5]);
  Serial.print(F("\nD8(W1):")); Serial.print(RAW_val_w[1]);
  Serial.print(F("\tD9(W2):")); Serial.print(RAW_val_w[2]);
  Serial.print(F("\tD10(W3):")); Serial.print(RAW_val_w[3]);
  Serial.print(F("\tD11(W4):")); Serial.print(RAW_val_w[4]);
  Serial.print(F("\tD7(W5):")); Serial.print(RAW_val_w[5]);
  Serial.print(F("\n"));
}

void SERMON5(){
  Serial.print(F("\n\t\t\t\t\t\t\t\t-^-^-^-^-^-"));
  Serial.print(F("\n\t\t\t\t\t\t\t\tSOIL_5:***")); Serial.print(Val_PERCENTAGE[5]); Serial.print(F("%"));
  Serial.print(F("\n\t\t\t\t\t\t\t\tWATER_5:~~~"));
  Serial.print(RAW_val_w[5] == LOW ? F("N") : F("Y"));
  SERMON_RAW();
  Serial.print(F("\n\t\t\t\t\t\t\t\tTarget_5:")); Serial.print(triggerAUTO[5]); Serial.print(F("%"));
  SERMON_HUM_TEMP(); SERMON_SERVO(); SERMON_RUNTIME();
}

void SERMON4(){
  Serial.print(F("\n\t\t\t\t\t\t-^-^-^-^-^-"));
  Serial.print(F("\n\t\t\t\t\t\tSOIL_4:***")); Serial.print(Val_PERCENTAGE[4]); Serial.print(F("%"));
  Serial.print(F("\n\t\t\t\t\t\tWATER_4:~~~"));
  Serial.print(RAW_val_w[4] == LOW ? F("N") : F("Y"));
  SERMON_RAW();
  Serial.print(F("\n\t\t\t\t\t\tTarget_4:")); Serial.print(triggerAUTO[4]); Serial.print(F("%"));
  SERMON_HUM_TEMP(); SERMON_SERVO(); SERMON_RUNTIME();
}

void SERMON3(){
  Serial.print(F("\n\t\t\t\t-^-^-^-^-^-"));
  Serial.print(F("\n\t\t\t\tSOIL_3:***")); Serial.print(Val_PERCENTAGE[3]); Serial.print(F("%"));
  Serial.print(F("\n\t\t\t\tWATER_3:~~~"));
  Serial.print(RAW_val_w[3] == LOW ? F("N") : F("Y"));
  SERMON_RAW();
  Serial.print(F("\n\t\t\t\tTarget_3:")); Serial.print(triggerAUTO[3]); Serial.print(F("%"));
  SERMON_HUM_TEMP(); SERMON_SERVO(); SERMON_RUNTIME();
}

void SERMON2(){
  Serial.print(F("\n\t\t-^-^-^-^-^-"));
  Serial.print(F("\n\t\tSOIL_2:***")); Serial.print(Val_PERCENTAGE[2]); Serial.print(F("%"));
  Serial.print(F("\n\t\tWATER_2:~~~"));
  Serial.print(RAW_val_w[2] == LOW ? F("N") : F("Y"));
  SERMON_RAW();
  Serial.print(F("\n\t\tTarget_2:")); Serial.print(triggerAUTO[2]); Serial.print(F("%"));
  SERMON_HUM_TEMP(); SERMON_SERVO(); SERMON_RUNTIME();
}

void SERMON1(){
  Serial.print(F("\n-^-^-^-^-^-"));
  Serial.print(F("\nSOIL_1:***")); Serial.print(Val_PERCENTAGE[1]); Serial.print(F("%"));
  Serial.print(F("\nWATER_1:~~~"));
  Serial.print(RAW_val_w[1] == LOW ? F("N") : F("Y"));
  SERMON_RAW();
  Serial.print(F("\nTarget_1:")); Serial.print(triggerAUTO[1]); Serial.print(F("%"));
  SERMON_HUM_TEMP(); SERMON_SERVO(); SERMON_RUNTIME();
}

void SERMON_STANDBY(){
  Serial.print(F("\n----------\t----------\t----------\t----------\t----------"));
  for(int i=1; i<=5; i++){
    Serial.print(F("\nSOIL_")); Serial.print(i); Serial.print(F(":***"));
    Serial.print(Val_PERCENTAGE[i]); Serial.print(F("%"));
  }
  Serial.print(F("\nWATER_1:~~~")); Serial.print(RAW_val_w[1]==LOW?"N":"Y");
  Serial.print(F("\tWATER_2:~~~")); Serial.print(RAW_val_w[2]==LOW?"N":"Y");
  Serial.print(F("\tWATER_3:~~~")); Serial.print(RAW_val_w[3]==LOW?"N":"Y");
  Serial.print(F("\tWATER_4:~~~")); Serial.print(RAW_val_w[4]==LOW?"N":"Y");
  Serial.print(F("\tWATER_5:~~~")); Serial.print(RAW_val_w[5]==LOW?"N":"Y");
  
  SERMON_RAW();
  Serial.print(F("\nTarget_1:")); Serial.print(triggerAUTO[1]); Serial.print(F("%\t"));
  Serial.print(F("Target_2:")); Serial.print(triggerAUTO[2]); Serial.print(F("%\t"));
  Serial.print(F("Target_3:")); Serial.print(triggerAUTO[3]); Serial.print(F("%\t"));
  Serial.print(F("Target_4:")); Serial.print(triggerAUTO[4]); Serial.print(F("%\t"));
  Serial.print(F("Target_5:")); Serial.print(triggerAUTO[5]); Serial.print(F("%"));
  
  SERMON_HUM_TEMP(); SERMON_SERVO(); SERMON_RUNTIME();
}

void setTrigger(String cmd) {
  int eq = cmd.indexOf('=');
  if (eq == -1) return;
  String num = cmd.substring(7, eq);
  int sector = num.toInt();
  int value = cmd.substring(eq+1).toInt();
  
  if (sector >=1 && sector <=5 && value >=0 && value <=100) {
    triggerAUTO[sector] = value;
    Serial.print(F("Trigger sector ")); Serial.print(sector);
    Serial.print(F(" set to ")); Serial.print(value); Serial.println(F("%"));
  }
}
