/* Deep sleep wake up example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "driver/rtc_io.h"
#include "soc/rtc.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_check.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#define TIMER_WAKEUP_TIME_US (60 * 1000 * 1000)

/* FreeRTOS event group to signal when we are connected properly */
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about one event
 * - are we connected to the AP with an IP? */
const int CONNECTED_BIT = BIT0;
const int PUBLISHED_BIT = BIT1;

int jump = 1;
uint64_t times;

static const char* TAG = "main";
const char* senddata = CONFIG_DATA_STARTUP;



static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    // your_context_t *context = event->context;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            msg_id = esp_mqtt_client_publish(client, CONFIG_TOPIC, senddata, 0, 1, 0);
            ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            break;
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            xEventGroupSetBits(wifi_event_group,PUBLISHED_BIT);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT_EVENT_DATA");
            printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
            printf("DATA=%.*s\r\n", event->data_len, event->data);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            break;
        default:
            ESP_LOGI(TAG, "Other event id:%d", event->event_id);
            break;
    }
    return ESP_OK;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32 "", base, event_id);
    mqtt_event_handler_cb(event_data);
}

static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = { 
        .broker.address.uri = CONFIG_BROKER_URL,
    };
#if CONFIG_BROKER_URL_FROM_STDIN
    char line[128];

    if (strcmp(mqtt_cfg.uri, "FROM_STDIN") == 0) {
        int count = 0;
        printf("Please enter url of mqtt broker\n");
        while (count < 128) {
            int c = fgetc(stdin);
            if (c == '\n') {
                line[count] = '\0';
                break;
            } else if (c > 0 && c < 127) {
                line[count] = c;
                ++count;
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
        mqtt_cfg.uri = line;
        printf("Broker url: %s\n", line);
    } else {
        ESP_LOGE(TAG, "Configuration mismatch: wrong broker url");
        abort();
    }
#endif /* CONFIG_BROKER_URL_FROM_STDIN */

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    esp_mqtt_client_start(client);
}

static void event_handler(void* arg, esp_event_base_t event_base,
    int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        /* This is a workaround as ESP32 WiFi libs don't currently auto-reassociate. */
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
    }
}

void initWifi(void){
    ESP_ERROR_CHECK(esp_netif_init());
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *Host = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(Host,CONFIG_HOSTNAME);               //Set Hostname of the Device

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_max_tx_power(84);
    esp_wifi_set_country_code("DE", true);

    wifi_config_t wifi_config = {                           //Set Wifi Config
        .sta = {                                            //WIFI Mode STATION
            .ssid = CONFIG_WIFI_SSID,                       //AP SSID is set in Menueconfig
            .password = CONFIG_WIFI_PASSWORD,               //AP password is set in Menueconfig
            .bssid_set = false,                             //Set BSSID of target AP ?
            .pmf_cfg = {                                    //Protected Managment Frame
                .capable = true,                            //Capable ?
                .required = false,                           //Required ?
            }   
        }
    };

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL,&instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL,&instance_got_ip));

    ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");
}

esp_err_t register_timer_wakeup(uint64_t time_in_us)
{
    ESP_RETURN_ON_ERROR(esp_sleep_enable_timer_wakeup(time_in_us), TAG, "Configure timer as wakeup source failed");
    ESP_LOGI(TAG, "timer wakeup source is ready");
    return ESP_OK;
}

