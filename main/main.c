#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <assert.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_now.h"
#include "esp_crc.h"
#include "driver/rmt.h"
#include "driver/gpio.h"
#include "rom/ets_sys.h"

#include "MQTT.h"

// Channel parameters
#define RMT_RX_CHANNEL_0   0      //RMT channel for first dht22 sensor
#define RMT_RX_GPIO_NUM_0  19     //GPIO number for first dht22 sensor

#define RMT_RX_CHANNEL_1   1      //RMT channel for first dht22 sensor
#define RMT_RX_GPIO_NUM_1  18     //GPIO number for first dht22 sensor

#define NUM_RMT_USED 2			//Specify how many channels are in use, used for generating message queues

// GPIO parameters
#define HEATER_GPIO_NUM 	GPIO_NUM_12 // GPIO pin 12 for heater relay
#define FAN_GPIO_NUM 		GPIO_NUM_13 // GPIO pin 13 for fan relay
#define PUMP_GPIO_NUM 		GPIO_NUM_14 // GPIO pin 14 for pump relay

// Device types
#define DEVICE_RMT 		0x01
#define DEVICE_GPIO 	0x02

// 16 file descriptors seems adequate for now
#define FD_TABLE_SIZE	0x10

// Timing parameters
#define RMT_CLK_DIV      80     //RMT counter clock divider value chosen as 80MHz. Obtained by dividing 160MHz and 240MHz CPU frequency by a factor of 2 and 3
//#define RMT_TICK_10_US  (80000000/RMT_CLK_DIV/100000)   //RMT counter value = 10us
#define rmt_item32_tIMEOUT_US  200   //RMT timeout value = 200us

// Number of samples over which to average results
#define NUM_SAMPLES 3

// Wifi parameters
#define ESP_WIFI_SSID      "SSID"
#define ESP_WIFI_PASS      "Password"
#define ESP_MAXIMUM_RETRY  5

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define GPIO_18	0x12
#define GPIO_19	0x13

typedef struct
{
    float temperature;
    float humidity;
} dht_data;

typedef struct
{
	rmt_channel_t channel_num;
    float temperature;
    float humidity;
} dht_message;

typedef struct
{
    rmt_channel_t channel_num;
	gpio_num_t gpio;
} sensor_config;

typedef struct fileDescriptor { 
    uint8_t inUse;
	uint8_t devType;
	uint8_t pinNum;
	uint8_t flags;
} fileDescriptor;

// Function declarations
void management_task(void *pvParameters);
void sensing_task(sensor_config *pvParameters);
void communication_task(esp_mqtt_client_handle_t pvParameters);
void pulse_line(rmt_channel_t channel_num, gpio_num_t gpio);
static void nec_rx_init(rmt_channel_t channel_num, gpio_num_t gpio_num);
void wifi_init_sta(void);
int read_RMT(int pin, void *buf, unsigned int nbyte);
int write_GPIO(int pin, void *buf, unsigned int nbyte);
int read_GPIO(int pin, void *buf, unsigned int nbyte);
void gpio_output_init(uint64_t pins);
int open_esp32(uint16_t device, uint8_t flags);
int close_esp32(int fd);
int read_esp32(int fd, void *buf, unsigned int nbyte);
int write_esp32(int fd, void *buf, unsigned int nbyte);
int fcntl_esp32(int fd, int cmd, int flags);
int *get_bits(RingbufHandle_t ring_buffer, rmt_channel_t channel_num, gpio_num_t gpio_num);
dht_data get_temp_humidity(RingbufHandle_t ring_buffer, rmt_channel_t channel_num, gpio_num_t gpio_num);


// Message queue for passing data between sensing task and management task
xQueueHandle sensor_data_queue[NUM_RMT_USED];

// Message queue for passing data between management task and communication task
xQueueHandle communication_data_queue;

//Event group to signal when WiFi is connected and IP is acquired
static EventGroupHandle_t s_wifi_event_group;

