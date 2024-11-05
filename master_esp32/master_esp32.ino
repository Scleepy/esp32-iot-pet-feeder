#include <WiFi.h>
#include <FirebaseClient.h>
#include <WiFiClientSecure.h>
#include <TimeLib.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <SPI.h>
#include <MFRC522.h>
#include "UUID.h"

#define WIFI_SSID "SSID"
#define WIFI_PASSWORD "PASSWORD"

#define DATABASE_URL "xxx"
#define COMMAND_MANUAL_DISPENSE_PATH "/commands/dispense"
#define COMMAND_MANUAL_RFID_DISPENSE_PATH "/commands/activeRFIDFeedingId"
#define PET_PROFILES_PATH "/petProfiles/"
#define PET_FEEDING_HISTORY_PATH "/petFeedingHistory/"
#define FOOD_LEVEL_PATH "/feedingData/foodLevel"
#define DAILY_GRAMS_PATH "/feedingData/dailyGrams/"
#define MANUAL_FEEDING_VALUE_PATH "/commands/manualFeedingValue/"
#define SCHEDULE_FEEDING_VALUE_PATH "/commands/scheduleFeedingValue/"

#define TRIG_PIN 15
#define ECHO_PIN 4

WiFiClientSecure ssl;
DefaultNetwork network;
AsyncClientClass client(ssl, getNetwork(network));

FirebaseApp app;
RealtimeDatabase Database;
AsyncResult result;
NoAuth noAuth;

WiFiUDP udp;
NTPClient ntp(udp, "pool.ntp.org", 7 * 3600, 60000);

#define SLAVE_ADDRESS 9

// MFRC522
#define RST_PIN 16
#define SS_PIN 19
#define MISO_PIN 17
#define MOSI_PIN 5  
#define SCK_PIN 18 

#define TUBE_HEIGHT 8.0

MFRC522 rfid(SS_PIN, RST_PIN);

String activeRFIDFeedingTime = "";
String activeRFIDFeedingId = "";
unsigned long lastJingleTime = 0;
const unsigned long jingleInterval = 20000;
#define feedingWindow 2

unsigned long dispenseStartTime = 0;
bool dispenseStarted = false;

UUID uuid;

void setup() {
  Serial.begin(115200);
  Wire.begin();

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.print("\nConnected with IP: ");
  Serial.println(WiFi.localIP());
  Serial.println();

  ntp.begin(); 

  setupFirebase();
  setupRFID();
  setFoodLevel();
  setupUUID();
}

void setupUUID(){
  uint32_t seed1 = random(999999999);
  uint32_t seed2 = random(999999999);

  uuid.seed(seed1, seed2);
}

void setupRFID(){
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
  rfid.PCD_Init();
  Serial.println("RFID Reader Ready!");
}

void setupFirebase() {
  Firebase.printf("Firebase Client v%s\n", FIREBASE_CLIENT_VERSION);
  ssl.setInsecure();
  initializeApp(client, app, getAuth(noAuth));
  app.getApp<RealtimeDatabase>(Database);
  Database.url(DATABASE_URL);
  client.setAsyncResult(result);
  Serial.print("Listening for commands at: ");
  Serial.println(COMMAND_MANUAL_DISPENSE_PATH);
  Serial.println(COMMAND_MANUAL_RFID_DISPENSE_PATH);
}

void printError(int code, const String &msg) {
  Firebase.printf("Error, msg: %s, code: %d\n", msg.c_str(), code);
}

float getFoodLevel() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH);
  float distance = duration * 0.034 / 2; 

  float foodLevel = TUBE_HEIGHT - distance;

  if (foodLevel < 0) foodLevel = 0; 
  if (foodLevel > TUBE_HEIGHT) foodLevel = TUBE_HEIGHT; 

  return foodLevel;
}

String checkFoodLevelLeft() {
  float foodLevel = getFoodLevel();
  float percentageLeft = (foodLevel / TUBE_HEIGHT) * 100;
  Serial.println("Food level: " + String(foodLevel) + " cm, Percentage left: " + String(percentageLeft) + "%");
  return String(percentageLeft) + "%"; 
}

