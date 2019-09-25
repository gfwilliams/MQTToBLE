/*
 * Install latest https://github.com/espressif/arduino-esp32 
 * Board: ESP32 Dev module
 * Set partition scheme - No OTA (2MB APP, 2MB SPIFFS)
 * 
 * https://github.com/knolleary/pubsubclient/releases > Arduino/libraries
 * 
 * Ctrl-Shift-I (Manage Libraries)
 * Install Arduino_JSON
 */

#include "const.h"
#include <WiFi.h>
#include <PubSubClient.h> // MQTT
#include <Arduino_JSON.h> // JSON - https://github.com/arduino-libraries/Arduino_JSON/blob/master/examples/JSONObject/JSONObject.ino
#include <BLEDevice.h>
// No docs? Use https://github.com/nkolban/esp32-snippets/tree/master/cpp_utils as source

const char* mqttTopicTx = "MQTToBLE/" BRIDGE_NAME "/tx";
const char* mqttTopicRx = "MQTToBLE/" BRIDGE_NAME "/rx";
const char* mqttTopicAdvertise = "MQTToBLE/" BRIDGE_NAME "/advertise";
const char* mqttTopicStatus = "MQTToBLE/" BRIDGE_NAME "/status";

static BLEUUID service_uuid_nodata("ac910000-43be-801f-3ffc-65d26351c312");
static BLEUUID service_uuid_hasdata("ac910001-43be-801f-3ffc-65d26351c312");
static BLEUUID service_uuid("ac910001-43be-801f-3ffc-65d26351c312");
static BLEUUID characteristic_tx("ac910002-43be-801f-3ffc-65d26351c312");
static BLEUUID characteristic_rx("ac910003-43be-801f-3ffc-65d26351c312");

#define WIFI

WiFiClient wifi;
PubSubClient mqtt(wifi);
BLEScan* pBLEScan;



long startTime;
long lastMsg = 0;

bool scanning = false;

typedef enum {
  BCS_DISCONNECTED,
  BCS_CONNECTING,
  BCS_CONNECTED
} BLEConnectionState;

BLEAddress bleConnectedDevice = BLEAddress("00:00:00:00:00:00");
BLEConnectionState bleConnectionState = BCS_DISCONNECTED;
BLEClient* bleClient;
BLERemoteCharacteristic* bleRXCharacteristic;
BLERemoteCharacteristic* bleTXCharacteristic;
uint8_t bleRXData[20];
int bleRXDataLength;
bool bleRXDataReceived = false;
long bleConnectionTimer; ///< reset last time there was activity

static void my_gap_event_handler(esp_gap_ble_cb_event_t  event, esp_ble_gap_cb_param_t* param) {
  ESP_LOGW(LOG_TAG, "custom gap event handler, event: %d", (uint8_t)event);
}

static void my_gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t* param) {
  ESP_LOGW(LOG_TAG, "custom gattc event handler, event: %d", (uint8_t)event);
}

void mqttStatus(const String &type, const String &message) {
  JSONVar json;
  json["type"] = type.c_str();
  json["data"] = message.c_str();
  String jsonString = JSON.stringify(json);
  
  Serial.print("MQTT> Status ");
  Serial.println(jsonString);
  mqtt.publish(mqttTopicStatus, jsonString.c_str());
}

void bleScanStart() {
  Serial.println("BLE> Restarting scan");
  pBLEScan->start(5, bleScanComplete, false);
}

void bleScanStop() {
  Serial.println("BLE> Stopping scan");
  pBLEScan->stop();
}

void bleScanComplete(BLEScanResults scanResults) {
  Serial.println("BLE> Scan complete");
  // restart scan
  bleScanStart();  
}


static void bleNotifyCallback(
    BLERemoteCharacteristic* pBLERemoteCharacteristic,
    uint8_t* pData,
    size_t length,
    bool isNotify) {

  // set global vars - we'll do this in our main loop
  bleConnectionTimer = millis();
  bleRXDataLength = length;
  if (bleRXDataLength > sizeof(bleRXData))
    bleRXDataLength = sizeof(bleRXData);
  memcpy(bleRXData, pData, length);
  bleRXDataReceived = true;  
}

class BLEClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    Serial.println("BLE> onConnect");
  }
  void onDisconnect(BLEClient* pclient) {
    Serial.println("BLE> onDisconnect");
    bleConnectionState = BCS_DISCONNECTED;
  }
};

void bleWrite(const uint8_t* data, size_t length) {
  bleConnectionTimer = millis();
  bleTXCharacteristic->writeValue(const_cast<uint8_t*>(data), length, false/*no response*/);
}

void bleDisconnect() {
  bleClient->disconnect();
  delete bleClient;
  bleClient = 0;
  bleConnectionState = BCS_DISCONNECTED;
  bleScanStart();
}

