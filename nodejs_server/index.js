// Node.js MQTT->BLE Server for https://github.com/gfwilliams/MQTToBLE

var MQTT_BRIDGE_SERVER = "mqtt://frank.local"; // server that bridges are connected to
var MQTT_SERVER = "localhost"; // server we're connecting to
var RECONNECT_TIMEOUT = 20*1000;
var BLE_MAX_PACKET_SIZE = 20;

var bleDevices = {};
var net = require('net');
var mqtt = require('mqtt');
console.log("MQTT> Connecting...");
var mqttClient  = mqtt.connect(MQTT_BRIDGE_SERVER);
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
function strToArray(str) {
  var b = new Array(str.length);
  for (var i=0;i<str.length;i++)
    b[i]=str.charCodeAt(i);
  return b;
}
function bufferToArray(buf) {
  var b = new Array(buf.length);
  for (var i=0;i<buf.length;i++)
    b[i]=buf.readUInt8(i);
  return b;
}
function arrayToBuf(array) {
  var b = Buffer.alloc(array.length);
  for (var i=0;i<array.length;i++)
    b.writeUInt8(array[i],i);
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
      sendQueue : [], // array of arrays of data
      mqttSocket : new net.Socket(),
      lastConnected : 0
    };
    bleDevices[args.addr] = device;
    var mqttKeepAlive;
    device.mqttSocket.connect(1883, MQTT_SERVER, function() {
      console.log("MQTT socket for "+args.addr+" open");
      device.mqttSocket.write(strToBuf(mqCon(args.addr)));
      mqttKeepAlive = setInterval(function() {
        device.mqttSocket.write(strToBuf(sFCC(12<<4, 0)));
      }, 60000); // 60 sec
    });
    device.mqttSocket.on('data', function(data) {
      var cmd = data[0]>>4;
      if (cmd==MQTT_CMD.PINGRESP) return console.log("Got PINGRESP - ignoring");
      data = bufferToArray(data); // was buffer
    	console.log(args.addr+ ' <== ' + data);
      while (data!==undefined) {
        var d;
        if (data.length <= BLE_MAX_PACKET_SIZE)  {
          d = data;
          data = undefined;
        } else {
          d = data.slice(0,BLE_MAX_PACKET_SIZE);
          data = data.slice(BLE_MAX_PACKET_SIZE);
        }
        console.log('... ' + d);
        device.sendQueue.push(d);
      }
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
    name : bridgeName,
    rssi : args.rssi,
    lastSeen : Date.now()
  };
  // TODO: Store an RSSI average for each bridge
  if (device.dataReady || device.sendQueue.length)
    handleDeviceConnection(device);
}
function bridgeRx(bridgeName, args) {
  // args = {addr:str, data:str}
  console.log(args.addr+" ==> "+args.data)
  var device = bleDevices[args.addr];
  if (!device) throw new Error("Unknown device!");
  device.mqttSocket.write(arrayToBuf(args.data));
}

function handleDeviceConnection(device) {
  if (!(device.dataReady || device.sendQueue.length))
    throw new Error("handleDeviceConnection called when no need");
  if (device.lastConnected+RECONNECT_TIMEOUT > Date.now())
    return; // don't reconnect if we already did something
  var data = [];
  if (device.sendQueue.length)
    data = device.sendQueue.shift();
  // Use the nearest receiver (that is still in range) if more than one
  var bridge;
  Object.keys(device.bridges).forEach(name=>{
    var b = device.bridges[name];
    if (!bridge || b.rssi>bridge.rssi)
      bridge = b;
  });
  if (!bridge) throw new Error("No bridge for "+device.addr+"!");
  mqttClient.publish("MQTToBLE/"+bridge.name+"/tx", JSON.stringify({addr:device.addr, data:data}));
  // TODO: Only remove from queue and send another when there's confirmation that the data was actually sent
  if (device.sendQueue.length) setTimeout(function() {
    handleDeviceConnection(device);
  }, 100);
}

// debugging
const repl = require('repl');
var context = repl.start('> ').context;
context.dumpDevices = function() {
  console.log(JSON.stringify(bleDevices,null,2))
};
