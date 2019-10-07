/*
  +----------------------------------------------------------------------+
  | Coogle WiFi 4x4 Keypad                                               |
  +----------------------------------------------------------------------+
  | Copyright (c) 2017-2019 John Coggeshall                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0 (the "License");      |
  | you may not use this file except in compliance with the License. You |
  | may obtain a copy of the License at:                                 |
  |                                                                      |
  | http://www.apache.org/licenses/LICENSE-2.0                           |
  |                                                                      |
  | Unless required by applicable law or agreed to in writing, software  |
  | distributed under the License is distributed on an "AS IS" BASIS,    |
  | WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or      |
  | implied. See the License for the specific language governing         |
  | permissions and limitations under the License.                       |
  +----------------------------------------------------------------------+
  | Authors: John Coggeshall <john@thissmarthouse.com>                   |
  +----------------------------------------------------------------------+
*/

/**
 * Arduino libs must be included in the .ino file to be detected by the build system
 */
#include <FS.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <Keypad.h>
#include <Hash.h>
#include <PubSubClient.h>
#include "app.h"

bool ota_ready = false;
bool restart = false;
Keypad *keypad = NULL;

void setupSerial()
{
	if(Serial) {
		return;
	}

	Serial.begin(SERIAL_BAUD);

	for(int i = 0; (i < 500000) && !Serial; i++) {
		yield();
	}


	Serial.printf(APP_NAME " v%s (%s) (built: %s %s)\r\n", _BuildInfo.src_version, _BuildInfo.env_version, _BuildInfo.date, _BuildInfo.time);
}

void setupMQTT()
{
	mqttManager = new CoogleIOT_MQTT;
	mqttManager->setLogger(_ciot_log);
	mqttManager->setConfigManager(configManager);
	mqttManager->setWifiManager(WiFiManager);
	mqttManager->initialize();
	mqttManager->setClientId(APP_NAME);
	mqttManager->connect();

	mqtt = mqttManager->getClient();
}

void onNewFirmware()
{
	LOG_INFO("New Firmware available");
	LOG_INFO("Current Firmware Details");
	LOG_PRINTF(INFO, APP_NAME " v%s (%s) (built: %s %s)\r\n", _BuildInfo.src_version, _BuildInfo.env_version, _BuildInfo.date, _BuildInfo.time);

	restart = true;
}

void onNTPReady()
{
	setupOTA();
	ota_ready = true;
}

void setupNTP()
{
    NTPManager = new CoogleIOT_NTP;
	NTPManager->setLogger(_ciot_log);
    NTPManager->setWifiManager(WiFiManager);
    NTPManager->setReadyCallback(onNTPReady);
    NTPManager->initialize();

}

void setupLogging()
{
	setupSerial();
    _ciot_log = new CoogleIOT_Logger(&Serial);
    _ciot_log->initialize();

}

bool onParseConfig(DynamicJsonDocument& doc) {
	JsonObject app;

	if(!doc["app"].is<JsonObject>()) {
		LOG_ERROR("No application configuration found");
		return false;
	}

	app = doc["app"].as<JsonObject>();

	if(app["mqtt_state_topic"].is<const char *>()) {
		strlcpy(app_config->state_topic, app["mqtt_state_topic"] | "", sizeof(app_config->state_topic));

		LOG_PRINTF(INFO, "State Topic: %s", app_config->state_topic);
	}

	if(app["hold_time"].is<int>()) {
		app_config->hold_time = app["hold_time"].as<int>();
		LOG_PRINTF(INFO, "Setting Button Hold Threshold to %d ms", app_config->hold_time);
	}

	return true;
}

void setupConfig()
{
	char *t;
	app_config = (app_config_t *)os_zalloc(sizeof(app_config_t));

	configManager = CoogleIOT_Config::getInstance();
	configManager->setLogger(_ciot_log);
	configManager->setConfigStruct((coogleiot_config_base_t *)app_config);
	configManager->setParseCallback(onParseConfig);
	configManager->initialize();
}


