#include <Wire.h>
#include <ESP32Servo.h>
#include <DFRobotDFPlayerMini.h>
#include <HX711_ADC.h>

#define SLAVE_ADDRESS 9
#define ACTIVATE_SERVO_AND_JINGLE 1
#define PLAY_JINGLE_ONLY 2
#define ROTATE_SERVO_ONLY 3

#define HX711_DOUT 4
#define HX711_SCK 5

HX711_ADC LoadCell(HX711_DOUT, HX711_SCK);
const int calVal_eepromAddress = 0;
unsigned long t = 0;

Servo myServo;
DFRobotDFPlayerMini myDFPlayer;

const float calibrationFactor = 423;
bool isDispensing = false;
bool dispensingComplete = false;

void setup() {
  Serial.begin(115200);
  Wire.begin(SLAVE_ADDRESS);
  Wire.onReceive(receiveEvent);
  setupServo();
  setupDFPlayer();
  setupLoadCell();
}

void setupServo() {
  myServo.attach(18); 
  myServo.write(0);
  delay(1000); 
}

void setupDFPlayer() {

  Serial2.begin(9600, SERIAL_8N1, 16, 17);
  if (!myDFPlayer.begin(Serial2)) {
    Serial.println("Unable to begin DFPlayer. Check connections.");
    while (true);
  }
  myDFPlayer.volume(100);
  Serial.println("DFPlayer Mini ready!");
}

// void setupLoadCell() {
//   Serial.println("Starting load cell...");

//   LoadCell.begin();
//   unsigned long stabilizingtime = 2000;
//   boolean _tare = true;
//   LoadCell.start(stabilizingtime, _tare);
//   if (LoadCell.getTareTimeoutFlag() || LoadCell.getSignalTimeoutFlag()) {
//     Serial.println("Timeout, check wiring and pin designations");
//     while (1);
//   } else {
//     LoadCell.setCalFactor(1.0);
//     Serial.println("Load cell ready.");
//   }
//   while (!LoadCell.update());
//   calibrate();
// }

void setupLoadCell() {
  Serial.println("Starting Load Cell...");

  LoadCell.begin();
  unsigned long stabilizingtime = 2000;
  bool _tare = true;
  LoadCell.start(stabilizingtime, _tare);
  
  if (LoadCell.getTareTimeoutFlag() || LoadCell.getSignalTimeoutFlag()) {
    Serial.println("Timeout, check wiring and pin designations");
    while (1);
  } else {
    LoadCell.setCalFactor(calibrationFactor); // Use predetermined calibration factor
    Serial.println("Load Cell setup complete with predetermined calibration.");
  }
}

void calibrate() {
  Serial.println("***");
  Serial.println("Start calibration:");
  Serial.println("Place the load cell on a stable surface.");
  Serial.println("Remove any load applied to the load cell.");
  Serial.println("Send 't' from serial monitor to set the tare offset.");

  boolean _resume = false;
  while (!_resume) {
    LoadCell.update();
    if (Serial.available() > 0) {
      char inByte = Serial.read();
      if (inByte == 't') LoadCell.tareNoDelay();
    }
    if (LoadCell.getTareStatus()) {
      Serial.println("Tare complete");
      _resume = true;
    }
  }

  Serial.println("Now, place your known mass on the load cell.");
  Serial.println("Send the weight of this mass (e.g., 100.0) from serial monitor.");

  float known_mass = 0;
  _resume = false;
  while (!_resume) {
    LoadCell.update();
    if (Serial.available() > 0) {
      known_mass = Serial.parseFloat();
      if (known_mass != 0) {
        Serial.print("Known mass is: ");
        Serial.println(known_mass);
        _resume = true;
      }
    }
  }

  LoadCell.refreshDataSet();
  float newCalibrationValue = LoadCell.getNewCalibration(known_mass);

  Serial.print("New calibration value set to: ");
  Serial.print(newCalibrationValue);
  Serial.println(", use this as calFactor in your project.");

  Serial.println("End calibration.");
}

void receiveEvent(int bytes) {
  if (Wire.available() > 0) {
    int command = Wire.read();
    
    if (command == ACTIVATE_SERVO_AND_JINGLE  && Wire.available() > 0) {
      int grams = Wire.read();
      Serial.println("Received command to dispense " + String(grams) + "grams and play jingle...");
      dispenseFood(grams);
      playJingle();
    } 
    else if (command == PLAY_JINGLE_ONLY) {
      playJingle();
    } 
    else if (command == ROTATE_SERVO_ONLY && Wire.available() > 0) {
      int grams = Wire.read();
      Serial.print("Received command to dispense ");
      Serial.print(grams);
      Serial.println(" grams.");
      dispenseFood(grams);
    } 
  }
}

void dispenseFood(int targetWeight) {
  isDispensing = true;
  dispensingComplete = false;

  float initialWeight = getWeight();
  float desiredWeight = initialWeight + targetWeight; 

  Serial.print("Dispensing food. Initial weight: ");
  Serial.print(initialWeight);
  Serial.print(", Target weight: ");
  Serial.println(desiredWeight);

  myServo.write(80); 
  Serial.println("Servo opened.");
  
  while (true) {
    float currentWeight = getWeight();
    Serial.print("Current weight: ");
    Serial.println(currentWeight);

    if (currentWeight >= desiredWeight) {
      break;
    }
  }

  myServo.write(0);   
  Serial.println("Servo closed.");
  Serial.println("Desired weight dispensed.");
  isDispensing = false;
  dispensingComplete = true;
}

float getWeight() {
  delay(200);
  if (LoadCell.update()) {
    float weight = LoadCell.getData();
    return weight;
  } 
  return 0; 
}

void playJingle() {
  myDFPlayer.play(1);
  Serial.println("Playing jingle...");
}

void loop() {
  if (!isDispensing){
    Serial.println("Current weight: " + String(getWeight()) + " grams");
  }
}