void setFoodLevel() {
  String currentFoodLevelPercentage = checkFoodLevelLeft(); 
  if (Database.set(client, FOOD_LEVEL_PATH, currentFoodLevelPercentage)) {
    Serial.println("Food level updated to: " + currentFoodLevelPercentage);
  } else {
    Serial.println("Failed to update food level in the database.");
  }
}

void checkDispenseCommand() {
  bool dispenseVal = Database.get<bool>(client, COMMAND_MANUAL_DISPENSE_PATH);
  if (client.lastError().code() == 0) {
    // Serial.println("Dispense Value: " + String(dispenseVal));

    if (dispenseVal) {
      Serial.println("Dispense command received. Running the servo and playing jingle...");
      triggerJingleAndDispense(1);

      Database.set(client, COMMAND_MANUAL_DISPENSE_PATH, false);
    }
  } else {
    printError(client.lastError().code(), client.lastError().message());
  }
}

String getCurrentTime(){
  int currentHour = ntp.getHours();
  int currentMinute = ntp.getMinutes();
  char buffer[6]; 
  snprintf(buffer, sizeof(buffer), "%02d:%02d", currentHour, currentMinute);
  return String(buffer);
}

void checkFeedingSchedule() {
  String feedTimesPath = "/feedTimes/";
  String feedTimesData = Database.get<String>(client, feedTimesPath);
  if (client.lastError().code() == 0) {
    ntp.update();

    DynamicJsonDocument doc(2048); 
    DeserializationError error = deserializeJson(doc, feedTimesData);

    if (error) {
      Serial.print(F("Deserialize failed: "));
      Serial.println(error.f_str());
      return;
    }

    time_t epochTime = ntp.getEpochTime();
    struct tm *ptm = gmtime ((time_t *)&epochTime);

    char formattedDateTime[20];
    sprintf(formattedDateTime, "%04d-%02d-%02d", 
            ptm->tm_year + 1900,
            ptm->tm_mon + 1,
            ptm->tm_mday);

    String currentDate = String(formattedDateTime);

    for (JsonPair kv : doc.as<JsonObject>()) {
      String feedTimeId = kv.key().c_str();
      JsonObject feedTimeObject = kv.value().as<JsonObject>();

      String feedTime = feedTimeObject["time"].as<String>();
      bool isActive = feedTimeObject["isActive"].as<bool>();
      bool isRFID = feedTimeObject["isRFID"].as<bool>();
      String lastTriggerDate = feedTimeObject["lastTriggerDate"].as<String>();

      if (!lastTriggerDate.isEmpty() && lastTriggerDate.startsWith(currentDate)) {
        //Serial.println("Already triggered today. Skipping...");
        continue;
      }

      String currentTime = getCurrentTime();
    
      if (feedTime == currentTime && isActive) {
        Serial.println("It's feeding time! Triggering dispenser...");
        String updatePath = feedTimesPath + feedTimeId + "/lastTriggerDate"; 

        if (Database.set(client, updatePath, currentDate)) {
          Serial.print("Last Trigger Date Set To: ");
          Serial.println(currentDate);
        } else {
          Serial.print("Failed to set last trigger date: ");
        }
        
        if (!isRFID) {
          triggerJingleAndDispense(2);
        } else {
          uuid.generate();
          String activeId = uuid.toCharArray(); 

          String newFeedingData = "{\"petFeedingHistoryId\": \"" + activeId + "\", " +
                                  "\"feedTimeId\": \"" + feedTimeId + "\", " +
                                  "\"petId\": [], " +
                                  "\"feedTypeName\": \"" + "Schedule RFID" + "\"}"; 

          String newFeedingPath = PET_FEEDING_HISTORY_PATH + activeId;
          Database.set<object_t>(client, newFeedingPath, object_t(newFeedingData));

          Serial.println("Scheduled RFID feeding triggered. Playing jingle & opening a " + String(feedingWindow) + " minute window for feeding");
          activeRFIDFeedingTime = getCurrentTime();
          activeRFIDFeedingId = activeId;
        }
      }
    }
  } else {
    printError(client.lastError().code(), client.lastError().message());
  }
}