bool bleConnect(BLEAddress address) {
  if (bleConnectionState != BCS_DISCONNECTED) {
    Serial.println("BLE> ERROR - bleConnect when not disconnected");
    mqttStatus("error", "bleConnect when not disconnected");
    return false;
  }
  bleScanStop();
  bleConnectedDevice = address;
  bleConnectionTimer = millis();
  bleConnectionState = BCS_CONNECTING;
  Serial.print("BLE> Forming a connection to ");
  Serial.println(bleConnectedDevice.toString().c_str());
  
  bleClient = BLEDevice::createClient();
  Serial.println("BLE> Created client");

  bleClient->setClientCallbacks(new BLEClientCallback());
  Serial.println("BLE> Callbacks set");

  // Connect to the remove BLE Server.
  Serial.println("BLE> Connecting...");
  if (!bleClient->connect(bleConnectedDevice, BLE_ADDR_TYPE_RANDOM)) { // if you pass BLEAdvertisedDevice instead of address, it will be recognized type of peer device address (public or private)
    Serial.println("Connection failed.");
    mqttStatus("error", "Unable to connect");
    bleConnectionState = BCS_DISCONNECTED;
    return false;
  }
  Serial.println("BLE> Connected to server");

  // Obtain a reference to the service we are after in the remote BLE server.
  BLERemoteService* pRemoteService = bleClient->getService(service_uuid);
  if (pRemoteService == nullptr) {
    Serial.print("Failed to find our service UUID: ");
    Serial.println(service_uuid.toString().c_str());
    mqttStatus("error", "No MQTT service on BLE device");
    bleDisconnect();
    return false;
  }
  Serial.println("BLE> Found our service");

  // Obtain a reference to the characteristic in the service of the remote BLE server.
  bleTXCharacteristic = pRemoteService->getCharacteristic(characteristic_tx);
  if (bleTXCharacteristic == nullptr) {
    Serial.print("BLE> Failed to find our characteristic UUID: ");
    Serial.println(characteristic_tx.toString().c_str());
    mqttStatus("error", "No MQTT TX characteristic on BLE device");
    bleDisconnect();
    return false;
  }
  Serial.println("BLE> Found our tx characteristic");

  // Obtain a reference to the characteristic in the service of the remote BLE server.
  bleRXCharacteristic = pRemoteService->getCharacteristic(characteristic_rx);
  if (bleRXCharacteristic == nullptr) {
    Serial.print("BLE> Failed to find our characteristic UUID: ");
    Serial.println(characteristic_rx.toString().c_str());
    mqttStatus("error", "No MQTT RX characteristic on BLE device");
    bleDisconnect();
    return false;
  }
  Serial.println("BLE> Found our rx characteristic");  

  if(bleRXCharacteristic->canNotify()) {
    Serial.println("BLE> Registering for notify");  
    bleRXCharacteristic->registerForNotify(bleNotifyCallback);
  }

  // we're connected
  bleConnectionState = BCS_CONNECTED;  
  bleConnectionTimer = millis();

  //Serial.println("BLE> Write empty string to kick off transmit");  
  //bleWrite("",0);

  return true;
}




class AdvertiseCallbacks: public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) {
    if (!dev.haveServiceUUID()) return;
    //Serial.println(dev.toString().c_str());
    
    if (dev.isAdvertisingService(service_uuid_nodata) ||
        dev.isAdvertisingService(service_uuid_hasdata)) {
      bool hasData = dev.isAdvertisingService(service_uuid_hasdata);

      String message = String("{\"addr\":\"")+dev.getAddress().toString().c_str()+
        "\",\"rssi\":"+dev.getRSSI()+
        ",\"dataReady\":"+(hasData?"true":"false")+"}";
      Serial.print("MQTT> ");
      Serial.print(mqttTopicAdvertise);
      Serial.print(" -> ");
      Serial.println(message.c_str());
      mqtt.publish(mqttTopicAdvertise, message.c_str());
    }
  }
};


void setup()
{
  Serial.begin(115200);
  delay(10);

  Serial.println("Starting BLE...");
  BLEDevice::init("");

  BLEDevice::setCustomGapHandler(my_gap_event_handler);
  BLEDevice::setCustomGattcHandler(my_gattc_event_handler);

  // Retrieve a Scanner and set the callback we want to use to be informed when we
  // have detected a new device.  Specify that we want active scanning and start the
  // scan to run for 5 seconds.
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new AdvertiseCallbacks());
  // scan for pretty much all the time
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99); // scan for 
  pBLEScan->setActiveScan(true);
  //pBLEScan->setActiveScan(false); // don't bother with scan response

