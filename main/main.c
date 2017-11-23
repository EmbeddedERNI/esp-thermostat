/**
 *  @brief     Proof of concept of a simple thermostat using a ESP32 module and a DHT22 sensor.
 *
 *  @file      main.c
 *  @author    Hernan Bartoletti - hernan.bartoletti@gmail.com
 *  @copyright MIT License
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "driver/gpio.h"

#include "comm.h"
#include "dht22.h"

#define PIN_OUTPUT  GPIO_NUM_23

typedef enum
{
    tm_off
,   tm_auto
,   tm_heat
} thermostat_mode_t;

typedef struct
{
    int16_t             setpoint;       // in tenths of celsius degrees
    int16_t             hysteresis;     // in tenths of celsius degrees
    int16_t             temperature;    // in tenths of celsius degrees
    thermostat_mode_t   mode;           
    bool                output;

    uint16_t            humidity;       // just to report..

} thermostat_internals_t;

const char *MQTT_TAG = "THERMOSTAT";

const gpio_config_t g_gpio_config[] = {
    { 1<<PIN_OUTPUT, GPIO_MODE_OUTPUT|GPIO_MODE_INPUT, GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_DISABLE }
,   { 0 }
};

thermostat_internals_t g_thermostat_internals = {
    .setpoint = 250
,   .hysteresis = 5
,   .temperature = 250
,   .mode = tm_auto
,   .output = false 
};

bool temperature_parse(const char* s, int16_t* temperature)
{ 
    int i;

    if(!s) return false;
    if(!temperature) return false;

    i = atoi(s);
    *temperature = (int16_t)i;
    return true;
}

void send_value(char opcode, int value)
{
    char s[15] = { 0 };

    sprintf(s, "%c=%d", opcode&0xDF, value); 
    comm_send_string(CONFIG_MQTT_TOPIC_DEFAULT, s); 
}

void send_mode(void)
{
    char s[10] = { 0 };
    
    sprintf(s, "M=%s", (tm_off==g_thermostat_internals.mode ? "off" : 
                        tm_auto==g_thermostat_internals.mode ? "auto" :
                        tm_heat==g_thermostat_internals.mode ? "heat" : "err" ));
    comm_send_string(CONFIG_MQTT_TOPIC_DEFAULT, s); 
}



/**
 *      T = off
 *      ---------------- setpoint+hysteresis
 *      
 *      ================ setpoint
 *      
 *      ---------------- setpoint-hysteresis
 *      T = on
 */
void thermostat_process(thermostat_internals_t* i)
{ 
    if(i)
    {
        switch(i->mode)
        { 
            case tm_off: 
            case tm_heat: 
            {
                i->output = (tm_heat==i->mode);
            } break;
            case tm_auto:
            {
                int16_t threshold = i->setpoint + (i->output ? i->hysteresis : - i->hysteresis);
                
                i->output = (i->temperature<threshold);
            }
        }
        send_value('O', i->output ? 1 : 0); 

        if(ESP_OK!=gpio_set_level(PIN_OUTPUT, i->output))
        { 
            printf("thermostat_process error: gpio_set_level fail!\n");
        }
    } 
}

bool cmd_process(const char*buff, char opcode, int value)
{
    char s[15] = { 0 };

    if(buff && ((opcode>='a' && opcode<='z') || (opcode>='A' && opcode<='Z') ))
    {
        opcode |= 0x20;

        s[0] = opcode;

        if(0==strcmp(buff, s))
        { 
            send_value(opcode, value);
            return true;
        } 
    } 
    return false;
}

