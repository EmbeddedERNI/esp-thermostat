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
#include "freertos/event_groups.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "mqtt.h"
#include "driver/gpio.h"

#include "comm.h"

static mqtt_client    *g_mqtt_client = NULL;
static comm_on_data_t  g_on_data = NULL;

extern const char *MQTT_TAG;

void connected_cb(mqtt_client *client, mqtt_event_data_t *event_data)
{
    ESP_LOGI(MQTT_TAG, "[APP] connected callback");
    g_mqtt_client = client;
    mqtt_subscribe(client, CONFIG_MQTT_TOPIC_DEFAULT, 0);
    mqtt_publish(client, CONFIG_MQTT_TOPIC_DEFAULT, "BEGIN!", 6, 0, 0);
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
    g_mqtt_client = client;
    ESP_LOGI(MQTT_TAG, "[APP] Subscribe ok, test publish msg");
    mqtt_publish(client, CONFIG_MQTT_TOPIC_DEFAULT, "abcde", 5, 0, 0);
}

void publish_cb(mqtt_client *client, mqtt_event_data_t *event_data)
{
    g_mqtt_client = client;
    ESP_LOGI(MQTT_TAG, "[APP] publish callback"); 
}
void data_cb(mqtt_client *client, mqtt_event_data_t *event_data)
{
    ESP_LOGI(MQTT_TAG, "[APP] data callback"); 
    char *topic = NULL;

    if(event_data->data_offset == 0) {

        topic = malloc(event_data->topic_length + 1);
        memcpy(topic, event_data->topic, event_data->topic_length);
        topic[event_data->topic_length] = 0;
        ESP_LOGI(MQTT_TAG, "[APP] Publish topic: %s", topic);
    }

    char *data = malloc(event_data->data_length + 1);
    memcpy(data, event_data->data, event_data->data_length);
    data[event_data->data_length] = 0;

    ESP_LOGI(MQTT_TAG, "[APP] Publish data[%d/%d bytes]",
             event_data->data_length + event_data->data_offset,
             event_data->data_total_length);

    ESP_LOGI(MQTT_TAG, "[APP] Publish data[%s]", data);
    if(g_on_data)
    {
        g_on_data(topic, data);
    }

    if(topic)
    {
        free(topic);
    }
    free(data);
}

mqtt_settings g_settings = { 
    .host = CONFIG_MQTT_BROKER_ADDRESS,
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
    .lwt_topic = CONFIG_MQTT_TOPIC_DEFAULT,
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
            ESP_LOGI(MQTT_TAG, "Starting MQTT");
            mqtt_start(&g_settings);
            //init app here
            break;
        case SYSTEM_EVENT_STA_DISCONNECTED:
            /* This is a workaround as ESP32 WiFi libs don't currently
               auto-reassociate. */
            esp_wifi_connect();
            ESP_LOGI(MQTT_TAG, "Stopping MQTT");
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

void comm_init(comm_on_data_t on_data)
{
    g_on_data = on_data;
    nvs_flash_init();
    wifi_conn_init(); 
}

bool comm_send(const char* topic, const char* buff, size_t buffsz)
{ 
    if(!g_mqtt_client)
        return false;

    mqtt_publish(g_mqtt_client, topic, buff, buffsz, 0, 0);
    return true;
}

bool comm_send_string(const char* topic, const char* s)
{
    return comm_send(topic, s, strlen(s));
}



