#include "MQTT.h"
#include "esp_log.h"

//target ranges for temperature and humidity
extern float target_temp;
extern float target_humidity;

// Actuator run timers
extern int heater_run_timer;
extern int fan_run_timer;
extern int pump_run_timer;

// Sensor read interval
extern int period_sensor_read;

esp_mqtt_client_handle_t mqtt_app_start()
{

    //MQTT client
    esp_mqtt_client_handle_t client;
    
    //configure MQTT client
    const esp_mqtt_client_config_t mqtt_cfg = {
        .uri = "mqtt://mqtt.eclipseprojects.io",
        .event_handle = mqtt_event_handler
    };

    //create client and return
    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_start(client);

    return client;
}

esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event)
{
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI("MQTT_CLIENT", "MQTT_EVENT_CONNECTED");
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI("MQTT_CLIENT", "MQTT_EVENT_DISCONNECTED");
            break;
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI("MQTT_CLIENT", "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI("MQTT_CLIENT", "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI("MQTT_CLIENT", "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI("MQTT_CLIENT", "MQTT_EVENT_DATA");
            printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
            printf("DATA=%.*s\r\n", event->data_len, event->data);
            mqtt_command_handler(event);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI("MQTT_CLIENT", "MQTT_EVENT_ERROR");
            break;
        default:
            ESP_LOGI("MQTT_CLIENT", "Other");
    }
    return ESP_OK;
}

int mqtt_command_handler(esp_mqtt_event_handle_t event)
{
    char command[15] = {'\0'};
    char temp[5] = {'\0'};
    float value;
    int i = 0;
    int j = 0;

    //retrieve command
    while((event->data)[i] != '_')
    {
        command[i] = (event->data)[i];
        i++;
    }

    command[i++] = '\0';

    //retrieve value
    while((event->data)[i] != '_')
    {
        temp[j] = (event->data)[i++];
        j++;
    }

    temp[j] = '\0';

    value = atof(temp);

    //apply command and print to screen
    //set temperature
    if(!(strcmp("setTemp", command)))
    {
        target_temp = value;
        printf("Target temperature set to %.2f\n", target_temp);
    }
    //set humidity
    else if(!(strcmp("setHumid", command)))
    {
        target_humidity = value;
        printf("Target humidity set to %.2f\n", target_humidity);
    }
    //turn on heater for set duration
    else if(!(strcmp("heaterOn", command)))
    {
        heater_run_timer = (int)value;
        printf("Heater turned on for %.0f seconds\n", value);
    }
	//turn on heater for set duration
    else if(!(strcmp("fanOn", command)))
    {
        fan_run_timer = (int)value;
        printf("Fan turned on for %.0f seconds\n", value);
    }
    //turn on pump for set duration
    else if(!(strcmp("pumpOn", command)))
    {
        pump_run_timer = (int)value;
        printf("Pump turned on for %.0f seconds\n", value);
    }
    //change sensing period
    else if(!(strcmp("sensingPeriod", command)))
    {
        period_sensor_read = (int)value;
        printf("Sensing period set to %d seconds\n", period_sensor_read);
    }
    else
    {
        printf("\nError processing command.\n");
        return 0;
    }

    return 1;
}