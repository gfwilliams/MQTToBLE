#include <WiFi.h>
#include <PubSubClient.h>
#include <BLEDevice.h>
// No docs? Use https://github.com/nkolban/esp32-snippets/tree/master/cpp_utils as source

const char* ssid     = "TALKTALK94E2FD";
const char* password = "GDKPTT7D";
const char* mqtt_server = "frank"; // mqtt server to connect to
const char* bridge_name = "esp32"; // the name of this bridge device (use mac?)

static BLEUUID service_uuid_nodata("ac910000-43be-801f-3ffc-65d26351c312");
static BLEUUID service_uuid_hasdata("ac910001-43be-801f-3ffc-65d26351c312");
static BLEUUID service_uuid("ac910001-43be-801f-3ffc-65d26351c312");
static BLEUUID characteristic_tx("ac910002-43be-801f-3ffc-65d26351c312");
static BLEUUID characteristic_rx("ac910003-43be-801f-3ffc-65d26351c312");


WiFiClient wifi;
PubSubClient mqtt(wifi);
BLEScan* pBLEScan;

BLERemoteCharacteristic* bleRXCharacteristic;
BLERemoteCharacteristic* bleTXCharacteristic;

long lastMsg = 0;
char msg[50];
int value = 0;


void bleScanStart() {
  pBLEScan->start(5, bleScanComplete, false);
}

void bleScanComplete(BLEScanResults scanResults) {
  Serial.println("Scan complete - restarting");
  // restart scan
  bleScanStart();  
}



static void bleNotifyCallback(
    BLERemoteCharacteristic* pBLERemoteCharacteristic,
    uint8_t* pData,
    size_t length,
    bool isNotify) {
  Serial.print("Notify callback for characteristic ");
  Serial.print(pBLERemoteCharacteristic->getUUID().toString().c_str());
  Serial.print(" of data length ");
  Serial.println(length);
  Serial.print("data: ");
  Serial.println((char*)pData);
}

class BLEClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    Serial.println("onConnect");
  }
  void onDisconnect(BLEClient* pclient) {
    Serial.println("onDisconnect");
  }
};

bool bleConnect(BLEAddress address) {
  Serial.print("Forming a connection to ");
  Serial.println(address.toString().c_str());
  
  BLEClient* pClient = BLEDevice::createClient();
  Serial.println(" - Created client");

  //pClient->setClientCallbacks(new BLEClientCallback());
  //Serial.println("Callbacks set");

  // Connect to the remove BLE Server.
  Serial.println("Connecting...");
  if (!pClient->connect(address)) { // if you pass BLEAdvertisedDevice instead of address, it will be recognized type of peer device address (public or private)
    Serial.println("Connection failed.");
    return false;
  }
  Serial.println(" - Connected to server");

  // Obtain a reference to the service we are after in the remote BLE server.
  BLERemoteService* pRemoteService = pClient->getService(service_uuid);
  if (pRemoteService == nullptr) {
    Serial.print("Failed to find our service UUID: ");
    Serial.println(service_uuid.toString().c_str());
    pClient->disconnect();
    return false;
  }
  Serial.println(" - Found our service");

  // Obtain a reference to the characteristic in the service of the remote BLE server.
  bleTXCharacteristic = pRemoteService->getCharacteristic(characteristic_tx);
  if (bleTXCharacteristic == nullptr) {
    Serial.print("Failed to find our characteristic UUID: ");
    Serial.println(characteristic_tx.toString().c_str());
    pClient->disconnect();
    return false;
  }
  Serial.println(" - Found our tx characteristic");

  // Obtain a reference to the characteristic in the service of the remote BLE server.
  bleRXCharacteristic = pRemoteService->getCharacteristic(characteristic_rx);
  if (bleRXCharacteristic == nullptr) {
    Serial.print("Failed to find our characteristic UUID: ");
    Serial.println(characteristic_rx.toString().c_str());
    pClient->disconnect();
    return false;
  }
  Serial.println(" - Found our rx characteristic");  

  if(bleRXCharacteristic->canNotify())
    bleRXCharacteristic->registerForNotify(bleNotifyCallback);

  bleTXCharacteristic->writeValue("", 0);
  // we're connected
  return true;
}