void app_main(void)
{
    uint64_t wakeup_pin_mask = esp_sleep_get_ext1_wakeup_status();
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    

    const int ext_wakeup_pin_1 = 2;
    const uint64_t ext_wakeup_pin_1_mask = 1ULL << ext_wakeup_pin_1;
    const int ext_wakeup_pin_2 = 4;
    const uint64_t ext_wakeup_pin_2_mask = 1ULL << ext_wakeup_pin_2;
    //vTaskDelay(1000 / portTICK_PERIOD_MS);
    
    jump=1;
    rtc_gpio_init(GPIO_NUM_2);
    rtc_gpio_init(GPIO_NUM_4);
    rtc_gpio_set_direction(GPIO_NUM_2, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_set_direction(GPIO_NUM_4, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_dis(GPIO_NUM_2);
    rtc_gpio_pullup_dis(GPIO_NUM_4);
    rtc_gpio_pulldown_en(GPIO_NUM_2);
    rtc_gpio_pulldown_en(GPIO_NUM_4);
    rtc_gpio_hold_en(GPIO_NUM_2);
    rtc_gpio_hold_en(GPIO_NUM_4);
    bool GPIO2 = rtc_gpio_get_level(GPIO_NUM_2);
    bool GPIO4 = rtc_gpio_get_level(GPIO_NUM_4);
    printf("Pin2:%s&Pin4:%s\n", GPIO2 ? "true" : "false", GPIO4 ? "true" : "false");
    
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        size_t required_size;
        err = nvs_get_str(nvs, "key", NULL, &required_size);
        if (err == ESP_OK) {
            char* saved_data = (char*)malloc(required_size);
            if (saved_data != NULL) {
                err = nvs_get_str(nvs, "key", saved_data, &required_size);
                if (err == ESP_OK) {
                    // Use saved_data (the retrieved string)
                    printf("Retrieved string: %s\n", saved_data);
                    senddata = saved_data;
                }
                //free(saved_data); // Don't forget to free the allocated memory
            }
        }
        nvs_close(nvs);
    }

    switch (esp_sleep_get_wakeup_cause()) {
        case ESP_SLEEP_WAKEUP_EXT1:
            if (wakeup_pin_mask != 0) {
            int pin = __builtin_ffsll(wakeup_pin_mask) - 1;
            printf("Wake up from GPIO %d\n", pin);
                if (pin == 4){
                    senddata = CONFIG_DATA_PIN4;
                    }else if (pin == 2)
                {
                    senddata = CONFIG_DATA_PIN2;
                }
            } else {
                printf("Wake up from GPIO\n");
            }
            break;
        case ESP_SLEEP_WAKEUP_TIMER:
            printf("Wake up from Timer\n");
            //printf(senddata);
            if (GPIO2 && GPIO4 == 0 && strcmp(senddata, CONFIG_DATA_PIN2_TIME) != 0){
                printf("GPIO 2 High\n");
                senddata = CONFIG_DATA_PIN2_TIME; 
            }else if(GPIO4 && GPIO2 == 0 && strcmp(senddata, CONFIG_DATA_PIN4_TIME) != 0){
                printf("GPIO 4 High\n");
                senddata = CONFIG_DATA_PIN4_TIME;
            }else if(GPIO2 && GPIO4 && strcmp(senddata, CONFIG_DATA_ERROR) != 0){
                printf("GPIO 2 & 4 High\n");
                senddata =CONFIG_DATA_ERROR;
            }else {
                jump = 0;
            }
            break;
        default:
            senddata = CONFIG_DATA_STARTUP;
            break;
    }
    if (jump){
        initWifi();
        xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
        vTaskDelay(1000 / portTICK_PERIOD_MS); //wait for stable connection
        mqtt_app_start();
        xEventGroupWaitBits(wifi_event_group, PUBLISHED_BIT, false, true, portMAX_DELAY);     
    } 
    GPIO2 = rtc_gpio_get_level(GPIO_NUM_2);
    GPIO4 = rtc_gpio_get_level(GPIO_NUM_4);
    printf("Pin2:%s&Pin4:%s\n", GPIO2 ? "true" : "false", GPIO4 ? "true" : "false");
    if (GPIO2 || GPIO4){
        /* Enable wakeup from light sleep by timer */
        if (jump == 0){
            times = (30 * 60 * 1000 * 1000);
        }else {
            times = TIMER_WAKEUP_TIME_US;
        }

        register_timer_wakeup(times);
        if (GPIO2 && GPIO4 == 0){
            esp_sleep_enable_ext1_wakeup(ext_wakeup_pin_2_mask, ESP_EXT1_WAKEUP_ANY_HIGH);
            printf("Enable Pin 4\n");
        }else if (GPIO4 && GPIO2 == 0){
            esp_sleep_enable_ext1_wakeup(ext_wakeup_pin_1_mask, ESP_EXT1_WAKEUP_ANY_HIGH);
            printf("Enable Pin 2\n");
        }
  
    }else esp_sleep_enable_ext1_wakeup(ext_wakeup_pin_1_mask | ext_wakeup_pin_2_mask, ESP_EXT1_WAKEUP_ANY_HIGH);

#if CONFIG_IDF_TARGET_ESP32
    // Isolate GPIO12 pin from external circuits. This is needed for modules
    // which have an external pull-up resistor on GPIO12 (such as ESP32-WROVER)
    // to minimize current consumption.
    rtc_gpio_isolate(GPIO_NUM_12);
#endif

    err = nvs_open("storage", NVS_READWRITE, &nvs);
if (err == ESP_OK) {
    err = nvs_set_str(nvs, "key", senddata);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
}

    printf("Entering deep sleep\n");
    esp_deep_sleep_start();
}