#ifdef WIFI   
  // Connecting to a WiFi network  
  Serial.print("WiFi> Connecting to WiFi ");
  Serial.println(ssid);
  
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("");
  Serial.println("WiFi> connected");
  Serial.print("WiFi> IP address: ");
  Serial.println(WiFi.localIP());
  
  mqtt.setServer(mqtt_server, 1883);
  mqtt.setCallback(mqttGotMessage);
#endif  

  bleScanStart();

  startTime = millis();
}

void mqttGotTxMessage(byte* payload, unsigned int length) {
  Serial.println("MQTT> TX message");
  char json[256];
  if (length>=sizeof(json)) length=sizeof(json)-1;
  memcpy(json, payload, length);
  json[length] = 0;
  
  JSONVar msgRoot = JSON.parse(json);

  // JSON.typeof(jsonVar) can be used to get the type of the var
  if (JSON.typeof(msgRoot) == "undefined") {
    Serial.println("MQTT> Parsing TX message failed!");
    return;
  }

  if (!msgRoot.hasOwnProperty("addr") ||
      !msgRoot.hasOwnProperty("data")) {
    Serial.println("MQTT> Malformed TX message!");
    return;
  }

  BLEAddress deviceAddress = BLEAddress((const char*)msgRoot["addr"]);
  JSONVar jsonData = msgRoot["data"];
  uint8_t data[20];
  size_t dataLen = 0;  
  if (JSON.typeof(jsonData) == "array") {
    dataLen = (size_t)(int)jsonData.length();
    if (dataLen > sizeof(data)) {
      mqttStatus("error", "MQTT TX data too long");
      dataLen = sizeof(data);
    }
    for (size_t i=0;i<dataLen;i++)
      data[i] = (uint8_t)(int)jsonData[i];
  } else {
    Serial.println("str");
    const char *s = (const char*)jsonData;
    dataLen = strlen(s);
    if (dataLen > sizeof(data)) {
      mqttStatus("error", "MQTT TX data too long");
      dataLen = sizeof(data);
    }
    for (size_t i=0;i<dataLen;i++)
      data[i] = (uint8_t)s[i];
  }

  Serial.print("MQTT> TX data ");
  Serial.print(dataLen);
  Serial.println(" bytes");
  
  if (bleConnectionState == BCS_CONNECTED) {
    if (!deviceAddress.equals(bleConnectedDevice))
      mqttStatus("error", "No MQTT TX characteristic on BLE device");
    else
      bleWrite(data, dataLen);
  } else {  
    if (bleConnect(deviceAddress))
      bleWrite(data, dataLen);
  }
  BLEAddress addr = BLEAddress(std::string((const char*)msgRoot["addr"]));
}

void mqttGotMessage(char* topic, byte* payload, unsigned int length) {
  Serial.print("MQTT> Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();

  if (strcmp(topic, mqttTopicTx)==0) {
    mqttGotTxMessage(payload, length);
  }
}

void mqttReconnect() {
  // Loop until we're reconnected
  while (!mqtt.connected()) {
    Serial.println("MQTT> Attempting MQTT connection...");
    // Attempt to connect
    if (mqtt.connect(bridge_name)) {
      Serial.println("MQTT> Connected");
      // Once connected, publish an announcement...
      mqttStatus("mqtt", "connected");
      // ... and resubscribe
      mqtt.subscribe(mqttTopicTx);
    } else {
      Serial.print("MQTT> failed, rc=");
      Serial.print(mqtt.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

void loop() {
  long now = millis();

#ifdef WIFI  
  if (!mqtt.connected()) {
    mqttReconnect();
  }
  mqtt.loop();
#endif  

  if (bleConnectionState == BCS_CONNECTED) {
    if (bleRXDataReceived) {
      bleRXDataReceived = false;
      /*JSONVar json;
      json["addr"] = bleConnectedDevice.toString().c_str();
      JSONVar jsonData;
      for (size_t i=0;i<bleRXDataLength;i++)
        jsonData[i] = bleRXData[i];
      json["data"] = jsonData;
      String jsonString = JSON.stringify(json);*/

      String jsonString = String("{\"addr\":\"")+bleConnectedDevice.toString().c_str()+
        "\",\"data\":[";
      for (size_t i=0;i<bleRXDataLength;i++) {
        if (i) jsonString += ",";
        jsonString += (int)bleRXData[i];
      }
      jsonString += "]}";
      
      Serial.print("MQTT> RX ");
      Serial.println(jsonString);
      mqtt.publish(mqttTopicRx, jsonString.c_str()); // broken somehow. yay!
    }
    
    if ((bleConnectionTimer + BLE_CONNECTION_TIMEOUT) < now) {
      Serial.println("BLE> disconnecting due to inactivity");
      bleDisconnect();
    }
  }

#ifdef WIFI
  if (now - lastMsg > 10000) {
    lastMsg = now;
    Serial.println("MQTT> ping");
    mqttStatus("mqtt", "ping");
  }
#endif  
}
