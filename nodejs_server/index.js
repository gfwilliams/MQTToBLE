var SERVER = "mqtt://localhost";

var bleDevices = {};
var mqtt = require('mqtt');
var mqttClient  = mqtt.connect(SERVER);
mqttClient.on('connect', function () {
  console.log("MQTT> Connected");
  mqttClient.subscribe('MQTToBLE/#');
});
mqttClient.on('message', function (topic, message) {
  var path = topic.split("/");
  if (path[0]!="MQTToBLE") return;
  message = message.toString(); // message is a buffer
  //console.log("MQTT> "+topic, message)
  var args = JSON.parse(message);
  if (path[2]=="advertise") bridgeAdvertise(path[1]/*bridgeName*/,args);
  if (path[2]=="rx") bridgeRx(path[1]/*bridgeName*/,args);
});

function bridgeAdvertise(bridgeName, args) {
  if (!args.addr || args.rssi===undefined)
    throw new Error("Invalid advertise message");
  if (!bleDevices[args.addr]) {
    console.log("New device added: ",args.addr);
    bleDevices[args.addr] = {
      bridges : {},
      dataReady : false,
      sendQueue : []
    };
  }
  var device = bleDevices[args.addr];
  device.dataReady = args.dataReady;
  device.bridges[bridgeName] = {
    rssi : args.rssi,
    lastSeen : Date.now()
  };
  if (device.dataReady || device.sendQueue)
    handleDeviceConnection();
}
function bridgeRx(bridgeName, args) {
}

function handleDeviceConnection() {
}

// debugging
const repl = require('repl');
var context = repl.start('> ').context;
context.dumpDevices = function() {
  console.log(JSON.stringify(bleDevices,null,2))
};
