#include <WiFi.h>
#include <PubSubClient.h>
#include <atomic>
#include "time.h"
#include "esp_sntp.h"
#include "parameters.h"

const int LightSensorPin = 32;
const int LightSensorThreashold = 500;

int lightSensorValue = 0;
int previousLightSensorValue = 0;
char lightSensorState[4] = "off";
std::atomic<bool> lightSensorStateChanged = false;
unsigned long lightSensorChangedTime = 0;

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);  // MQTT

void checkLightSensorState() {
  lightSensorValue = analogRead(LightSensorPin);
  if (std::abs(lightSensorValue - previousLightSensorValue) >= 50) {
    Serial.printf("LightSensorValue is %d\n", lightSensorValue);
    previousLightSensorValue = lightSensorValue;

    if (lightSensorValue > LightSensorThreashold) {
      // Light is ON, check if we need to update state
      // Capture change if previous state was off, and that state was captured at least 1s ago
      if (strcmp(lightSensorState, "off") == 0 && (millis() - lightSensorChangedTime) >= 1000) {
        strncpy(lightSensorState, "on", 4);
        lightSensorStateChanged = true;
        lightSensorChangedTime = millis();
      }
    } else {
      // Light is OFF, check if we need to update state
      // Capture change if previous state was on, and that state was captured at least 1s ago
      if (strcmp(lightSensorState, "on") == 0 && (millis() - lightSensorChangedTime) >= 1000) {
        strncpy(lightSensorState, "off", 4);
        lightSensorStateChanged = true;
        lightSensorChangedTime = millis();
      }
    }
  }

  // Report new state if it has actually changed
  if (lightSensorStateChanged && strcmp(lightSensorState, "on") == 0) {
    Serial.println("Light is on");
    // Message for ESP32 display module
    mqttClient.publish("garageLight/lightStateChanged", "on");
    // Message for Home Assitant
    mqttClient.publish("garage/light/sensor", "ON");
    lightSensorStateChanged = false;
  } else if (lightSensorStateChanged && strcmp(lightSensorState, "off") == 0) {
    Serial.println("Light is off");
    // Message for ESP32 display module
    mqttClient.publish("garageLight/lightStateChanged", "off");
    // Message for Home Assitant
    mqttClient.publish("garage/light/sensor", "OFF");
    lightSensorStateChanged = false;
  }
}

void ensureMqttBrokerConnected() {
  // Reconnect if not connected
  if (!mqttClient.connected()) {
    Serial.printf("Connecting to %s...\n", mqttServer);
    
    // Attempt to connect
    if (mqttClient.connect("GarageLightSensor", mqtt_user, mqtt_password)) {
      Serial.printf("connected to %s\n", mqttServer);
      // Subscribe to topics
      mqttClient.subscribe("garageLight/queryDeviceStatus");
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println();
    }
  }
}

void ensureWifiConnected() {
  if (WiFi.status() != WL_CONNECTED) { 
    Serial.println("WiFi disconnected! Reconnecting..."); 
    WiFi.disconnect(); 
    WiFi.begin(wifi_ssid, wifi_password); 
    unsigned long startAttemptTime = millis();
    
    // Try for up to 10 seconds 
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) { 
      delay(250); 
      Serial.print("."); 
    } 
    
    if (WiFi.status() == WL_CONNECTED) { 
      Serial.println("WiFi reconnected"); 
    } else { 
      Serial.println("WiFi reconnect failed"); 
    } 
  }
}

void printLocalTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("No time available (yet)");
    return;
  }
  Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
}

// Callback function (gets called when time adjusts via NTP)
void timeavailable(struct timeval *t) {
  Serial.println("Got time adjustment from NTP!");
  printLocalTime();
}

