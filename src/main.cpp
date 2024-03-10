/**
  Nilan Modbus firmware for D1 Mini (ESP8266) together with a TTL to RS485 Converter https://www.aliexpress.com/item/32836213346.html?spm=a2g0s.9042311.0.0.27424c4dqnr5i7

  Written by Dan Gunvald
    https://github.com/DanGunvald/NilanModbus

  Modified to use with Home Assistant by Anders Kvist, Jacob Scherrebeck and other great people :)
    https://github.com/anderskvist/Nilan_Homeassistant
    https://github.com/jascdk/Nilan_Homeassistant

  Read from a Nilan Air Vent System (Danish Brand) using a Wemos D1
  Mini (or other ESP8266-based board) and report the values to an MQTT
  broker. Then use it for your home-automation system like Home Assistant.

  External dependencies. Install using the Arduino library manager:

     "Arduino JSON V6 by Benoît Blanchon https://github.com/bblanchon/ArduinoJson - IMPORTANT - Use latest V.6 !!! This code won´t compile with V.5
     "ModbusMaster by Doc Walker https://github.com/4-20ma/ModbusMaster
     "PubSubClient" by Nick O'Leary https://github.com/knolleary/pubsubclient

  Project inspired by https://github.com/DanGunvald/NilanModbus
*/

#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>
#include <ModbusMaster.h>
#include <PubSubClient.h>
#include "configuration.h"
#define SERIAL_SOFTWARE 1
#define SERIAL_HARDWARE 2
#if SERIAL_CHOICE == SERIAL_SOFTWARE
// for some reason this library keeps beeing included when building
// even if SERIAL_CHOICE != SERIAL_SOFTWARE
#include <SoftwareSerial.h>
#endif
#define HOST "NilanGW-%s" // Change this to whatever you like.
#define MAX_REG_SIZE 26
#define VENTSET 1003
#define RUNSET 1001
#define MODESET 1002
#define TEMPSET 1004
#define PROGRAMSET 500
#define COMPILED __DATE__ " " __TIME__

#if SERIAL_CHOICE == SERIAL_SOFTWARE
#define DEBUG_SERIAL // Serial debug can only be used if serial port is not used by modbus
#endif

#define DEBUG_SCAN_TIME // Turn on/off debugging of scan times
#ifdef DEBUG_SCAN_TIME
// Scan time variables
#define SCAN_COUNT_MAX 100000
int scanTime = -1; // Used to measure scan times of program
int scanLast = -1;
int scanMax = -1;
int scanMin = 5000; // Set to a fake high number
double scanMovingAvr = 20;
int scanCount = 0;
#endif
#define OUT_TOPIC_VENT "ventilation"

#if SERIAL_CHOICE == SERIAL_SOFTWARE
SoftwareSerial SSerial(SERIAL_SOFTWARE_RX, SERIAL_SOFTWARE_TX); // RX, TX
#endif

// Water meter variables
int fakeIr = 0; // Used while debugging
#define OUT_TOPIC_WATER "water"
#define WATER_IR_PIN A0
#define WATER_IR_LVL_HYSTERESIS 30
int waterIrLevel, waterIrLevelReal, waterLastLevel, waterIrDiff, waterIrMaxReal, waterIrMinReal = 1024;
int waterIrMin = 100;    // 75; // set realistic low  but about 20% higher then min. IR value found via debugging
int waterIrMiddle = 400; // Set to about the middle of the IR returned value. IR value found via debugging
int waterIrMax = 700;    // 800; // set realistic high but about 20% lower then max. IR value found via debugging
unsigned long waterEntryNext;
int waterState, waterStateLast;
int waterConsumptionCount;
bool waterWaitingForConsumptionCount = true;

// TODO delete next:
double movingAvrIrMin = 20;
double movingAvrIrMax = 20;
double movingAvrIrMid1 = 20;
double movingAvrIrMid2 = 20;
int movingAvrIrMinCount, movingAvrIrMaxCount, movingAvrIrMidCount1, movingAvrIrMidCount2;

const char *ssid = WIFI_SSID;
const char *password = WIFI_PASSWORD;
char chipID[12];
char host[64];
const char *mqttServer = MQTT_SERVER;
unsigned short mqttServerPort = MQTT_SERVER_PORT;
const char *mqttUsername = MQTT_USERNAME;
const char *mqttPassword = MQTT_PASSWORD;
const char *otaPassword = OTA_PASSWORD;

WiFiServer webServer(WEB_SERVER_PORT);
WiFiClient wifiClient;
String IPaddress;
PubSubClient mqttClient(wifiClient);
unsigned long ventEntryNext;
long modbusCooldown = 0;   // Used to limit modbus read/write operations
int modbusCooldownHit = 0; // Used to limit modbus read/write operations
int16_t rsBuffer[MAX_REG_SIZE];
ModbusMaster node;

int16_t ventAlarmListNumber[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 70, 71, 90, 91, 92};
String ventAlarmListText[] = {"NONE", "HARDWARE", "TIMEOUT", "FIRE", "PRESSURE", "DOOR", "DEFROST", "FROST", "FROST", "OVERTEMP", "OVERHEAT", "AIRFLOW", "THERMO", "BOILING", "SENSOR", "ROOM LOW", "SOFTWARE", "WATCHDOG", "CONFIG", "FILTER", "LEGIONEL", "POWER", "T AIR", "T WATER", "T HEAT", "MODEM", "INSTABUS", "T1SHORT", "T1OPEN", "T2SHORT", "T2OPEN", "T3SHORT", "T3OPEN", "T4SHORT", "T4OPEN", "T5SHORT", "T5OPEN", "T6SHORT", "T6OPEN", "T7SHORT", "T7OPEN", "T8SHORT", "T8OPEN", "T9SHORT", "T9OPEN", "T10SHORT", "T10OPEN", "T11SHORT", "T11OPEN", "T12SHORT", "T12OPEN", "T13SHORT", "T13OPEN", "T14SHORT", "T14OPEN", "T15SHORT", "T15OPEN", "T16SHORT", "T16OPEN", "ANODE", "EXCH INFO", "SLAVE IO", "OPT IO", "PRESET", "INSTABUS"};