void triggerJingleAndDispense(int mode) {
  String weightStr = ""; 

  if (mode == 1) {
    weightStr = Database.get<String>(client, MANUAL_FEEDING_VALUE_PATH);
  } else {
    weightStr = Database.get<String>(client, SCHEDULE_FEEDING_VALUE_PATH);
  }

  if (client.lastError().code() == 0) {
    if (weightStr.length() > 0) {
      int weight = weightStr.toInt();

      if (weight > 0) {
        Wire.beginTransmission(SLAVE_ADDRESS);
        Wire.write(1);
        Wire.write(weight);
        Wire.endTransmission();

        setDailyGrams(weight);
        dispenseStartTime = millis();
        dispenseStarted = true;
      } else {
        Serial.println("Invalid manual weight value.");
      }
    } else {
      Serial.println("Weight value is empty or invalid.");
    }
  } else {
    printError(client.lastError().code(), client.lastError().message());
  }
}

void triggerJingleOnly() {
  Wire.beginTransmission(SLAVE_ADDRESS);
  Wire.write(2);
  Wire.endTransmission();
}

void triggerDispenseOnly(float weight) {
  Wire.beginTransmission(SLAVE_ADDRESS);
  Wire.write(3);
  Wire.write((int)weight);
  Wire.endTransmission();

  setDailyGrams((int)weight);
  dispenseStartTime = millis();
  dispenseStarted = true;
}

void checkManualRFIDDispenseCommand() {
  String activeId = Database.get<String>(client, COMMAND_MANUAL_RFID_DISPENSE_PATH);
  if (client.lastError().code() == 0) {
    if (activeId != "") {
      Serial.println("Manual RFID feeding triggered. Playing jingle & opening a " + String(feedingWindow) + " minute window for feeding");
      activeRFIDFeedingTime = getCurrentTime();
      activeRFIDFeedingId = activeId;

      Database.set<String>(client, COMMAND_MANUAL_RFID_DISPENSE_PATH, "");
    }
  } else {
    printError(client.lastError().code(), client.lastError().message());
  }
}

void triggerAutoJingle() {
  if (activeRFIDFeedingTime != "") {
    String currentTime = getCurrentTime();

    if (checkWithinTimeWindow(currentTime, activeRFIDFeedingTime, feedingWindow)) {
      if (lastJingleTime == 0 || millis() - lastJingleTime >= jingleInterval) {
        triggerJingleOnly();
        lastJingleTime = millis();
        Serial.println("Jingle played as part of active RFID feeding window.");
      }
    } else {
      Serial.println("RFID feeding time window expired. Resetting values.");

      activeRFIDFeedingTime = "";
      activeRFIDFeedingId = "";
      lastJingleTime = 0;
    }
  }
}

bool checkWithinTimeWindow(String currentTime, String startTime, int windowMinutes) {
  int currentHour = currentTime.substring(0, 2).toInt();
  int currentMinute = currentTime.substring(3, 5).toInt();
  int startHour = startTime.substring(0, 2).toInt();
  int startMinute = startTime.substring(3, 5).toInt();

  int currentTotalMinutes = currentHour * 60 + currentMinute;
  int startTotalMinutes = startHour * 60 + startMinute;

  return (currentTotalMinutes >= startTotalMinutes) && (currentTotalMinutes <= startTotalMinutes + windowMinutes);
}