// File Descriptor table
// Inherently global, all functions need to agree on FD state
fileDescriptor FD_Table[FD_TABLE_SIZE];

// Array to keep track of GPIO status, since IDF doesn't provide a read function
// Needs to be global for same reason as above, part of FD API
int gpio_status[39] = {0};

// Constant value used to configure sensor tasks
const sensor_config sensors[NUM_RMT_USED] = {{RMT_RX_CHANNEL_0, RMT_RX_GPIO_NUM_0}, {RMT_RX_CHANNEL_1, RMT_RX_GPIO_NUM_1}};

// Initialize environmental targets to default values, can be changed later via MQTT
// Need to be global because multiple tasks read/write these values
float target_temp = 25.0;
float temp_margin = 1.5;
float target_humidity = 25.0;

// Sensor read interval, configurable via MQTT
int period_sensor_read = 6;

// Control loop runs once per second, so these values are in seconds
int heater_activation_delay = 10;
int fan_activation_delay = 10;
int pump_activation_delay = 10;

int heater_min_runtime = 10;
int fan_min_runtime = 10;
int pump_min_runtime = 10;

int heater_run_timer = 0;
int fan_run_timer = 0;
int pump_run_timer = 0;

//Create required tasks
void app_main(void)
{
	//MQTT client
	esp_mqtt_client_handle_t client;

	//Initialize NVS, needed for wifi config
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      nvs_flash_erase();
      nvs_flash_init();
    }
	
	// Initialize wifi
    wifi_init_sta();

    //start MQTT client, subscribe to commands
    client = mqtt_app_start();
    sleep(1);
    esp_mqtt_client_subscribe(client, "esp/commands", 0);

	// Initialize queues
	for(int i = 0; i < NUM_RMT_USED; ++i)
	{
		//create a queue to recieve data from sensing task
		sensor_data_queue[i] = xQueueCreate(1, sizeof(dht_message));
	}
	//create a queue to send data to communication task
    communication_data_queue = xQueueCreate(10, sizeof(dht_message));
	
	// Initialize FD slots, marking them as available
	int i;
	for(int i = 0; i < FD_TABLE_SIZE; ++i)
	{
		FD_Table[i].inUse = 0;
	}
	
	// Initialize GPIO outputs
	uint64_t pins = (0x1 << HEATER_GPIO_NUM) | (0x1 << FAN_GPIO_NUM) | (0x1 << PUMP_GPIO_NUM);
    gpio_output_init(pins);
	
	// Create neccesary message queues
    xTaskCreatePinnedToCore(management_task, "management_task", 2048, NULL, 0, NULL, 1);
	xTaskCreatePinnedToCore(sensing_task, "Sensor 0 Monitoring Task", 2048, &sensors[0], 0, NULL, 1);
	xTaskCreatePinnedToCore(sensing_task, "Sensor 1 Monitoring Task", 2048, &sensors[1], 0, NULL, 1);
	xTaskCreatePinnedToCore(communication_task, "communication_task", 2048, client, 0, NULL, 1);
}