void comm_on_data(const char* topic, const char* buff)
{ 
    if(0==strcmp(topic, CONFIG_MQTT_TOPIC_DEFAULT) && buff && strlen(buff)<=10)
    {
        if(0==strncmp(buff, "s=",2))
        {
            if(temperature_parse(buff+2, &g_thermostat_internals.setpoint))
            {
                printf("New setpoint is set at %d celsius degrees\n", g_thermostat_internals.setpoint);
            }
            else
            {
                printf("ERROR trying to update the setpoint [%s]\n", buff); 
            }

            send_value('S', g_thermostat_internals.setpoint);

            thermostat_process(&g_thermostat_internals);
        }
        else if(0==strncmp(buff, "d=",2))
        {
            if(temperature_parse(buff+2, &g_thermostat_internals.hysteresis))
            {
                printf("New hysteresis is set at %d celsius degrees\n", g_thermostat_internals.hysteresis);
            }
            else
            {
                printf("ERROR trying to update the hysteresis [%s]\n", buff); 
            }

            send_value('D', g_thermostat_internals.hysteresis);

            thermostat_process(&g_thermostat_internals);
        }
        else if(0==strcmp(buff, "m=auto"))
        { 
            g_thermostat_internals.mode = tm_auto;

            thermostat_process(&g_thermostat_internals);
            send_mode();
        }
        else if(0==strcmp(buff, "m=heat"))
        { 
            g_thermostat_internals.mode = tm_heat;

            thermostat_process(&g_thermostat_internals);
            send_mode();
        }
        else if(0==strcmp(buff, "m=off"))
        { 
            g_thermostat_internals.mode = tm_off;

            thermostat_process(&g_thermostat_internals);
            send_mode();
        }
        else if(cmd_process(buff, 'o', g_thermostat_internals.output ? 1 : 0) )
        {
        }
        else if(cmd_process(buff, 't', g_thermostat_internals.temperature) )
        {
        }
        else if(cmd_process(buff, 'h', g_thermostat_internals.humidity) )
        {
        }
        else if(cmd_process(buff, 's', g_thermostat_internals.setpoint) )
        {
        }
        else if(cmd_process(buff, 'd', g_thermostat_internals.hysteresis) )
        {
        }
        else if(0==strcmp(buff, "m"))
        {
            send_mode(); 
        }
    }
}


void app_main()
{
    int i;

    ESP_LOGI(MQTT_TAG, "[APP] Startup..");
    ESP_LOGI(MQTT_TAG, "[APP] Free memory: %d bytes", system_get_free_heap_size());
    ESP_LOGI(MQTT_TAG, "[APP] SDK version: %s, Build time: %s", system_get_sdk_version(), BUID_TIME);


    for(i=0; g_gpio_config[i].pin_bit_mask; ++i)
    {
        if(ESP_OK!=gpio_config(&g_gpio_config[i]))
        {
            printf("ERROR during gpio_config for 0x%016llX mask!\n", g_gpio_config[i].pin_bit_mask);
        }
    }

    comm_init(comm_on_data);

    dht22_init();

    for(i=0; ; ++i)
    {   
        uint16_t  humidity;
        int16_t   temperature;
        bool      humidity_reported = false;
        bool      temperature_reported = false;

        if(dht22_read(&humidity, &temperature))
        {
            printf("DHT22 read successfully!\n");
            printf("  humidity = %i.%u%%\n", humidity/10, humidity%10); 
            printf("  temperature = %i.%u degrees\n", temperature/10, temperature%10); 
        } 

        if(temperature!=g_thermostat_internals.temperature)
        {
            g_thermostat_internals.temperature = temperature;
            thermostat_process(&g_thermostat_internals);

            send_value('T', g_thermostat_internals.temperature);
            temperature_reported = true;
        }

        if(g_thermostat_internals.humidity!=humidity)
        {
            g_thermostat_internals.humidity = humidity;
            send_value('H', g_thermostat_internals.humidity);
            humidity_reported = true;
        } 
        
        if((i%12)==0 && !temperature_reported)
        {
            send_value('T', g_thermostat_internals.temperature);
        }

        if((i%12)==1 && !humidity_reported)
        {
            send_value('H', g_thermostat_internals.humidity);
        }

        if((i%12)==2)
        {
            send_value('S', g_thermostat_internals.setpoint);
        }

        if((i%12)==3)
        {
            send_value('D', g_thermostat_internals.hysteresis);
        }

        if((i%12)==4)
        {
            send_value('O', g_thermostat_internals.output ? 1 : 0); 
        }

        if((i%12)==5)
        { 
            send_mode();
        }

        vTaskDelay( 5000 / portTICK_PERIOD_MS );
    } 
}