class AdvertiseCallbacks: public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) {
    if (!dev.haveServiceUUID()) return;
    Serial.println(dev.toString().c_str());
    
    if (dev.isAdvertisingService(service_uuid_nodata) ||
        dev.isAdvertisingService(service_uuid_hasdata)) {
      bool hasData = dev.isAdvertisingService(service_uuid_hasdata);

      String topic = String("MQTToBLE/") + bridge_name;+ "/advertise";
      String message = String("{\"addr\":\"")+dev.getAddress().toString().c_str()+
        "\",\"rssi\":"+dev.getRSSI()+
        ",\"dataReady\":"+(hasData?"true":"false")+"}";
      Serial.print("MQTT ");
      Serial.print(topic.c_str());
      Serial.print(" -> ");
      Serial.println(message.c_str());
      mqtt.publish(topic.c_str(), message.c_str());
      /*pBLEScan->stop();
      myDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
      doScan = true;*/
      pBLEScan->stop();
      bleConnect(dev.getAddress());
    }
  }
};


void setup()
{
  Serial.begin(115200);
  delay(10);

  Serial.println("Starting BLE...");
  BLEDevice::init("");

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
 
  // Connecting to a WiFi network  
 /* Serial.print("Connecting to WiFi ");
  Serial.println(ssid);
  
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
  
  mqtt.setServer(mqtt_server, 1883);
  mqtt.setCallback(callback);*/

 
}

void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();

  // Switch on the LED if an 1 was received as first character
  if ((char)payload[0] == '1') {

  }
}

void mqttReconnect() {
  // Loop until we're reconnected
  while (!mqtt.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Create a random mqtt ID
    String mqttId = "ESP8266mqtt-";
    mqttId += String(random(0xffff), HEX);
    // Attempt to connect
    if (mqtt.connect(mqttId.c_str())) {
      Serial.println("connected");
      // Once connected, publish an announcement...
      mqtt.publish("outTopic", "hello world");
      // ... and resubscribe
      mqtt.subscribe("inTopic");
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqtt.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

bool scanning = false;

void loop()
{
  /*if (!mqtt.connected()) {
    mqttReconnect();
  }
  mqtt.loop();*/
  if (!scanning) {
    scanning = true;
    //bleScanStart();
    BLEScanResults foundDevices = pBLEScan->start(5);
    for (int i=0;i<foundDevices.getCount();i++) {
      BLEAdvertisedDevice dev = foundDevices.getDevice(i);
      Serial.println(dev.toString().c_str());
      if (dev.haveServiceUUID()) {
        if (dev.isAdvertisingService(service_uuid_nodata) ||
            dev.isAdvertisingService(service_uuid_hasdata)) {
          bool hasData = dev.isAdvertisingService(service_uuid_hasdata);
      
          String topic = String("MQTToBLE/") + bridge_name;+ "/advertise";
          String message = String("{\"addr\":\"")+dev.getAddress().toString().c_str()+
            "\",\"rssi\":"+dev.getRSSI()+
            ",\"dataReady\":"+(hasData?"true":"false")+"}";
          Serial.print("Found device ");
          Serial.print(topic.c_str());
          Serial.print(" -> ");
          Serial.println(message.c_str());
          bleConnect(dev.getAddress());
        }
      }
    }
  }
/*
  long now = millis();
  if (now - lastMsg > 2000) {
    lastMsg = now;
    ++value;
    snprintf (msg, 50, "hello world #%ld", value);
    Serial.print("Publish message: ");
    Serial.println(msg);
    mqtt.publish("outTopic", msg);  
  }*/
}