void checkRFIDPresent() {
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    String tagId = "";
    for (byte i = 0; i < rfid.uid.size; i++) {
      tagId += String(rfid.uid.uidByte[i], HEX); 
    }
    Serial.print("Tag ID: ");
    Serial.println(tagId);
    rfid.PICC_HaltA();

    String petProfilesData = Database.get<String>(client, PET_PROFILES_PATH);

    if (client.lastError().code() == 0) {
      DynamicJsonDocument doc(2048);
      DeserializationError error = deserializeJson(doc, petProfilesData);

      if (error) {
        Serial.print(F("Deserialize failed: "));
        Serial.println(error.f_str());
        return;
      }

      if (!doc.containsKey(tagId)) {
        String newProfileData = "{\"tagId\": \"" + tagId + "\", \"petName\": \"\", \"dispenseAmount\": 0, \"isRegistered\": false}";

        String newProfilePath = PET_PROFILES_PATH + tagId;
        Database.set<object_t>(client, newProfilePath, object_t(newProfileData));

        if (client.lastError().code() == 0) {
          Serial.println("New pet profile added to database.");
        } else {
          Serial.println("Error while adding profile:");
          printError(client.lastError().code(), client.lastError().message());
        }
      } else {
        bool isRegistered = doc[tagId]["isRegistered"];
        int dispenseAmount = doc[tagId]["dispenseAmount"];

        if (!isRegistered) {
          Serial.println("Tag not registered. Register the pet first.");
          return;
        }

        String currentTime = getCurrentTime();

        if (currentTime == activeRFIDFeedingTime || checkWithinTimeWindow(currentTime, activeRFIDFeedingTime, feedingWindow)) {
          String getPath = PET_FEEDING_HISTORY_PATH + activeRFIDFeedingId;
          String feedingHistoryData = Database.get<String>(client, getPath);

          bool petAlreadyFed = false;

          if (client.lastError().code() == 0) {
            String fedPetIds = feedingHistoryData;

            DynamicJsonDocument historyDoc(2048);
            error = deserializeJson(historyDoc, feedingHistoryData);

            if (error) {
              Serial.print(F("Deserialize failed: "));
              Serial.println(error.f_str());
              return;
            }

            String petIdsList = "";
            bool petAlreadyFed = false;

            if (historyDoc.containsKey("petId")) {
                JsonArray petIds = historyDoc["petId"].as<JsonArray>();
                
              for (JsonVariant petId : petIds) {
                if (petId.as<String>() == tagId) {
                  petAlreadyFed = true;
                  break;
                }
                petIdsList += "\"" + petId.as<String>() + "\",";
              }
            }

            if (petAlreadyFed) {
              Serial.println("Pet has already been fed in this feeding window. No additional feed dispensed.");
            } else {
              petIdsList += "\"" + tagId + "\"";
              String updatedPetIds = "[" + petIdsList + "]";

              String updatePath = PET_FEEDING_HISTORY_PATH + activeRFIDFeedingId + "/petId";
              Database.set<object_t>(client, updatePath, object_t(updatedPetIds));

              if (client.lastError().code() == 0) {
                Serial.print("Dispensing ");
                Serial.print(dispenseAmount);
                Serial.println(" grams of food for pet.");

                triggerDispenseOnly(dispenseAmount);
              } else {
                Serial.println("Error while updating feeding history:");
                printError(client.lastError().code(), client.lastError().message());
              }
            }
          } else {
            printError(client.lastError().code(), client.lastError().message());
          }
        } else {
          Serial.println("Outside of feeding time window. RFID scan ignored.");
        }
      }
    } else {
      printError(client.lastError().code(), client.lastError().message());
    }
  }
}

void checkFoodLevelSet(){
  if (dispenseStarted) {
    if (millis() - dispenseStartTime >= 15000) {
      setFoodLevel();
      dispenseStarted = false;
    }
  }
}

void setDailyGrams(int value) {
  uuid.generate();
  String feedTimeId = uuid.toCharArray();

  time_t epochTime = ntp.getEpochTime();
  struct tm *ptm = gmtime ((time_t *)&epochTime);

  char formattedDateTime[20];
  sprintf(formattedDateTime, "%02d/%02d/%04d - %02d:%02d", 
          ptm->tm_mon + 1,
          ptm->tm_mday,
          ptm->tm_year + 1900,
          ptm->tm_hour,
          ptm->tm_min);
  
  String currentDate = String(formattedDateTime);

  String newGramsData = "{\"date\": \"" + currentDate + "\", " +
                        "\"feedTimeId\": \"" + feedTimeId + "\", " +
                        "\"value\": " + String(value) + "}";

  String newGramsPath = DAILY_GRAMS_PATH + feedTimeId;
  Database.set<object_t>(client, newGramsPath, object_t(newGramsData));
}

void loop() {
  checkManualRFIDDispenseCommand();
  checkDispenseCommand();
  checkFeedingSchedule();
  checkRFIDPresent();
  triggerAutoJingle();
  checkFoodLevelSet();
}
