/* Copyright (c) 2015 Ollie Phillips. See the file LICENSE for copying permission. */
/* Stripped out MQTT module that does basic PUBSUB.
   Originally from https://github.com/olliephillips/tinyMQTT */

var _q;
var TMQ = function(server, optns){
	var opts = optns || {};
	_q = this;
};

var sFCC = String.fromCharCode;

TMQ.prototype.onData = function(data) {
	var cmd = data.charCodeAt(0);
	if((cmd >> 4) === 3) {
		var var_len = data.charCodeAt(2) << 8 | data.charCodeAt(3);
		var msg = {
			topic: data.substr(4, var_len),
			message: data.substr(4+var_len, (data.charCodeAt(1))-var_len)
		};
		_q.emit("message", msg);
	}
};

function mqStr(str) {
	return sFCC(str.length >> 8, str.length&255) + str;
}

function mqPkt(cmd, variable, payload) {
	return sFCC(cmd, variable.length + payload.length) + variable + payload;
}

TMQ.prototype.subscribe = function(topic) {
	_q.cl.write(mqPkt((8 << 4 | 2), sFCC(1<<8, 1&255), mqStr(topic)+sFCC(1)));
};

TMQ.prototype.publish = function(topic, data) {
	if((topic.length + data.length) > 127) {throw "tMQTT-TL";}
    _q.cl.write(mqPkt(0b00110001, mqStr(topic), data));
    _q.emit("published");
};

var mqttOut = "";
var mqttPushTimeout = undefined;
var mqtt = new TMQ("BLE", {});
mqtt.cl = {write:function(d) { 
  console.log(d.length);
  mqttOut+=d; 
  if (NRF.getSecurityStatus().connected)
    pushMQTTData();
  else
    mqttHasData(true); 
}};
function mqttHasData(d) {
  // 2 byte, flags
  // 17 byte, complete 128 bit uuid list,
  NRF.setAdvertising([2,1,6,17,7,18, 195, 81, 99, 210, 101, 252, 63, 31, 128, 190, 67, d?1:0, 0, 145, 172]);
}

function pushMQTTData() {
  if (!mqttOut.length) {
    mqttHasData(false);
    return;
  }
  if (mqttPushTimeout) return; // we'll get called back soon anyway
  if (!NRF.getSecurityStatus().connected) return; // no connection
  var d = mqttOut.substr(0,20);
  mqttOut = mqttOut.substr(20);
  NRF.updateServices({
  'ac910001-43be-801f-3ffc-65d26351c312' : {
    'ac910003-43be-801f-3ffc-65d26351c312': { // rx - from node TO bridge
      value : d,
      notify: true
    }
  }});
  if (mqttOut.length) {
    mqttPushTimeout = setTimeout(function() {
      mqttPushTimeout = undefined;
      pushMQTTData();
    },100);
  }
}
NRF.setServices({
  'ac910001-43be-801f-3ffc-65d26351c312' : {
    'ac910002-43be-801f-3ffc-65d26351c312' : { // tx - from bridge TO node
      writable: true,
      value : "",
      maxLen : 20,
      onWrite : function(evt) {
        if (evt.data.length) mqtt.onData(E.toString(evt.data));
        pushMQTTData();
      }
    }, 'ac910003-43be-801f-3ffc-65d26351c312': { // rx - from node TO bridge
      notify: true,
      value : "",
      maxLen : 20
    }
  }
});
NRF.setScanResponse([]); // remove scan response packet
mqttHasData(0);
 
mqtt.subscribe("espruino/test");
mqtt.on("message", function(msg){
    console.log(msg.topic);
    console.log(msg.message);
});
mqtt.on("published", function(){
    console.log("message sent");
});