void management_task(void *pvParameters)
{
	// Open file descriptors for both temperature sensors
	int tempsensor1_FD = open_esp32((DEVICE_RMT << 8) | RMT_RX_GPIO_NUM_0, O_RDONLY);
	int tempsensor2_FD = open_esp32((DEVICE_RMT << 8) | RMT_RX_GPIO_NUM_1, O_RDONLY);	
    
	// Open file descriptors for all the actuators
	int pump_FD = open_esp32((DEVICE_GPIO << 8) | PUMP_GPIO_NUM, O_RDWR);
	int heater_FD = open_esp32((DEVICE_GPIO << 8) | HEATER_GPIO_NUM, O_RDWR);
	int fan_FD = open_esp32((DEVICE_GPIO << 8) | FAN_GPIO_NUM, O_RDWR);
	
	// State variable used for delayed action
	int heater_delay_counter = 0;
	int fan_delay_counter = 0;
	int pump_delay_counter = 0;
	
	int heater_is_running = 0;
	int fan_is_running = 0;
	int pump_is_running = 0;
	
	// Temporary storage of temperature/humidity data
	dht_message temperature_data;
	
	for(;;)
    {
		// Attempt to read data from sensor 0
		if(read_esp32(tempsensor1_FD, &temperature_data, sizeof(dht_message)) == 0){
			// If data is available, send to communications task for MQTT publishing
			xQueueSend(communication_data_queue, &temperature_data, 0);
		}
		// Attempt to read data from sensor `
		if(read_esp32(tempsensor2_FD, &temperature_data, sizeof(dht_message)) == 0){
			// If data is available, send to communications task for MQTT publishing
			xQueueSend(communication_data_queue, &temperature_data, 0);
		}
		// Data from sensor 1 is used for control decisions
		
		// Poll all actuators to determine state
		read_esp32(heater_FD, &heater_is_running, sizeof(int));
		read_esp32(fan_FD, &fan_is_running, sizeof(int));
		read_esp32(pump_FD, &pump_is_running, sizeof(int));
		
		// Heater controls
		if(heater_run_timer > 0){
			// Activate heater if it's not already running
			if(heater_is_running == 0){
				printf("Activating heater\n");
				write_esp32(heater_FD, 1, sizeof(int));
			}
			// Decrement run timer
			heater_run_timer--;	
		}
		else {
			// Shutoff heater if it's still running
			if(heater_is_running == 1){
				printf("Shutting off heater\n");
				write_esp32(heater_FD, 0, sizeof(int));
			}
		}
		
		// Fan controls
		if(fan_run_timer > 0){
			// Activate fan if it's not already running
			if(fan_is_running == 0){
				printf("Activating fan\n");
				write_esp32(fan_FD, 1, sizeof(int));
			}
			// Decrement run timer
			fan_run_timer--;	
		}
		else {
			// Shutoff fan if it's still running
			if(fan_is_running == 1){
				printf("Shutting off fan\n");
				write_esp32(fan_FD, 0, sizeof(int));
			}
		}
		
		// Pump controls
		if(pump_run_timer > 0){
			// Activate pump if it's not already running
			if(pump_is_running == 0){
				printf("Activating pump\n");
				write_esp32(pump_FD, 1, sizeof(int));
			}
			// Decrement run timer
			pump_run_timer--;	
		}
		else {
			// Shutoff pump if it's still running
			if(pump_is_running == 1){
				printf("Shutting off pump\n");
				write_esp32(pump_FD, 0, sizeof(int));
			}
		}
		
		// Environment conditions
		
		// Too cold
		if(temperature_data.temperature < (target_temp - temp_margin)){
			printf("Temp below threshold\n");
			if(heater_is_running == 0){
				// Increment delay counter
				heater_delay_counter++;
				
				// If delay threshold is reached, set the run timer
				if(heater_delay_counter == heater_activation_delay){
					// Reset the delay counter
					heater_delay_counter = 0;

					// Set the heater's run timer
					heater_run_timer = heater_min_runtime;
				}
			}
			else{
				if(heater_run_timer ==0){
					// If heater is still needed, extend run timer for another control cycle
					heater_run_timer++;
				}
			}
		}
		
		
		// Too hot
		else if(temperature_data.temperature > (target_temp + temp_margin)){
			printf("Temp above threshold\n");
			if(fan_is_running == 0){
				// Increment delay counter
				fan_delay_counter++;
				
				// If delay threshold is reached, set the run timer
				if(fan_delay_counter == fan_activation_delay){
					// Reset the delay counter
					fan_delay_counter = 0;

					// Set the fan's run timer
					fan_run_timer = fan_min_runtime;
				}
			}
			else{
				if(fan_run_timer ==0){
					// If fan is still needed, extend run timer for another control cycle
					fan_run_timer++;
				}
			}
		}
		
		// Too dry
		if(temperature_data.humidity < target_humidity){
			printf("Humidity below threshold\n");
			if(pump_is_running == 0){
				// Increment delay counter
				pump_delay_counter++;
				
				// If delay threshold is reached, set the run timer
				if(pump_delay_counter == pump_activation_delay){
					// Reset the delay counter
					pump_delay_counter = 0;

					// Set the pump's run timer
					pump_run_timer = pump_min_runtime;
				}
			}
			else{
				if(pump_run_timer ==0){
					// If pump is still needed, extend run timer for another control cycle
					pump_run_timer++;
				}
			}
		}

		// Wait 1 second before repeating control loop
		vTaskDelay(pdMS_TO_TICKS(1000));
    }
	// Loop should never exit, but if it does, close the open FD's
	close_esp32(tempsensor1_FD);
	//close_esp32(tempsensor2_FD);
	close_esp32(pump_FD);
	close_esp32(heater_FD);
	close_esp32(fan_FD);
}