String webRequestQueries[4]; // operation, group, address, value
enum ventRequestTypes
{
  reqtemp1 = 0, // regtemp1+2+3 is separated due to modbus breakdown when reading addresses from none present  "optional print board" expansion
  reqtemp2,
  reqtemp3,
  reqalarm,
  reqtime,
  reqcontrol,
  reqspeed,
  reqairtemp,
  reqairflow,
  reqairheat,
  reqprogram,
  requser,
  requser2,
  reqinfo,
  reqinputairtemp,
  reqapp,
  reqoutput,
  reqdisplay1,
  reqdisplay2,
  reqdisplay,
  reqmax // <-- Used to end for loops
};

String ventGroups[] = {"temp1", "temp2", "temp3", "alarm", "time", "control", "speed", "airtemp", "airflow", "airheat", "program", "user", "user2", "info", "inputairtemp", "app", "output", "display1", "display2", "display"};

// Start address to read from
int ventRegistrationAddresses[] = {203, 207, 221, 400, 300, 1000, 200, 1200, 1100, 0, 500, 600, 610, 100, 1200, 0, 100, 2002, 2007, 3000};

// How many values to read from based on start address
// byte ventRegistrationSizes[] = {23, 10, 6, 8, 2, 6, 2, 0, 1, 6, 6, 14, 7, 4, 26, 4, 4, 1};
byte ventRegistrationSizes[] = {2, 2, 1, 10, 6, 8, 2, 6, 2, 0, 1, 6, 6, 14, 1, 4, 26, 4, 4, 1};

// 0=raw, 1=x, 2 = return 2 characters ASCII,
// 4=xx, 8= return float dived by 1000,
byte ventRegistrationTypes[] = {8, 8, 8, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 2, 1, 4, 4, 8};

// Text translation of incoming data of the given address
char const *ventRegistrationNames[][MAX_REG_SIZE] = {
    // temp
    // {"T0_Controller", "T1_Intake", NULL, "T3_Exhaust", "T4_Outlet", NULL, NULL, "T7_Inlet", "T8_Outdoor", NULL, NULL, NULL, NULL, NULL, NULL, "T15_Room", NULL, NULL, NULL, NULL, NULL, "RH", NULL},
    {"T3_Exhaust", "T4_Outlet"},
    {"T7_Inlet", "T8_Outdoor", NULL, NULL, NULL, NULL, NULL, NULL, "T15_Room", NULL, NULL, NULL, NULL, NULL, "RH", NULL},
    {"RH"},
    // alarm
    {"Status", "List_1_ID", "List_1_Date", "List_1_Time", "List_2_ID", "List_2_Date", "List_2_Time", "List_3_ID", "List_3_Date", "List_3_Time"},
    // time
    {"Second", "Minute", "Hour", "Day", "Month", "Year"},
    // control
    {"Type", "RunSet", "ModeSet", "VentSet", "TempSet", "ServiceMode", "ServicePct", "Preset"},
    // speed
    {"ExhaustSpeed", "InletSpeed"},
    // airtemp
    {"CoolSet", "TempMinSum", "TempMinWin", "TempMaxSum", "TempMaxWin", "TempSummer"},
    // airflow
    {"AirExchMode", "CoolVent"},
    // airheat
    {},
    // program
    {"Program"},
    // program.user
    {"UserFuncAct", "UserFuncSet", "UserTimeSet", "UserVentSet", "UserTempSet", "UserOffsSet"},
    // program.user2 requires the optional print board
    {"User2FuncAct", "User2FuncSet", "User2TimeSet", "User2VentSet", "User2TempSet", "User2OffsSet"},
    // info
    {"UserFunc", "AirFilter", "DoorOpen", "Smoke", "MotorThermo", "Frost_overht", "AirFlow", "P_Hi", "P_Lo", "Boil", "3WayPos", "DefrostHG", "Defrost", "UserFunc_2"},
    // inputairtemp
    {"IsSummer", "TempInletSet", "TempControl", "TempRoom", "EffPct", "CapSet", "CapAct"},
    // app
    {"Bus.Version", "VersionMajor", "VersionMinor", "VersionRelease"},
    // output
    {"AirFlap", "SmokeFlap", "BypassOpen", "BypassClose", "AirCircPump", "AirHeatAllow", "AirHeat_1", "AirHeat_2", "AirHeat_3", "Compressor", "Compressor_2", "4WayCool", "HotGasHeat", "HotGasCool", "CondOpen", "CondClose", "WaterHeat", "3WayValve", "CenCircPump", "CenHeat_1", "CenHeat_2", "CenHeat_3", "CenHeatExt", "UserFunc", "UserFunc_2", "Defrosting"},
    // display1
    {"Text_1_2", "Text_3_4", "Text_5_6", "Text_7_8"},
    // display2
    {"Text_9_10", "Text_11_12", "Text_13_14", "Text_15_16"},
    // air bypass
    {"AirBypass/IsOpen"}};

char const *getName(ventRequestTypes type, int address)
{
  if (address >= 0 && address <= ventRegistrationSizes[type])
  {
    return ventRegistrationNames[type][address];
  }
  return NULL;
}

void modbusCool(int coolDownTimeMS)
{
  // Fix for breaking out of modbus error loop
  if ((long)millis() < (long)modbusCooldown)
  {
    if (modbusCooldownHit > 50)
    {
#ifdef DEBUG_SERIAL
      Serial.println("modbusCool limit reached. Doing reboot now.");
#endif
      mqttClient.publish(OUT_TOPIC_VENT "/error/modbusCooldown", "1"); // error when connecting through modbus
      ESP.reset();
    }
    modbusCooldownHit++;
    while ((long)millis() < (long)modbusCooldown)
    {
      delay(20);
    }
  }
  else if (modbusCooldownHit > 0)
  {
    modbusCooldownHit = 0;
  }
  modbusCooldown = millis() + coolDownTimeMS;
}

char WriteModbus(uint16_t addr, int16_t val)
{
  modbusCool(200);
  node.setTransmitBuffer(0, val);
  char result = 0;
  result = node.writeMultipleRegisters(addr, 1);
  return result;
}

