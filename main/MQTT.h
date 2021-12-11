#ifndef COMMANDS_H_
#define COMMANDS_H_

#include "mqtt_client.h"

//Target temp and humidity
extern float target_temp;
extern float target_humidity;

extern int heater_duration;
extern int pump_duration;
extern int sensing_period;

//MQTT function declarations
esp_mqtt_client_handle_t mqtt_app_start();
esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event);
int mqtt_command_handler(esp_mqtt_event_handle_t event);

#endif