void sensing_task(sensor_config *pvParameters)
{
	//creating ring buffer
	RingbufHandle_t ring_buffer;
	// Initialize RMT channel
	nec_rx_init(pvParameters->channel_num, pvParameters->gpio);
	// Get ring buffer handle
	rmt_get_ringbuf_handle(pvParameters->channel_num, &ring_buffer);
	// Start RMT channel
	rmt_rx_start(pvParameters->channel_num, true);
	
	// Start monitoring loop
    for(;;)
    {
		dht_data data;
		float temp = 0.0;
		float humidity = 0.0;
		
		for(int i = 0; i < NUM_SAMPLES; i++)
		{
			data = get_temp_humidity(ring_buffer, pvParameters->channel_num, pvParameters->gpio);
			temp = temp + data.temperature; 
			humidity = humidity + data.humidity;
			// According to datasheet, sensor should be read at maximum of 0.5Hz
			vTaskDelay(pdMS_TO_TICKS(2000));
		}
		
		// Calculate averaged result
		dht_message result = { pvParameters->channel_num, temp / NUM_SAMPLES, humidity / NUM_SAMPLES };
		
		// Send results to management task
		xQueueOverwrite(sensor_data_queue[pvParameters->channel_num], &result);
		
		// Sensing interval, subtract time used for sensing
        vTaskDelay(pdMS_TO_TICKS(period_sensor_read*1000 - NUM_SAMPLES*2000));
    }
}

void communication_task(esp_mqtt_client_handle_t pvParameters)
{
    for(;;)
    {
		//set test status value
		char* status = "nominal";
		char temperature[8];
		char humidity[8];
		
		dht_message data;
		if(xQueueReceive(communication_data_queue, &data, portMAX_DELAY)) {
			// When an event is found in the queue, parse the fields and print to console
			printf("Data from channel: %d\n", data.channel_num);
			printf("Temperature detected: %f\n", data.temperature);
			printf("Humidity detected: %f\n\n", data.humidity);
			
			// Convert floats to strings for MQTT publishing
			sprintf(temperature, "%2.2f", data.temperature);
			sprintf(humidity, "%2.2f", data.humidity);
			

            //publish acquired data
            if(data.channel_num == 0)
            {
                esp_mqtt_client_publish(pvParameters, "sensors/chan0", temperature, 0, 1, 0);
                sleep(1);
                esp_mqtt_client_publish(pvParameters, "sensors/chan0", humidity, 0, 1, 0);
            }
            else
            {
                esp_mqtt_client_publish(pvParameters, "sensors/chan1", temperature, 0, 1, 0);
                sleep(1);
                esp_mqtt_client_publish(pvParameters, "sensors/chan1", humidity, 0, 1, 0);
                
                //publish current status
                esp_mqtt_client_publish(pvParameters, "esp/status", status, 0, 1, 0);
            }

            
        }
    }
}