char ReadModbus(uint16_t addr, uint8_t sizer, int16_t *vals, int type)
{
  modbusCool(200);
  char result = 0;
  // Make sure type is either 0 or 1
  switch (type & 1)
  {
  case 0:
    result = node.readInputRegisters(addr, sizer);
    break;
  case 1:
    result = node.readHoldingRegisters(addr, sizer);
    break;
  }
  if (result == node.ku8MBSuccess)
  {
    for (int j = 0; j < sizer; j++)
    {
      vals[j] = node.getResponseBuffer(j);
    }
    return result;
  }
  return result;
}

String httpMethod;
String httpInputKey;

JsonObject HandleRequest(JsonDocument &doc)
{
  JsonObject root = doc.to<JsonObject>();
  ventRequestTypes r = reqmax;
  if (webRequestQueries[1] != "")
  {
    for (int i = 0; i < reqmax; i++)
    {
      if (ventGroups[i] == webRequestQueries[1])
      {
        r = (ventRequestTypes)i;
      }
    }
  }
  char type = ventRegistrationTypes[r];
  if (webRequestQueries[0] == "read")
  {
    int address = 0;
    int nums = 0;
    char result = -1;
    address = ventRegistrationAddresses[r];
    nums = ventRegistrationSizes[r];

    result = ReadModbus(address, nums, rsBuffer, type);
    if (result == 0)
    {
      root["status"] = "Modbus connection OK";
      for (int i = 0; i < nums; i++)
      {
        char const *name = getName(r, i);
        if (name != NULL && strlen(name) > 0)
        {
          if ((type == 2 && i > 0) || type == 4)
          {
            String str = "";
            str += (char)(rsBuffer[i] & 0x00ff);
            str = (char)(rsBuffer[i] >> 8) + str;
            // Remove leading space from one character string
            str.trim();
            root[name] = str;
          }
          else if (type == 8)
          {
            root[name] = rsBuffer[i] / 100.0;
          }
          else
          {
            root[name] = rsBuffer[i];
          }
        }
      }
    }
    else
    {
      root["status"] = "Modbus connection failed";
    }
    root["requestAddress"] = address;
    root["requestNumber"] = nums;
  }
  else if (webRequestQueries[0] == "set" && webRequestQueries[1] != "" && webRequestQueries[2] != "")
  {
    if (httpInputKey == WEB_SERVER_KEY)
    {
      int address = atoi(webRequestQueries[1].c_str());
      int value = atoi(webRequestQueries[2].c_str());
      char result = WriteModbus(address, value);
      root["result"] = result;
      root["address"] = address;
      root["value"] = value;
      if (result != 0)
      {
        root["status"] = "Modbus connection failed";
      }
    }
    else
    {
      root["status"] = "Unauthorized";
    }
  }
  else if (webRequestQueries[0] == "get" && webRequestQueries[1] >= "0" && webRequestQueries[2] > "0")
  {
    int address = atoi(webRequestQueries[1].c_str());
    int nums = atoi(webRequestQueries[2].c_str());
    int type = atoi(webRequestQueries[3].c_str());
    char result = ReadModbus(address, nums, rsBuffer, type);
    if (result == 0)
    // if (true)
    {
      root["status"] = "Modbus connection OK";
      for (int i = 0; i < nums; i++)
      {
        root[String("address" + String(address + i))] = rsBuffer[i];
      }
    }
    else
    {
      root["status"] = "Modbus connection failed";
    }
    root["result"] = result;
    root["requestAddress"] = address;
    root["requestNumber"] = nums;
    switch (type)
    {
    case 0:
      root["type"] = "Input register";
      break;
    case 1:
      root["type"] = "Holding register";
      break;
    default:
      root["type"] = "Should be 0 or 1 for input/holding register";
    }
  }
  else if (webRequestQueries[0] == "help" || webRequestQueries[0] == "")
  {
    for (int i = 0; i < reqmax; i++)
    {
      root[ventGroups[i]] = "http://../read/" + ventGroups[i];
    }
  }
  root["operation"] = webRequestQueries[0];
  root["group"] = webRequestQueries[1];
  return root;
}

bool mqttSendOnConnectWater = false;

void incrementTicks(int consumption = 1)
{
  waterConsumptionCount += consumption;
  if (mqttClient.connected() && WiFi.status() == WL_CONNECTED)
  {
    char intToChar[12];
    sprintf(intToChar, "%d", waterConsumptionCount);
    mqttClient.publish(OUT_TOPIC_WATER "/total", String(waterConsumptionCount).c_str(), true); // Retain value
  }
  else if (consumption > 0 && mqttSendOnConnectWater == false)
  {
    mqttSendOnConnectWater = true;
  }
}

#define WIFI_CONNECT_TIMEOUT 300000 // Maximum time to wait for WiFi connection in milliseconds
enum connectState
{
  CONNECT_START,
  CONNECT_WAIT,
  CONNECT_SUCCESS,
  CONNECT_FAILURE
};

void wifiHandle()
{
  static unsigned long wifiConnectFailTime = 0;
  static connectState wifiConnectState = CONNECT_START;
  // static int numberRetries = 0;
  switch (wifiConnectState)
  {
  case CONNECT_START:
    WiFi.begin(ssid, password);
#ifdef DEBUG_SERIAL
    Serial.print("Wifi trying to connect to: ");
    Serial.println(ssid);
#endif
    wifiConnectState = CONNECT_WAIT;
    wifiConnectFailTime = millis() + WIFI_CONNECT_TIMEOUT;
    // numberRetries = 0;
    break;

  case CONNECT_WAIT:
    if (WiFi.waitForConnectResult() == WL_CONNECTED)
    {
      wifiConnectState = CONNECT_SUCCESS;
#ifdef DEBUG_SERIAL
      Serial.print("Wifi connection up. On ip: ");
      Serial.println(WiFi.localIP());
#endif
      ArduinoOTA.setHostname(host);
      ArduinoOTA.setPassword(otaPassword);
      ArduinoOTA.begin(false); // Sec: Disable mDns if not used
      webServer.begin();
      wifiConnectState = CONNECT_SUCCESS;
      // wifiConnectState = CONNECT_START; // Reset state for future connections
      break;
    }
    else if (millis() > wifiConnectFailTime)
    {
      wifiConnectState = CONNECT_FAILURE;
    }
    // numberRetries++;
    // Serial.print("Wifi connect failed, tries: ");
    // Serial.println(numberRetries);
    break;
  case CONNECT_SUCCESS:
    if (WiFi.status() != WL_CONNECTED)
    {
#ifdef DEBUG_SERIAL
      Serial.println("WiFi connection lost. Reconnecting...");
#endif
      wifiConnectState = CONNECT_START;
    }
    break;
  case CONNECT_FAILURE:
#ifdef DEBUG_SERIAL
    Serial.println("Wifi connection not up within timeout. Doing reboot now.");
    Serial.print("RSSI: ");
    Serial.println(WiFi.RSSI());
#endif
    delay(1000);
    // Give up and do a reboot
    ESP.restart();

    break;
  }
}