void setupWiFi()
{
    WiFiManager = new CoogleIOT_Wifi;
    WiFiManager->setLogger(_ciot_log);
    WiFiManager->setConfigManager(configManager);

    WiFiManager->initialize();
}

void verifyUpgrade()
{
	otaManager->verifyOTAComplete();
	restart = true;
}

void setupOTA()
{
	otaManager = new CoogleIOT_OTA;
	otaManager->setLogger(_ciot_log);
	otaManager->setWifiManager(WiFiManager);
	otaManager->setNTPManager(NTPManager);
	otaManager->setCurrentVersion(_BuildInfo.src_version);
	otaManager->setConfigManager(configManager);
	otaManager->setOTACompleteCallback(onNewFirmware);
	otaManager->setUpgradeVerifyCallback(verifyUpgrade);
	otaManager->useSSL(false);
	otaManager->initialize();
}

void logSetupInfo()
{
	FSInfo fs_info;
	LOG_PRINTF(INFO, APP_NAME " v%s (%s) (built: %s %s)\r\n", _BuildInfo.src_version, _BuildInfo.env_version, _BuildInfo.date, _BuildInfo.time);

	if(!SPIFFS.begin()) {
		LOG_ERROR("Failed to start SPIFFS file system!");
	} else {
		SPIFFS.info(fs_info);
		LOG_INFO("SPIFFS File System");
		LOG_INFO("-=-=-=-=-=-=-=-=-=-");
		LOG_PRINTF(INFO, "Total Size: %d byte(s)\nUsed: %d byte(s)\nAvailable: ~ %d byte(s)", fs_info.totalBytes, fs_info.usedBytes, fs_info.totalBytes - fs_info.usedBytes);
	}
}

void onKeypadEvent(KeypadEvent key) 
{
	static KeypadEvent lastEvent;
	static KeyState lastState;

	app_config_t *config;	

	if(!configManager) {
		return;
	}

	if(!configManager->loaded) {
		return;
	}

	if(!mqttManager->connected()) {
		return;
	}

	config = (app_config_t *)configManager->getConfig();

	switch(keypad->getState()) {

		case PRESSED:
			lastState = PRESSED;
			break;
		case RELEASED:
			if(lastState != HOLD) {
				LOG_PRINTF(DEBUG, "Released: %c\n", key);
				mqtt->publish(config->state_topic, &key, 1);
			}
			lastState = RELEASED;
			break;
		case HOLD:
			char t[2];
			LOG_PRINTF(DEBUG, "Holding: %c\n", key);
			
			t[0] = 'H';
			t[1] = key;
			mqtt->publish(config->state_topic, &t[0], 2);
			lastState = HOLD;
			break;
	}

	lastEvent = key;
}

void setupKeypad()
{
	app_config_t *config;

	if(!configManager) {
		LOG_ERROR("Could not setup Keypad - no config manager");
		return;
	}

	if(!configManager->loaded) {
		LOG_ERROR("Could not setup Keypad - config manager not loaded");
		return;
	}

	config = (app_config_t *)configManager->getConfig();

	keypad = new Keypad(makeKeymap(keymap), keypad_row_pins, keypad_col_pins, keypad_rows, keypad_cols);
    keypad->addEventListener(onKeypadEvent);
    keypad->setHoldTime(config->hold_time);
}

void setup()
{
    randomSeed(micros());

    setupLogging();
    setupConfig();

	logSetupInfo();

    setupWiFi();
    setupNTP();
    setupMQTT();

    setupKeypad();

    // Give the logger an NTP Manager so it can record timestamps with logs
    _ciot_log->setNTPManager(NTPManager);
    
}

void loop()
{
	if(keypad) {
		keypad->getKey();
	}

	if(restart) {
		ESP.restart();
		return;
	}

	WiFiManager->loop();
	NTPManager->loop();
	mqttManager->loop();

	if(ota_ready) {
		otaManager->loop();
	}


}