// Couldn't use 'open()' because the compiler includes a header file with conflicting definition
int open_esp32(uint16_t device, uint8_t flags){
	// Separate device type and pin number
	uint8_t devType = (device & 0xFF00) >> 8;
	uint8_t pinNum = device & 0x00FF;
	
	// Before creating a new FD, make sure no conflicting FD's exists
	for (int i = 0; i < FD_TABLE_SIZE; ++i)
	{
		// Only need to consider active FD's on the same pin
		if(FD_Table[i].inUse == 1 && FD_Table[i].pinNum == pinNum){	
			// Conditions for rejecting FD request are:
			// 1. Device type mismatch
			// 2. Existing FD has flags set other than O_RDONLY
			// 3. Requested FD has flags set other than O_RDONLY
			if(FD_Table[i].devType != devType || FD_Table[i].flags != O_RDONLY || flags != O_RDONLY){
				return -1;
			}
		}
	}
	
	// Now that FD request is validated, find an empty slot and create the FD
	for (int i = 0; i < FD_TABLE_SIZE; ++i)
	{
		// Use the first available slot
		if(FD_Table[i].inUse == 0){	
			FD_Table[i].inUse = 1;
			FD_Table[i].devType = devType;
			FD_Table[i].pinNum = pinNum;
			FD_Table[i].flags = flags;
			
			//printf("Created FD with devType %d, pinNum %d", devType, pinNum);
			
			// Return index to the FD
			return i;
		}
	}
	// If we get to this point, FD table is full, return error
	return -1;
}

int close_esp32(int fd){
	// Only need to mark the FD slot as available
	FD_Table[fd].inUse = 0;
	return 0;
}


int read_esp32(int fd, void *buf, unsigned int nbyte){
	// If FD isn't valid, or write-only flag is set, return error
	if(FD_Table[fd].inUse == 0 || (FD_Table[fd].flags & O_WRONLY) > 0){
		return -1;
	}
	if(FD_Table[fd].devType == DEVICE_RMT){
		return read_RMT(FD_Table[fd].pinNum, buf, nbyte);
	}
	if(FD_Table[fd].devType == DEVICE_GPIO){
		return read_GPIO(FD_Table[fd].pinNum, buf, nbyte);
	}
	
	return -1;
	
}

int read_RMT(int pin, void *buf, unsigned int nbyte){
	int channel = 0;
	// Need to find the RMT channel associated with given pin
	for (int i = 0; i < NUM_RMT_USED; ++i)
	{
		if(sensors[i].gpio == pin){
			channel = sensors[i].channel_num;
			break;
		}
	}
	
	dht_message data;
	// Retrieve latest value from message queue
	if(xQueueReceive(sensor_data_queue[channel], &data, 0)){
		// Upon successful retrieval from queue, copy desired bytes to destination buffer
		memcpy(buf, &data, nbyte);
		return 0;
	}
	else{
		return -1;
	}
}


int write_esp32(int fd, void *buf, unsigned int nbyte){
	// If FD isn't valid, or write flag isn't set, return error
	if(FD_Table[fd].inUse == 0 || (FD_Table[fd].flags == O_RDONLY)){
		return -1;
	}
	
	if(FD_Table[fd].devType == DEVICE_RMT){
		// Can't write to RMT
		return -1;
	}
	if(FD_Table[fd].devType == DEVICE_GPIO){
		return write_GPIO(FD_Table[fd].pinNum, buf, nbyte);
	}
	
	return -1;
}

int read_GPIO(int pin, void *buf, unsigned int nbyte){
	if(memcpy(buf, &gpio_status[pin], nbyte) == 0){
		return 0;
	}
	return -1;
}

int write_GPIO(int pin, void *buf, unsigned int nbyte){
	//Attempt to write value
	if(gpio_set_level(pin, buf) ==0){
		gpio_status[pin] = buf;
		return 0;
	}
	return -1;
}