void mqttReconnect()
{
  static unsigned long mqttConnectNext = 0;
  static connectState mqttConnectState = CONNECT_START;
  static int numberRetries = 0;
#define mqttRetryCount 50
  switch (mqttConnectState)
  {
  case CONNECT_SUCCESS:
    if (!mqttClient.connected())
    {
      mqttConnectState = CONNECT_START;
    }
  case CONNECT_WAIT:
    if (millis() > mqttConnectNext)
    {
      mqttConnectState = CONNECT_START;
    }
    else
    {
      break;
    }
    break;
  case CONNECT_START:
    if (mqttClient.connect(chipID, mqttUsername, mqttPassword, OUT_TOPIC_VENT "/alive", 1, true, "0"))
    {
#ifdef DEBUG_SERIAL
      Serial.println("MQTT connection up. Subscribing to topics now.");
#endif
      mqttClient.publish(OUT_TOPIC_VENT "/alive", "1", true);
      mqttClient.subscribe(OUT_TOPIC_VENT "/cmd/+");
      mqttClient.subscribe(OUT_TOPIC_WATER "/cmd/+");
      if (mqttSendOnConnectWater)
      {
        mqttSendOnConnectWater = false;
        incrementTicks(0);
      }
      mqttConnectState = CONNECT_SUCCESS;
      numberRetries = 0;
      return;
    }
    numberRetries++;
#ifdef DEBUG_SERIAL
    Serial.print("MQTT connect failed, rc=");
    Serial.print(mqttClient.state());
    Serial.print(" trying again in 5 seconds. Tries left ");
    Serial.print(numberRetries);
    Serial.print(" of ");
    Serial.println(mqttRetryCount);
#endif
    if (numberRetries > mqttRetryCount)
    {
      mqttConnectState = CONNECT_FAILURE;
    }
    else if (WiFi.status() != WL_CONNECTED)
    {
#ifdef DEBUG_SERIAL
      Serial.println("WiFi connection lost while MQTT trying to reconnect. Reconnecting to wifi...");
#endif
      numberRetries = 0;
      wifiHandle();
    }
    else
    {
      mqttConnectState = CONNECT_WAIT;
      mqttConnectNext = millis() + 5000;
      delay(5);
    }
    break;
  case CONNECT_FAILURE:
#ifdef DEBUG_SERIAL
    Serial.println("MQTT reconnect tried limit reached. Doing reboot now.");
#endif
    delay(1000);
    // Give up and do a reboot
    ESP.restart();
    break;
  }
}

void mqttHandle()
{
  if (!mqttClient.connected())
  {
    mqttReconnect();
  }
  else
  {
    mqttClient.loop();
  }
  // Error due to analog reading of A0 to close to mqtt connect may cause error ref. https://github.com/knolleary/pubsubclient/issues/604#issuecomment-605597161
  // Adding a delay seems to fix this
  delay(2);
}