void incomingMqttMessage(char *topic, uint8_t *message, unsigned int length) {
  Serial.print("Message received on topic: ");
  Serial.println(topic);

  Serial.print("Message: ");

  String value = "";

  for (int i = 0; i < length; i++) {
    Serial.print((char)message[i]);
    value += (char)message[i];
  }

  Serial.println();

  if (strcmp(topic, "garageLight/queryDeviceStatus") == 0) {
    // Send current state of garage light sensor
    if (digitalRead(LightSensorPin) == HIGH) {
      strncpy(lightSensorState, "on", 4);
      lightSensorStateChanged = true;
    } else {
      strncpy(lightSensorState, "off", 4);
      lightSensorStateChanged = true;
    }
  }
}

void publishDiscoveryMqttMessage() {
  String lightSensorDiscoveryMessage = "{"
    "\"name\": \"Garage Light\","
    "\"unique_id\": \"garage_light_sensor\","
    "\"state_topic\": \"garage/light/sensor\","
    "\"payload_on\": \"ON\","
    "\"payload_off\": \"OFF\","
    "\"device_class\": \"light\","
    "\"icon\": \"mdi:garage\""
  "}";
  Serial.println("Publishing Home Assistant Binary Sensor Discovery Message");
  mqttClient.publish("homeassistant/binary_sensor/garage_light_sensor/config", lightSensorDiscoveryMessage.c_str(), true); // true means the message is retained when HA is restarted
}

void subscribeToMqttTopics() {
  if (mqttClient.subscribe("garageLight/queryDeviceStatus")) {
    Serial.println("Subscribed to topic: garageLight/queryDeviceStatus");
  } else {
    Serial.println("Failed to subscribe to topic!");
  }
}

void setup() {
  Serial.begin(115200);

  // First step is to configure WiFi STA and connect in order to get the current time and date.
  Serial.printf("Connecting to %s ", wifi_ssid);
  WiFi.begin(wifi_ssid, wifi_password);

  /**
   * NTP server address could be acquired via DHCP,
   *
   * NOTE: This call should be made BEFORE esp32 acquires IP address via DHCP,
   * otherwise SNTP option 42 would be rejected by default.
   * NOTE: configTime() function call if made AFTER DHCP-client run
   * will OVERRIDE acquired NTP server address
   */
  esp_sntp_servermode_dhcp(1);  // (optional)

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" CONNECTED");

  // set notification call-back function
  sntp_set_time_sync_notification_cb(timeavailable);

  /**
   * This will set configured ntp servers and constant TimeZone/daylightOffset
   * should be OK if your time zone does not need to adjust daylightOffset twice a year,
   * in such a case time adjustment won't be handled automagically.
   */
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2);

  /**
   * A more convenient approach to handle TimeZones with daylightOffset
   * would be to specify a environment variable with TimeZone definition including daylight adjustmnet rules.
   * A list of rules for your zone could be obtained from https://github.com/esp8266/Arduino/blob/master/cores/esp8266/TZ.h
   */
  //configTzTime(time_zone, ntpServer1, ntpServer2);

  // Connect to MQTT broker
  mqttClient.setServer(mqttServer, 1883);
  mqttClient.setCallback(incomingMqttMessage);

  // Connect to MQTT broker
  mqttClient.setServer(mqttServer, 1883);
  mqttClient.setCallback(incomingMqttMessage);

  while (!mqttClient.connected()) {
    ensureWifiConnected();
    ensureMqttBrokerConnected();
  }

  if (mqttClient.connected()) {
    // Register this device as a sensor for the emergency stop button in Home Assistant
    publishDiscoveryMqttMessage();
  } else {
    Serial.println("MQTT not connected - cannot publish discovery message");
  }
}

void loop() {
  static unsigned long lastCheckedLightState = 0;

  // Reconnect both WiFi and MQTT if connection is broken
  ensureWifiConnected();
  ensureMqttBrokerConnected();

  mqttClient.loop(); // Process incoming MQTT messages

  // Handle light state
  if (millis() - lastCheckedLightState >= 100) {
    checkLightSensorState();
    lastCheckedLightState = millis();  // Reset timing
  }
}