int fcntl_esp32(int fd, int cmd, int flags){
	// If FD isn't valid return error
	if(FD_Table[fd].inUse == 0){
		return -1;
	}
	// If reading flags, return the current values
	if(cmd == F_GETFL){
		return FD_Table[fd].flags;
	}
	// If setting flags, validate request
	if(cmd == F_SETFL){
		for (int i = 0; i < FD_TABLE_SIZE; ++i)
		{
			// Need to consider other active FD's assigned to the same pin
			if(FD_Table[i].inUse == 1 && i != fd && FD_Table[i].pinNum == FD_Table[fd].pinNum){	
				// If another FD exists on the same pin, the only allowable flag is O_RDONLY
				if(flags != O_RDONLY){
					return -1;
				}
			}
		}
		// If logic made it here, no conflicting FD's exist, apply change
		FD_Table[fd].flags = flags;
		return 0;
	}
	return -1;
}

//GPIO setup
void pulse_line(rmt_channel_t channel_num, gpio_num_t gpio)
{
    //Pull data line high to request reading from sensor
    gpio_matrix_out(gpio, SIG_GPIO_OUT_IDX, 0, 0);
    gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
    ets_delay_us(900);
	
	// After specified interval, return GPIO to the RMT for recieving
    rmt_set_pin(channel_num, RMT_MODE_RX, gpio);
    PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[gpio]);   
}

//RMT rx module initialization
static void nec_rx_init(rmt_channel_t channel_num, gpio_num_t gpio_num)
{
    rmt_config_t rmt_rx;
    rmt_rx.channel = channel_num;
    rmt_rx.gpio_num = gpio_num;
    rmt_rx.clk_div = RMT_CLK_DIV;
    rmt_rx.mem_block_num = 1;
    rmt_rx.rmt_mode = RMT_MODE_RX;
    rmt_rx.rx_config.filter_en = 1;
    rmt_rx.rx_config.filter_ticks_thresh = 200;
    rmt_rx.rx_config.idle_threshold = 1000;
    rmt_config(&rmt_rx);
    rmt_driver_install(rmt_rx.channel, 1000, 0);
}

//DHT22 sensor algorithm
dht_data get_temp_humidity(RingbufHandle_t ring_buffer, rmt_channel_t channel_num, gpio_num_t gpio_num)
{
	
    int *bits;       //pointer
    int count = 0;   //counter
    
    //temp related
    int high_temp = 0;
    unsigned char first_byte_temp  = 0;
    unsigned char second_byte_temp = 0;
    
    //humidity related
    int high_humidity = 0;                        
    unsigned char first_byte_humidity  = 0;
    unsigned char second_byte_humidity = 0;

    bits = get_bits(ring_buffer, channel_num, gpio_num);

    //first two bytes of humidity
    for(int i = 0; i <= 8; i++)
    {
        //first two bits refer to the first two response pulses
        first_byte_humidity |= bits[i + 2] <<(7 - count);
        count ++;
    }
    count = 0;

    for(int i = 8; i <= 15; i++)
    {
        //first two bits refer to the first two response pulses
        second_byte_humidity |= bits[i + 2] << (7 - count);
        count ++;
    }
    count = 0;

    //first two bits refer to the first two response pulses
    for(int i = 16; i <= 23; i++)
    {
        first_byte_temp |= bits[i+2] << (7 - count);
        count ++;
    }
    count = 0;

    for(int i = 24; i <= 31; i++)
    {
        second_byte_temp |= bits[i+2] << (7 - count);
        count ++;
    }
    count = 0;

    high_humidity = first_byte_humidity << 8;
    high_temp = first_byte_temp << 8;
	
	//dht22 sensor
    dht_data data;
    data.temperature = (((float)(high_temp + second_byte_temp)) / 10);
    data.humidity = (((float)high_humidity + second_byte_humidity) / 10);
    return data;
}