void mqttCallback(char *topic, byte *payload, unsigned int length)
{
#ifdef DEBUG_SERIAL
  Serial.print("Incoming mqtt topic: ");
  Serial.print(topic);
  Serial.println(" value[0]: " + String(payload[0]));
#endif
  String inputString;
  bool triggeredVentilation = false;
  for (unsigned int i = 0; i < length; i++)
  {
    inputString += (char)payload[i];
  }
  // Check if topic is equal to string
  if (strcmp(topic, OUT_TOPIC_VENT "/cmd/ventset") == 0)
  {
    if (length == 1 && payload[0] >= '0' && payload[0] <= '4')
    {
      int16_t speed = payload[0] - '0';
      WriteModbus(VENTSET, speed);
      mqttClient.publish(OUT_TOPIC_VENT "/cmd/ventset", "", true);
      triggeredVentilation = true;
    }
  }
  else if (strcmp(topic, OUT_TOPIC_VENT "/cmd/modeset") == 0)
  {
    if (length == 1 && payload[0] >= '0' && payload[0] <= '4')
    {
      int16_t mode = payload[0] - '0';
      WriteModbus(MODESET, mode);
      mqttClient.publish(OUT_TOPIC_VENT "/cmd/modeset", "", true);
      triggeredVentilation = true;
    }
  }
  else if (strcmp(topic, OUT_TOPIC_VENT "/cmd/runset") == 0)
  {
    if (length == 1 && payload[0] >= '0' && payload[0] <= '1')
    {
      int16_t run = payload[0] - '0';
      WriteModbus(RUNSET, run);
      mqttClient.publish(OUT_TOPIC_VENT "/cmd/runset", "", true);
      triggeredVentilation = true;
    }
  }
  else if (strcmp(topic, OUT_TOPIC_VENT "/cmd/tempset") == 0)
  {
    if (length > 0 && inputString.toInt() >= 5 && inputString.toInt() <= 25)
    {
      WriteModbus(TEMPSET, inputString.toInt() * 100); // Expect temperature 23 as 2300
      mqttClient.publish(OUT_TOPIC_VENT "/cmd/tempset", "", true);
      triggeredVentilation = true;
    }
  }
  else if (strcmp(topic, OUT_TOPIC_VENT "/cmd/programset") == 0)
  {
    if (length == 1 && payload[0] >= '0' && payload[0] <= '4')
    {
      int16_t program = payload[0] - '0';
      WriteModbus(PROGRAMSET, program);
      mqttClient.publish(OUT_TOPIC_VENT "/cmd/programset", "", true);
      triggeredVentilation = true;
    }
  }
  else if (strcmp(topic, OUT_TOPIC_VENT "/cmd/update") == 0)
  {
    // Enter mode in 60 seconds to prioritize OTA
    if (payload[0] == '1')
    {
      mqttClient.publish(OUT_TOPIC_VENT "/cmd/update", "2");
      for (unsigned int i = 0; i < 300; i++)
      {
        ArduinoOTA.handle();
        delay(200);
      }
      mqttHandle();
    }
    if (length > 0 && payload[0] != '0')
    {
      mqttClient.publish(OUT_TOPIC_VENT "/cmd/update", "", true); // Clear any retained messages
      delay(5);
      mqttClient.publish(OUT_TOPIC_VENT "/cmd/update", "0");
    }
  }
  else if (strcmp(topic, OUT_TOPIC_VENT "/cmd/reboot") == 0 || strcmp(topic, OUT_TOPIC_WATER "/cmd/reboot") == 0)
  {
    if (length > 0)
    {
      mqttClient.publish(OUT_TOPIC_VENT "/cmd/reboot", "", true);  // Clear any retained messages
      mqttClient.publish(OUT_TOPIC_WATER "/cmd/reboot", "", true); // Clear any retained messages
      delay(150);                                                  // FIX: wait before reboot to make sure any retained commands are sent back to broker
      // mqttClient.publish(OUT_TOPIC_VENT "/cmd/reboot", "0");
      ESP.restart();
    }
  }
  else if (strcmp(topic, OUT_TOPIC_VENT "/cmd/version") == 0)
  {
    if (length > 0 && inputString != String(COMPILED))
    {
      mqttClient.publish(OUT_TOPIC_VENT "/cmd/version", "", true);
      delay(5);
      mqttClient.publish(OUT_TOPIC_VENT "/cmd/version", String(COMPILED).c_str());
    }
  }
  else if (strcmp(topic, OUT_TOPIC_VENT "/cmd/readout") == 0)
  {
    if (length > 0)
    {
      triggeredVentilation = true;
      mqttClient.publish(OUT_TOPIC_VENT "/cmd/readout", "", true);
    }
  }
  // Start of water topics
  else if (strcmp(topic, OUT_TOPIC_WATER "/cmd/total") == 0)
  {
    if (length > 0)
    {
      mqttClient.publish(OUT_TOPIC_WATER "/cmd/total", "", true);
      waterConsumptionCount = inputString.toInt();
      // incrementTicks by 0 to tell any listers the value has changed by 0
      incrementTicks(0);
    }
  }
  else if (strcmp(topic, OUT_TOPIC_WATER "/cmd/readout") == 0)
  {
    if (length > 0)
    {
      mqttClient.publish(OUT_TOPIC_WATER "/cmd/readout", "", true); // Clear any retained messages
      mqttClient.publish(OUT_TOPIC_WATER "/debug/irLevel", String(waterIrLevel).c_str());
      mqttClient.publish(OUT_TOPIC_WATER "/debug/irLevelReal", String(waterIrLevelReal).c_str());
      mqttClient.publish(OUT_TOPIC_WATER "/debug/irMin", String(waterIrMin).c_str());
      mqttClient.publish(OUT_TOPIC_WATER "/debug/irMax", String(waterIrMax).c_str());
      mqttClient.publish(OUT_TOPIC_WATER "/debug/IrMiddle", String(waterIrMiddle).c_str());
      mqttClient.publish(OUT_TOPIC_WATER "/debug/irMaxReal", String(waterIrMaxReal).c_str());
      mqttClient.publish(OUT_TOPIC_WATER "/debug/irMinReal", String(waterIrMinReal).c_str());
      mqttClient.publish(OUT_TOPIC_WATER "/debug/state", String(waterState).c_str());
      mqttClient.publish(OUT_TOPIC_WATER "/debug/movingAvrIrMin", String(floor(movingAvrIrMin * 100) / 100).c_str());
      mqttClient.publish(OUT_TOPIC_WATER "/debug/movingAvrIrMax", String(floor(movingAvrIrMax * 100) / 100).c_str());
      mqttClient.publish(OUT_TOPIC_WATER "/debug/movingAvrIrMid1", String(floor(movingAvrIrMid1 * 100) / 100).c_str());
      mqttClient.publish(OUT_TOPIC_WATER "/debug/movingAvrIrMid2", String(floor(movingAvrIrMid2 * 100) / 100).c_str());
      mqttClient.publish(OUT_TOPIC_WATER "/debug/movingAvrIrMinCount", String(movingAvrIrMinCount).c_str());
    }
  }
  else if (strcmp(topic, OUT_TOPIC_WATER "/cmd/irlvl") == 0)
  {
    if (length > 0 && inputString.toInt() >= 1 && inputString.toInt() <= 1024)
    {
      fakeIr = inputString.toInt();
      // WriteModbus(TEMPSET, inputString.toInt() * 100); // Expect temperature 23 as 2300
      // mqttClient.publish(OUT_TOPIC_VENT "/cmd/tempset", "", true);
      // triggeredVentilation = true;
    }
    else if (payload[0] == '0')
    {
      // Do nothing
      if (fakeIr > 0)
        fakeIr = 0;
    }
    else if (length > 0)
    {
      delay(5);
      mqttClient.publish(OUT_TOPIC_WATER "/cmd/irlvl", "0");
      if (fakeIr > 0)
        fakeIr = 0;
    }
    mqttClient.publish(OUT_TOPIC_WATER "/cmd/irlvl", "", true);
  }
  else if (waterWaitingForConsumptionCount && strcmp(topic, OUT_TOPIC_WATER "/total") == 0)
  {
    waterWaitingForConsumptionCount = false;
    mqttClient.unsubscribe(OUT_TOPIC_WATER "/total");
    waterConsumptionCount = inputString.toInt(); // Convert char to int. On error returns zero
    mqttClient.publish(OUT_TOPIC_WATER "/debug/totalRecovery", inputString.c_str());
  }
  else
  {
    String inputTopic = topic;
#ifdef DEBUG_SERIAL
    Serial.print("Unknown mqtt topic: ");
    Serial.print(inputTopic);
    Serial.println(" value: " + inputString);
#endif
    mqttClient.publish(OUT_TOPIC_VENT "/error/topic", inputTopic.c_str());
  }
  if (triggeredVentilation == true)
  {
    ventEntryNext = 0; //-MQTT_SEND_INTERVAL;
  }
}

