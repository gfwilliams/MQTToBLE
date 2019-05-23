var SERVER = "mqtt://localhost";
var RECONNECT_TIMEOUT = 20*1000;

var bleDevices = {};
var net = require('net');
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
  console.log("MQTT> "+topic, message)
  var args = JSON.parse(message);
  if (path[2]=="advertise") bridgeAdvertise(path[1]/*bridgeName*/,args);
  if (path[2]=="rx") bridgeRx(path[1]/*bridgeName*/,args);
});
var MQTT_CMD = {
    CONNECT    : 1,
    CONNACK    : 2,
    PUBLISH    : 3,
    PUBACK     : 4,
    PUBREC     : 5,
    PUBREL     : 6,
    PUBCOMP    : 7,
    SUBSCRIBE  : 8,
    SUBACK     : 9,
    UNSUBSCRIBE: 10,
    UNSUBACK   : 11,
    PINGREQ    : 12,
    PINGRESP   : 13,
    DISCONNECT : 14
};

var sFCC = String.fromCharCode;
function mqStr(str) {
	return sFCC(str.length >> 8, str.length&255) + str;
}
function mqPkt(cmd, variable, payload) {
	return sFCC(cmd, variable.length + payload.length) + variable + payload;
}
function mqCon(id){
  // Authentication?
  var flags = 0;
  var payload = mqStr(id);
  return mqPkt(0b00010000,
    mqStr("MQTT")/*protocol name*/+
    sFCC(4/*protocol level*/,flags,255,255/*Keepalive*/),
    payload);
}
function strToBuf(str) {
  var b = Buffer.alloc(str.length);
  for (var i=0;i<str.length;i++)
    b[i]=str.charCodeAt(i);
  return b;
}

function bridgeAdvertise(bridgeName, args) {
  // args = {addr:str, rssi:int, dataReady:bool}
  if (!args.addr || args.rssi===undefined)
    throw new Error("Invalid advertise message");
  var device = bleDevices[args.addr];
  if (!device) {
    console.log("New device added: ",args.addr);
    device = {
      addr : args.addr,
      bridges : {},
      dataReady : false,
      sendQueue : [],
      mqttSocket : new net.Socket(),
      lastConnected : 0
    };
    bleDevices[args.addr] = device;
    var mqttKeepAlive;
    device.mqttSocket.connect(1883, 'localhost', function() {
      console.log("MQTT socket for "+args.addr+" open");
      device.mqttSocket.write(strToBuf(mqCon(args.addr)));
      mqttKeepAlive = setInterval(function() {
        device.mqttSocket.write(strToBuf(sFCC(12<<4, 0)));
      }, 60000); // 60 sec
    });
    device.mqttSocket.on('data', function(data) {
      var cmd = data[0]>>4;
      if (cmd==MQTT_CMD.PINGRESP) return console.log("Got PINGRESP - ignoring");
      data = data.toString(); // was buffer
    	console.log(args.addr+ ' <== ' + JSON.stringify(data));
      device.sendQueue.push(data);
      handleDeviceConnection(device);
    });
    device.mqttSocket.on('close', function() {
    	console.log("MQTT socket for "+args.addr+" closed");
      if (mqttKeepAlive) clearInterval(mqttKeepAlive);
      mqttKeepAlive = undefined;
    });
  }
  device.dataReady = args.dataReady;
  device.bridges[bridgeName] = {
    rssi : args.rssi,
    lastSeen : Date.now()
  };
  // TODO: Store an RSSI average for each bridge
  if (device.dataReady || device.sendQueue.length)
    handleDeviceConnection(device);
}
function bridgeRx(bridgeName, args) {
  // args = {addr:str, data:str}
  console.log(args.addr+" ==> "+JSON.stringify(args))
  var device = bleDevices[args.addr];
  if (!device) throw new Error("Unknown device!");
  console.log(strToBuf(args.data).length);
  device.mqttSocket.write(strToBuf(args.data));
}

function handleDeviceConnection(device) {
  if (!(device.dataReady || device.sendQueue.length))
    throw new Error("handleDeviceConnection called when no need");
  if (device.lastConnected+RECONNECT_TIMEOUT > Date.now())
    return; // don't reconnect if we already did something
  var data = "";
  if (device.sendQueue.length)
    data = device.sendQueue.shift();
  // TODO: Use the nearest receiver (that is still in range) if more than one
  // TODO: Only remove from queue when there's confirmation that the data was actually sent
  mqttClient.publish("MQTToBLE/snoopy/tx", JSON.stringify({addr:device.addr, data:data}));
}

// debugging
const repl = require('repl');
var context = repl.start('> ').context;
context.dumpDevices = function() {
  console.log(JSON.stringify(bleDevices,null,2))
};