//RMT and Ring Buffer
int *get_bits(RingbufHandle_t ring_buffer, rmt_channel_t channel_num, gpio_num_t gpio_num)
{
    rmt_item32_t *items = NULL;
    uint32_t length = 4;
    static int bits[42] = {0};

    pulse_line(channel_num, gpio_num);

    //items = (rmt_item32_t *) xRingbufferReceive(ring_buffer, &length, 1);
	items = xRingbufferReceive(ring_buffer, &length, 10);
	uint32_t base = items;
    if(items)
    {
		// Each RMT item is 4 bytes long, 2 bytes for each edge of the pulse
		length /= 4;
        for(int i = 0; i < length; i++){	
			
			// Each RMT reading contains two "levels", each representing a line level (High or Low) and how long it was held
			// DHT22 encodes data by modulating the duration of "High" signal
			// Determine which level reading is High, since we only care about that one
            if(items->level0 == 1)
            {
                if(items->duration0 < 30 && items->duration0 > 20)
                {
                    bits[i] = 0;
                }
                else if(items->duration0 > 70 && items->duration0 < 80)
                {
                    bits[i] = 1;
                }
            }
            else if(items->level1 == 1)
            {
				// Short pulse (26-28us) indicates data bit 0
                if(items->duration1 < 30 && items->duration1 > 20)
                {
                    bits[i] = 0;
                }
				// Long pulse (70us) indicates data bit 1
                else if(items->duration1 > 70 && items->duration1 < 80)
                {
                    bits[i] = 1;
                }
            }
            items++;
        }
		vRingbufferReturnItem(ring_buffer, base);
    }
    return &bits[0];
}

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
	int s_retry_num = 0;
	
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } 
	else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
		printf("Connecting to the AP failed\n");
        if (s_retry_num < ESP_MAXIMUM_RETRY) {
            // Retry connecting to WiFi
			printf("Retrying...\n");
			s_retry_num++;
			esp_wifi_connect();
        } 
		else {
			// Set failure bits if retry counter is met
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } 
	else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		// Parse IP address
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        printf("Acquired IP address: " IPSTR "\n", IP2STR(&event->ip_info.ip));
        // Reset retry counter
		s_retry_num = 0;
		// Set success bit
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
	// Create event group 
    s_wifi_event_group = xEventGroupCreate();

	// Initialize the network stack
    esp_netif_init();
	
	// Create the default event loop
    esp_event_loop_create_default();
	
	// Create default wifi station
    esp_netif_create_default_wifi_sta();

	// Generate default wifi configuration
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	
	// Initialize wifi driver and allocate resources
    esp_wifi_init(&cfg);

	// Create event handler hooks
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
	
	// Register event handler hooks for detecting wifi state
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip);

	// Configure custom wifi options
    wifi_config_t wifi_config = { .sta = {.ssid = ESP_WIFI_SSID, .password = ESP_WIFI_PASS}};
    
	// Set wifi mode to station (client)
	esp_wifi_set_mode(WIFI_MODE_STA);
	
	// Apply wifi client configuration
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
	
	// Start wifi service
    esp_wifi_start();

    // Wait for wifi success or failure bit to be asserted in event group
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    // Parse bits returned from event group to determine wifi status
    if (bits & WIFI_CONNECTED_BIT) {
        printf("connected to ap SSID: %s password: %s\n", ESP_WIFI_SSID, ESP_WIFI_PASS);
    } 
	else if (bits & WIFI_FAIL_BIT) {
        printf("Failed to connect to SSID: %s, password: %s\n", ESP_WIFI_SSID, ESP_WIFI_PASS);
    } 
	else {
        printf("UNEXPECTED EVENT\n");
    }

    // Unregister event loop hooks
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip);
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id);
	
	// Remove event group
    vEventGroupDelete(s_wifi_event_group);
}


//GPIO outputs initialization
void gpio_output_init(uint64_t pins)
{
    gpio_config_t io_conf = {};
	io_conf.intr_type = GPIO_INTR_DISABLE;
	io_conf.mode = GPIO_MODE_OUTPUT;
	io_conf.pin_bit_mask = pins;
	io_conf.pull_down_en = 0;
	io_conf.pull_up_en = 0;
	gpio_config(&io_conf);
	gpio_set_level(pins, 1);
}