bool readRequest(WiFiClient &client)
{
  int lineIndex = -1;
  httpMethod = "";
  httpInputKey = "";
  webRequestQueries[0] = "";
  webRequestQueries[1] = "";
  webRequestQueries[2] = "";
  webRequestQueries[3] = "";
#ifdef DEBUG_SERIAL
  Serial.print("Web request read started from ");
  Serial.print(client.remoteIP());
  Serial.println(". Content on next line:");
#endif
  while (client.connected())
  {
    lineIndex++;
    String line = client.readStringUntil('\n');
    if (line == "\n" || line == "" || !client.available())
    {
      if (lineIndex == 0)
      {
#ifdef DEBUG_SERIAL
        Serial.println("Web request read ended in failure. Wrong input.");
#endif
        return false;
      }
#ifdef DEBUG_SERIAL
      Serial.print(httpMethod);
      Serial.print(" path=[");
      for (int i = 0; i < 4; i++)
      {
        Serial.print(i);
        Serial.print(":");
        Serial.print(webRequestQueries[i]);
        if (i < 3)
          Serial.print(", ");
      }

      if (httpInputKey.length() > 0)
      {
        Serial.print("] Key: ");
        Serial.println(httpInputKey);
      }
      else
      {
        Serial.println("]");
      }
#endif
      return true;
    }

    if (lineIndex == 0)
    {
      // First line this contains something like: GET /api HTTP/1.1
      int firstSpace = line.indexOf(' ');
      int secondSpace = line.indexOf(' ', firstSpace + 1);
      httpMethod = line.substring(0, firstSpace); // Will be GET or POST or ...
      String path = line.substring(firstSpace + 1, secondSpace);
      // Find the positions of the slashes
      int slashPositions[3]; // Array to store the positions of slashes
      for (int i = 0; i < 3; i++)
      {
        slashPositions[i] = path.indexOf('/', (i == 0) ? 1 : slashPositions[i - 1] + 1);
        if (slashPositions[i] == -1)
        {
          slashPositions[i] = path.length();
        }
      }
      webRequestQueries[0] = path.substring(1, slashPositions[0]);
      webRequestQueries[1] = path.substring(slashPositions[0] + 1, slashPositions[1]);
      webRequestQueries[2] = path.substring(slashPositions[1] + 1, slashPositions[2]);
      webRequestQueries[3] = path.substring(slashPositions[2] + 1);
    }
    // Check for the custom header "Key"
    if (line.startsWith("Key:"))
    {
      httpInputKey = line.substring(4); // Assuming "Key: " is 5 characters long
      httpInputKey.trim();              // Remove leading and trailing whitespace
    }
  }
#ifdef DEBUG_SERIAL
  Serial.println("Web request read ended in failure. Client closed connection.");
#endif
  return false;
}

void writeResponse(WiFiClient &client, const JsonDocument &doc)
{
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/json");
  client.println("Connection: close");
  // Fix: To adhere to RFC2616 section 14.13. Calculate length of data to client
  String response = "";
  serializeJsonPretty(doc, response);
  client.print("Content-Length: ");
  client.println(response.length());
  client.println();
  client.print(response);
}

#ifdef DEBUG_SCAN_TIME
// Scan time is the time then looping part of a program runs in miliseconds.
// Rule of thumb is to allow max 20ms to rule the program as runnning "live" and not async
// Live running programs are relevant when expecting non buffered IO operations with the real world
void scanTimer()
{
  if (scanCount > SCAN_COUNT_MAX)
  {
    return;
  }
  if (scanLast == -1)
  {
    scanLast = millis();
    return;
  }
  scanTime = millis() - scanLast;
  if (scanTime > scanMax)
    scanMax = scanTime;
  if (scanTime < scanMin && scanTime > 0)
    scanMin = scanTime;
  scanCount++;
  scanMovingAvr = scanTime * (0.3 / (1 + scanCount)) + scanMovingAvr * (1 - (0.3 / (1 + scanCount)));
  if (scanCount > SCAN_COUNT_MAX)
  {
    mqttClient.publish(OUT_TOPIC_VENT "/debug/scanMin", String(scanMin).c_str());
    mqttClient.publish(OUT_TOPIC_VENT "/debug/scanMax", String(scanMax).c_str());
    mqttClient.publish(OUT_TOPIC_VENT "/debug/scanMovingAvr", String(floor(scanMovingAvr * 100) / 100).c_str());
  }
  scanLast = millis();
}
#endif

int readIR()
{
  int level = analogRead(WATER_IR_PIN);
  waterIrLevelReal = level;
  if (fakeIr > 0)
    level = fakeIr;
  // if (level < 50)
  //   level = 50;
  // if (level > 600)
  //   level = 600;
  if (level > waterIrMax)
  {
    waterIrMax = level; // maximum signal
  }
  else if (level < waterIrMin)
  {
    waterIrMin = level;                            // minimum signal
    waterIrMiddle = (waterIrMax + waterIrMin) / 2; // calculate middle of the signal
  }
  if (waterIrMiddle < 100)
  {
    waterIrMiddle = 200;
  }
  return level;
}

void setup()
{
  sprintf(chipID, "%08X", ESP.getChipId());
  sprintf(host, HOST, chipID);
#ifdef DEBUG_SERIAL
  Serial.begin(115200);
  Serial.print("Started ");
  Serial.println("chipID: " + String(host));
#endif
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW); // Reverse meaning. LOW=LED ON
  WiFi.mode(WIFI_STA);
  WiFi.hostname(host);
  while (WiFi.status() != WL_CONNECTED)
  {
    wifiHandle();
  }
#if SERIAL_CHOICE == SERIAL_SOFTWARE
#warning Compiling for software serial
  SSerial.begin(19200, SWSERIAL_8E1);
  node.begin(MODBUS_SLAVE_ADDRESS, SSerial);
#elif SERIAL_CHOICE == SERIAL_HARDWARE
#warning Compiling for hardware serial
  Serial.begin(19200, SERIAL_8E1);
  node.begin(MODBUS_SLAVE_ADDRESS, Serial);
