#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event_loop.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "mqtt.h"
#include "driver/gpio.h"

#define PIN_OUTPUT  GPIO_NUM_23
#define PIN_INPUT   GPIO_NUM_22

const gpio_config_t g_gpio_config[] = {
    { 1<<PIN_OUTPUT, GPIO_MODE_OUTPUT|GPIO_MODE_INPUT, GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_DISABLE }
,   { 1<<PIN_INPUT,  GPIO_MODE_INPUT,                  GPIO_PULLUP_ENABLE,  GPIO_PULLDOWN_DISABLE, GPIO_INTR_NEGEDGE }
,   { 0 }
};

static SemaphoreHandle_t g_semaphore = NULL;
static mqtt_client *g_mqtt_client = NULL;

static void IRAM_ATTR my_isr_handler(void* arg)
{
    if(g_semaphore)
    {
        if(pdTRUE != xSemaphoreGiveFromISR(g_semaphore, NULL))
        {   // Handle the error
        } 
    }
}

static void my_task(void* arg)
{
    static TickType_t wait_for = 5000 / portTICK_PERIOD_MS;

    if (g_semaphore)
    {
        for(;;)
        {
            if(pdTRUE == xSemaphoreTake(g_semaphore, wait_for))
            {
                if(ESP_OK==gpio_set_level(PIN_OUTPUT, !gpio_get_level( PIN_OUTPUT )))
                {
                    printf("Setting level %u for pin #%u!\n", gpio_get_level( PIN_OUTPUT ), PIN_OUTPUT);
                }
                else 
                {
                    printf("ERROR during gpio_set_level for pin #%u!\n", PIN_OUTPUT);
                }
            }

            if(g_mqtt_client)
            {
                char s[10] = { 0 };
                sprintf(s, "OUT=%u",gpio_get_level( PIN_OUTPUT ));
                mqtt_publish(g_mqtt_client, "/test", s, strlen(s), 0, 0);
            }
        }
    }
    else
    {
        printf("ERROR. The semaphore must be created first!\n");
    }
}


const char *MQTT_TAG = "MQTT_SAMPLE";

void connected_cb(mqtt_client *client, mqtt_event_data_t *event_data)
{
    g_mqtt_client = client;
    mqtt_subscribe(client, "/test", 0);
    mqtt_publish(client, "/test", "BEGIN!", 6, 0, 0);
}
void disconnected_cb(mqtt_client *client, mqtt_event_data_t *event_data)
{
    g_mqtt_client = NULL;
    ESP_LOGI(MQTT_TAG, "[APP] disconnected callback");
}
void reconnect_cb(mqtt_client *client, mqtt_event_data_t *event_data)
{ 
    g_mqtt_client = client;
    ESP_LOGI(MQTT_TAG, "[APP] reconnect callback");
}
void subscribe_cb(mqtt_client *client, mqtt_event_data_t *event_data)
{
    ESP_LOGI(MQTT_TAG, "[APP] Subscribe ok, test publish msg");
    mqtt_publish(client, "/test", "abcde", 5, 0, 0);
}

void publish_cb(mqtt_client *client, mqtt_event_data_t *event_data)
{
    ESP_LOGI(MQTT_TAG, "[APP] publish callback"); 
}
void data_cb(mqtt_client *client, mqtt_event_data_t *event_data)
{
    if(event_data->data_offset == 0) {

        char *topic = malloc(event_data->topic_length + 1);
        memcpy(topic, event_data->topic, event_data->topic_length);
        topic[event_data->topic_length] = 0;
        ESP_LOGI(MQTT_TAG, "[APP] Publish topic: %s", topic);
        free(topic);
    }

    char *data = malloc(event_data->data_length + 1);
    memcpy(data, event_data->data, event_data->data_length);
    data[event_data->data_length] = 0;

    ESP_LOGI(MQTT_TAG, "[APP] Publish data[%d/%d bytes]",
             event_data->data_length + event_data->data_offset,
             event_data->data_total_length);

    if(g_semaphore && 0==strcmp(data, "TOGGLE"))
    {
        if(pdTRUE != xSemaphoreGive(g_semaphore))
        {   // Handle the error
        }
    }

    ESP_LOGI(MQTT_TAG, "[APP] Publish data[%s]", data);

    free(data);

}

mqtt_settings settings = {
    .host = "192.168.34.18",
#if defined(CONFIG_MQTT_SECURITY_ON)
    .port = 8883, // encrypted
#else
    .port = 1883, // unencrypted
#endif
    .client_id = "mqtt_client_id",
    .username = "user",
    .password = "pass",
    .clean_session = 0,
    .keepalive = 120,
    .lwt_topic = "/test",
    .lwt_msg = "offline",
    .lwt_qos = 0,
    .lwt_retain = 0,
    .connected_cb = connected_cb,
    .disconnected_cb = disconnected_cb,
    .subscribe_cb = subscribe_cb,
    .publish_cb = publish_cb,
    .data_cb = data_cb
};



static esp_err_t wifi_event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
        case SYSTEM_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case SYSTEM_EVENT_STA_GOT_IP:
            mqtt_start(&settings);
            //init app here
            break;
        case SYSTEM_EVENT_STA_DISCONNECTED:
            /* This is a workaround as ESP32 WiFi libs don't currently
               auto-reassociate. */
            esp_wifi_connect();
            mqtt_stop();
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void wifi_conn_init(void)
{
    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_init(wifi_event_handler, NULL));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_LOGI(MQTT_TAG, "start the WIFI SSID:[%s] password:[%s]", CONFIG_WIFI_SSID, "******");
    ESP_ERROR_CHECK(esp_wifi_start());
}

void app_main()
{
    ESP_LOGI(MQTT_TAG, "[APP] Startup..");
    ESP_LOGI(MQTT_TAG, "[APP] Free memory: %d bytes", system_get_free_heap_size());
    ESP_LOGI(MQTT_TAG, "[APP] SDK version: %s, Build time: %s", system_get_sdk_version(), BUID_TIME);

    int i;


    for(i=0; g_gpio_config[i].pin_bit_mask; ++i)
    {
        if(ESP_OK!=gpio_config(&g_gpio_config[i]))
        {
            printf("ERROR during gpio_config for 0x%016llX mask!\n", g_gpio_config[i].pin_bit_mask);
        }
    }

    //create a semaphore to synchronize events between the ISR and the worker task
    g_semaphore = xSemaphoreCreateBinary();

    //start worker task
    xTaskCreate(my_task, "my_task", 2048, NULL, 10, NULL);

    //install gpio isr service
    gpio_install_isr_service(0);

    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(PIN_INPUT, my_isr_handler, (void*) PIN_INPUT);

    nvs_flash_init();
    wifi_conn_init();

    for(;;)
    {   // Wait forever
        vTaskDelay( portMAX_DELAY );
    } 
}
