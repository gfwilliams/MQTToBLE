var SERVER = "mqtt://localhost";
var BRIDGENAME = "esp32";
var DISCONNECT_TIMEOUT = 2000; // 2 seconds of inactivity
var MQTToBLE_UUID_NODATA = "ac91000043be801f3ffc65d26351c312";
var MQTToBLE_UUID_DATA = "ac91000143be801f3ffc65d26351c312";
var MQTToBLE_SERVICE = "ac91000143be801f3ffc65d26351c312";
var MQTToBLE_CHAR_TX = "ac91000243be801f3ffc65d26351c312";
var MQTToBLE_CHAR_RX = "ac91000343be801f3ffc65d26351c312";

var mqtt = require('mqtt');
var mqttClient  = mqtt.connect(SERVER);
var mqttConnected = false;

var noble = require('noble');
var btDevices = {};
var btConnectedDevice = undefined;
var btWriteFn = undefined;
var btWriteQueue = {}; // per-device queue of stuff to write (map addr -> array of strings)

function strToArray(str) {
  var b = new Array[str.length];
  for (var i=0;i<str.length;i++)
    b[i]=str.charCodeAt(i);
  return b;
}
function arrayToString(array) {
  return String.fromCharCode.apply(null,array);
}

function onDiscovery(peripheral) {
  // peripheral.rssi                             - signal strength
  // peripheral.address                          - MAC address
  // peripheral.advertisement.localName          - device's name
  // peripheral.advertisement.manufacturerData   - manufacturer-specific data
  // peripheral.advertisement.serviceData        - normal advertisement service data
  // output what we have
  btDevices[peripheral.address] = peripheral;
  var serviceUuids = peripheral.advertisement.serviceUuids;
  if (serviceUuids.indexOf(MQTToBLE_UUID_DATA)>=0 ||
      serviceUuids.indexOf(MQTToBLE_UUID_NODATA)>=0) {
    var hasData = serviceUuids.indexOf(MQTToBLE_UUID_DATA)>=0;
    if (mqttConnected)
      mqttClient.publish('MQTToBLE/'+BRIDGENAME+'/advertise',JSON.stringify({
        addr : peripheral.address,
        rssi : peripheral.rssi,
        dataReady : hasData
      }));
    console.log(
      peripheral.address, hasData?"Data ready":"no data"
    );
  }
}

noble.on('stateChange',  function(state) {
  if (state!="poweredOn") return;
  console.log("Starting scan...");
  noble.startScanning([], true);
});
noble.on('discover', onDiscovery);
noble.on('scanStart', function() { console.log("Scanning started."); });
noble.on('scanStop', function() { console.log("Scanning stopped.");});


mqttClient.on('connect', function () {
  console.log("MQTT> Connected");
  mqttConnected = true;
  mqttClient.subscribe('MQTToBLE/'+BRIDGENAME+'/tx');
})

mqttClient.on('message', function (topic, message) {
  message = message.toString(); // message is a buffer
  console.log("MQTT> "+topic, message)
  if (topic == 'MQTToBLE/'+BRIDGENAME+'/tx') {
    var args = JSON.parse(message);
    bleWrite(args.addr, arrayToString(args.data));
  }
})

function bleWrite(addr, data) {
  if (!addr || data===undefined) throw new Error("Invalid write command");
  if (btWriteQueue[addr]===undefined) btWriteQueue[addr] = [];
  btWriteQueue[addr].push(data);
  console.log("Queued write "+addr+" <- "+JSON.stringify(data));
  serviceBtWriteQueue(addr);
}

/// do the next thing in the queue, return false is nothing
function serviceBtWriteQueue(addr) {
  if (btConnectedDevice!==undefined) {
    if (btConnectedDevice==addr) btWriteFn();
    return false; // already busy
  }
  if (addr===undefined) addr = Object.keys(btWriteQueue)[0];
  if (addr===undefined) return false; // no data
  if (!btWriteQueue[addr].length) {
    delete btWriteQueue[addr];
    return false; // no data
  }
  bleConnect(addr);
  return true;
}

function bleConnect(addr) {
  if (btConnectedDevice) throw new Error("Connection in progress")
  btDevice = btDevices[addr];
  if (btDevice === undefined) throw new Error("Device not found")
  btConnectedDevice = "CONNECTING";
  var isSending = false;

  function disconnected() {
    console.log("BT> Disconnected");
    btConnectedDevice = undefined;
    btWriteFn = undefined;
    // look for more stuff
    if (!serviceBtWriteQueue()) {
      console.log("Noble: Starting scan");
      noble.startScanning([], true);
    }
  }
  function sendData(callback) {
    function sender(callback) {
      if (!btWriteQueue[addr]) return callback();
      if (!btWriteQueue[addr].length) {
        delete btWriteQueue[addr];
        return callback();
      }
      var data = btWriteQueue[addr].shift();
      console.log(`Sending ${JSON.stringify(data)} (${data.length} bytes)`);
      txCharacteristic.write(new Buffer(data), false, function() {
        console.log("Sendcb");
        sender(callback);
      });
    }
    isSending = true;
    console.log("Starting send");
    sender(function() {
      console.log("Sending complete");
      isSending = false;
      callback();
    });
  }

  function connected() {
    var disconnectTimeout;
    function startDisconnectTimeout() {
      if (disconnectTimeout) clearTimeout(disconnectTimeout);
      disconnectTimeout = setTimeout(function() {
        console.log("BT> Disconnecting");
        btDevice.disconnect();
      }, DISCONNECT_TIMEOUT);
    }

    btConnectedDevice = addr;
    btWriteFn = function() {
      if (isSending) return; // we'll pick it up off the queue anyway
      sendData(startDisconnectTimeout);
    };
    sendData(startDisconnectTimeout);
  }
  var txCharacteristic, rxCharacteristic, btDevice;

  console.log("Noble: Stopping scan");
  noble.stopScanning();
  console.log("BT> Connecting");
  btDevice.on('disconnect', function() {
    txCharacteristic = undefined;
    rxCharacteristic = undefined;
    btDevice = undefined;
    disconnected();
  });

  btDevice.connect(function (error) {
    if (error) {
      console.log("BT> ERROR Connecting");
      return disconnected();
    }
    console.log("BT> Connected");

    btDevice.discoverAllServicesAndCharacteristics(function(error, services, characteristics) {
      function findByUUID(list, uuid) {
         for (var i=0;i<list.length;i++)
           if (list[i].uuid==uuid) return list[i];
         return undefined;
       }

      txCharacteristic = findByUUID(characteristics, MQTToBLE_CHAR_TX);
      rxCharacteristic = findByUUID(characteristics, MQTToBLE_CHAR_RX);
      if (error || !txCharacteristic || !rxCharacteristic) {
        console.log("BT> ERROR getting services/characteristics");
        console.log("TX "+txCharacteristic);
        console.log("RX "+rxCharacteristic);
        btDevice.disconnect();
      }
      rxCharacteristic.on('data', function (data) {
        data = data.toString('binary'); // as string
        console.log("BT> rx "+JSON.stringify(data));
        mqttClient.publish('MQTToBLE/'+BRIDGENAME+'/rx',JSON.stringify({
          addr : addr,
          data : strToArray(data)
        }));
      });
      rxCharacteristic.subscribe(function() {
        console.log("BT> Characteristics set up");
        connected();
      });
    });
  });
};


// debugging
const repl = require('repl');
var context = repl.start('> ').context;
context.bleWrite = function(data) {
  bleWrite("fc:cc:b8:22:b0:42",data);
};