#else
#error hardware or serial serial port?
#endif

  mqttClient.setServer(mqttServer, mqttServerPort);
  mqttClient.setCallback(mqttCallback);
  while (!mqttClient.connected())
  {
    mqttHandle();
    ArduinoOTA.handle();
  }
  mqttClient.publish(OUT_TOPIC_VENT "/debug/bootTime", String(millis()).c_str());
  IPaddress = WiFi.localIP().toString();
  mqttClient.publish(OUT_TOPIC_VENT "/debug/ip", IPaddress.c_str());
  mqttClient.publish(OUT_TOPIC_VENT "/debug/hostname", host);
  waterEntryNext = millis();
  if (waterWaitingForConsumptionCount)
  {
    mqttClient.subscribe(OUT_TOPIC_WATER "/total");
  }
  // Allows for 5 seconds to wait for mqtt data for retained total consumption count
  while (millis() < waterEntryNext + 5000 && waterWaitingForConsumptionCount)
  {
    mqttHandle();
    ArduinoOTA.handle();
  }
  if (waterWaitingForConsumptionCount)
  {
    mqttClient.unsubscribe(OUT_TOPIC_WATER "/total");
    waterWaitingForConsumptionCount = false;
  }
  waterIrLevel = readIR();
  if (waterIrLevel > waterIrMax)
  {
    waterState = 2; // Test to see if this reduce missing count on reboots
  }
  digitalWrite(LED_BUILTIN, HIGH);
}

bool modbusErrorActive = false;
bool mqttSendOnConnectVent = false;

void loop()
{
  // Check if WiFi is still connected
  if (WiFi.status() != WL_CONNECTED)
  {
    wifiHandle();
  }
  else
  {
    ArduinoOTA.handle();
    WiFiClient webClient = webServer.available();
    if (webClient)
    {
      bool success = readRequest(webClient);
      if (success)
      {
        StaticJsonDocument<1000> doc;
        HandleRequest(doc);
        writeResponse(webClient, doc);
      }
      webClient.stop();
    }
    mqttHandle();
  }

  unsigned long now = millis();
  // Only run ventilation read if wifi and mqtt running.
  // If data cannot be send anyway no reason to get it
  if (now > ventEntryNext && mqttClient.connected() && WiFi.status() == WL_CONNECTED)
  {
    ventEntryNext = now + MQTT_SEND_INTERVAL;
    //  ventRequestTypes rr[] = {reqtemp, reqcontrol, reqtime, reqoutput, reqspeed, reqalarm, reqinputairtemp, reqprogram, requser, reqdisplay, reqinfo}; // put another register in this line to subscribe
    ventRequestTypes rr[] = {reqtemp1, reqtemp2, reqtemp3, reqcontrol, reqalarm, reqinputairtemp, reqprogram, reqdisplay, requser}; // put another register in this line to subscribe
    for (unsigned int i = 0; i < (sizeof(rr) / sizeof(rr[0])); i++)
    {
      ventRequestTypes r = rr[i];
      char result = ReadModbus(ventRegistrationAddresses[r], ventRegistrationSizes[r], rsBuffer, ventRegistrationTypes[r]);
      if (result == 0)
      {
        if (modbusErrorActive)
        {
          mqttClient.publish(OUT_TOPIC_VENT "/error/modbus", "0"); // no error when connecting through modbus
          modbusErrorActive = false;
        }
        for (int i = 0; i < ventRegistrationSizes[r]; i++)
        {
          char const *name = getName(r, i);
          char numberString[10];
          if (name != NULL && strlen(name) > 0)
          {
            String mqttTopic;
            switch (r)
            {
            case reqcontrol:
              mqttTopic = OUT_TOPIC_VENT "/control/"; // Subscribe to the "control" register
              if (strncmp(name, "TempSet", 7) == 0)
              {
                // TempSet value = 2300 is converted to 23
                dtostrf((rsBuffer[i] / 100), 1, 0, numberString);
              }
              else
              {
                itoa((rsBuffer[i]), numberString, 10);
              }
              break;
            case reqtime:
              mqttTopic = OUT_TOPIC_VENT "/time/"; // Subscribe to the "output" register
              itoa((rsBuffer[i]), numberString, 10);
              break;
            case reqoutput:
              mqttTopic = OUT_TOPIC_VENT "/output/"; // Subscribe to the "output" register
              itoa((rsBuffer[i]), numberString, 10);
              break;
            case reqdisplay:
              mqttTopic = OUT_TOPIC_VENT "/display/"; // Subscribe to the "input display" register
              itoa((rsBuffer[i]), numberString, 10);
              break;
            case reqspeed:
              mqttTopic = OUT_TOPIC_VENT "/speed/"; // Subscribe to the "speed" register
              itoa((rsBuffer[i]), numberString, 10);
              break;
            case reqalarm:
              mqttTopic = OUT_TOPIC_VENT "/alarm/"; // Subscribe to the "alarm" register

              switch (i)
              {
              case 1: // Alarm.List_1_ID
              case 4: // Alarm.List_2_ID
              case 7: // Alarm.List_3_ID
                if (rsBuffer[i] > 0)
                {
                  sprintf(numberString, "UNKNOWN"); // Preallocate unknown if no match if found
                  for (unsigned int p = 0; p < (sizeof(ventAlarmListNumber)); p++)
                  {
                    if (ventAlarmListNumber[p] == rsBuffer[i])
                    {
                      sprintf(numberString, ventAlarmListText[p].c_str());
                      break;
                    }
                  }
                }
                else
                {
                  sprintf(numberString, "None"); // No alarm, output None
                }
                break;
              case 2: // Alarm.List_1_Date
              case 5: // Alarm.List_2_Date
              case 8: // Alarm.List_3_Date
                if (rsBuffer[i] > 0)
                {
                  sprintf(numberString, "%d", (rsBuffer[i] >> 9) + 1980);
                  sprintf(numberString + strlen(numberString), "-%02d", (rsBuffer[i] & 0x1E0) >> 5);
                  sprintf(numberString + strlen(numberString), "-%02d", (rsBuffer[i] & 0x1F));
                }
                else
                {
                  sprintf(numberString, "N/A"); // No alarm, output N/A
                }
                break;
              case 3: // Alarm.List_1_Time
              case 6: // Alarm.List_2_Time
              case 9: // Alarm.List_3_Time
                if (rsBuffer[i] > 0)
                {
                  sprintf(numberString, "%02d", rsBuffer[i] >> 11);
                  sprintf(numberString + strlen(numberString), ":%02d", (rsBuffer[i] & 0x7E0) >> 5);
                  sprintf(numberString + strlen(numberString), ":%02d", (rsBuffer[i] & 0x11F) * 2);
                }
                else
                {
                  sprintf(numberString, "N/A"); // No alarm, output N/A
                }

                break;
              default: // used for Status bit (case 0)
                itoa((rsBuffer[i]), numberString, 10);
              }
              break;
            case reqinputairtemp:
              mqttTopic = OUT_TOPIC_VENT "/inputairtemp/"; // Subscribe to the "inputairtemp" register
              itoa((rsBuffer[i]), numberString, 10);
              break;
            case reqprogram:
              mqttTopic = OUT_TOPIC_VENT "/weekprogram/"; // Subscribe to the "week program" register
              itoa((rsBuffer[i]), numberString, 10);
              break;
            case requser:
              mqttTopic = OUT_TOPIC_VENT "/user/"; // Subscribe to the "user" register
              itoa((rsBuffer[i]), numberString, 10);
              break;
            case requser2:
              mqttTopic = OUT_TOPIC_VENT "/user/"; // Subscribe to the "user2" register
              itoa((rsBuffer[i]), numberString, 10);
              break;
            case reqinfo:
              mqttTopic = OUT_TOPIC_VENT "/info/"; // Subscribe to the "info" register
              itoa((rsBuffer[i]), numberString, 10);
              break;
            case reqtemp1:
            case reqtemp2:
            case reqtemp3:
              if (strncmp("RH", name, 2) == 0)
              {
                mqttTopic = OUT_TOPIC_VENT "/moist/"; // Subscribe to moisture-level
              }
              else
              {
                mqttTopic = OUT_TOPIC_VENT "/temp/"; // Subscribe to "temp" register
              }
              dtostrf((rsBuffer[i] / 100.0), 5, 2, numberString);
              break;
            default:
              // If not all enumerations possibilities are handled then message are added to the unmapped topic
              mqttTopic = OUT_TOPIC_VENT "/unmapped/";
              break;
            }
            mqttTopic += (char *)name;
            mqttClient.publish(mqttTopic.c_str(), numberString);
          }
        }
      }
      else
      {
        if (modbusErrorActive == false)
        {
          mqttClient.publish(OUT_TOPIC_VENT "/error/modbus", "1"); // no error when connecting through modbus
          modbusErrorActive = true;
        }
      }
    }
  }
  if (now > waterEntryNext)
  {
    waterEntryNext = now + 100;
    // readIR also writes to global var waterIrLevelReal the real read value
    waterIrLevel = readIR();
    waterIrDiff = waterIrLevel - waterLastLevel; // first derivative
    // Hysteresis to remove small differences in IR level reads
    if (abs(waterIrDiff) < WATER_IR_LVL_HYSTERESIS)
    {
      waterIrLevel = waterLastLevel;
      waterIrDiff = 0;
    }
    else
    {
      waterLastLevel = waterIrLevel;
    }
    // Debugging:
    if (waterIrLevelReal > waterIrMaxReal)
    {
      waterIrMaxReal = waterIrLevelReal; // maximum signal
    }
    else if (waterIrLevelReal < waterIrMinReal)
    {
      waterIrMinReal = waterIrLevelReal;
    }
    switch (waterState)
    {
    case 0:
      if (waterIrDiff < 0 && (waterIrLevel > waterIrMiddle))
      {
        waterState = 1;
        if (movingAvrIrMaxCount < 2147483646)
        {
          movingAvrIrMax = waterIrMaxReal * (0.3 / (1 + movingAvrIrMaxCount)) + movingAvrIrMax * (1 - (0.3 / (1 + movingAvrIrMaxCount)));
          movingAvrIrMaxCount++;
        }
      }
      digitalWrite(LED_BUILTIN, HIGH);
      break;
    case 1:
      if (waterIrDiff < 0 && (waterIrLevel < waterIrMiddle))
      {
        waterIrMax = waterIrMax - waterIrMiddle * 0.1; // for long term adjustment
        waterState = 2;
        if (movingAvrIrMidCount1 < 2147483646)
        {
          movingAvrIrMid1 = waterIrLevelReal * (0.3 / (1 + movingAvrIrMidCount1)) + movingAvrIrMid1 * (1 - (0.3 / (1 + movingAvrIrMidCount1)));
          movingAvrIrMidCount1++;
        }
      }
      digitalWrite(LED_BUILTIN, LOW);
      break;
    case 2:
      if (waterIrDiff > 0 && (waterIrLevel < waterIrMiddle))
      {
        waterState = 3;
        if (movingAvrIrMinCount < 2147483646)
        {
          movingAvrIrMin = waterIrMinReal * (0.3 / (1 + movingAvrIrMinCount)) + movingAvrIrMin * (1 - (0.3 / (1 + movingAvrIrMinCount)));
          movingAvrIrMinCount++;
        }
      }
      digitalWrite(LED_BUILTIN, HIGH);
      break;
    case 3:
      if (waterIrDiff > 0 && (waterIrLevel > waterIrMiddle))
      {
        waterIrMin = waterIrMin + waterIrMiddle * 0.1; // long-term adjustment
        incrementTicks(1);
        waterState = 0;
        if (movingAvrIrMidCount2 < 2147483646)
        {
          movingAvrIrMid2 = waterIrLevelReal * (0.3 / (1 + movingAvrIrMidCount2)) + movingAvrIrMid2 * (1 - (0.3 / (1 + movingAvrIrMidCount2)));
          movingAvrIrMidCount2++;
        }
      }
      digitalWrite(LED_BUILTIN, LOW);
      break;
    default:
      break;
    }
    if (waterEntryNext > (unsigned long)4294900000) // 1288490187)
    {
      // Fix to make sure the command millis() dont overflow. This happens after 50 days and would mess up some logic above
      // Reboot if ESP has been running for approximately 49 days.
      ESP.restart();
    }
  }

#ifdef DEBUG_SCAN_TIME
  scanTimer();
#endif
}
