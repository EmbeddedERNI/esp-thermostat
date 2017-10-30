/**
 *  @brief     Module to read a DHT22 sensor using an Espressif ESP32 module.
 *
 *  @file      dht22.c
 *  @author    Hernan Bartoletti - hernan.bartoletti@gmail.com
 *  @copyright MIT License
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "soc/timer_group_struct.h"
#include "driver/periph_ctrl.h"
#include "driver/timer.h"
#include "rom/ets_sys.h"

#include "dht22.h"


#define DHT22_PIN                   GPIO_NUM_21
#define DHT22_SIGNAL_INTERVAL_MAX   0x80
#define DHT22_TIMER_GROUP           TIMER_GROUP_0
#define DHT22_TIMER                 TIMER_0

static const gpio_config_t g_gpio_config[] = {
    { 1<<DHT22_PIN, GPIO_MODE_INPUT, GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_ANYEDGE }
,   { 0 }
};

static timer_config_t g_timer_config = {
    .alarm_en       = false
,   .counter_en     = false
,   .intr_type      = TIMER_ALARM_DIS
,   .counter_dir    = TIMER_COUNT_UP
,   .auto_reload    = false
,   .divider        = 8    // 1 us
};

typedef struct
{
    uint16_t index;
    bool     level;
    uint64_t time;
} dht22_signal_interval_t;

typedef struct
{
    uint8_t  raw[5];
    int16_t  temperature;
    uint16_t humidity;
} dht22_value_t;

typedef enum
{
    dht22_idle
,   dht22_reading
,   dht22_done
,   dht22_error
} dht22_state_t;

static SemaphoreHandle_t g_semaphore   = NULL;
static QueueHandle_t     g_queue       = NULL;
static dht22_state_t     g_dht22_state = dht22_idle;

static void IRAM_ATTR dht22_isr_handler(void* arg)
{
    static uint16_t cnt = 0;
    dht22_signal_interval_t interval = { 0 };

    if(dht22_idle==g_dht22_state)
    {
        g_dht22_state = dht22_reading;
        cnt = 0;
    }

    interval.index = cnt;
    interval.level = gpio_get_level(DHT22_PIN);
    if(ESP_OK!=timer_get_counter_value(DHT22_TIMER_GROUP, DHT22_TIMER, &interval.time))
    {
        g_dht22_state = dht22_error;
    }

    if(dht22_reading==g_dht22_state)
    {
        if(pdTRUE!=xQueueSendFromISR(g_queue, &interval, NULL))
        {
            g_dht22_state = dht22_error;
        }
    }

    if(83==++cnt)
    {
        g_dht22_state = dht22_done;
        if(pdTRUE != xSemaphoreGiveFromISR(g_semaphore, NULL))
        {   // Handle the error
            g_dht22_state = dht22_error;
        }
    }
}

bool read(dht22_value_t* value)
{
    const  TickType_t    wait_for = 100 / portTICK_PERIOD_MS;
    const  TickType_t    min_interval_between_readings = 2000 / portTICK_PERIOD_MS;
    static TickType_t    last_read = 0;
    static dht22_value_t last_value = { 0 };
    TickType_t           now = xTaskGetTickCount();

    if(!value)
    {
//        printf("dht22_read error: invalid arguments!\n");
        return false;
    }

    if(last_read && now>last_read && (now-last_read)<min_interval_between_readings)
    {
        *value = last_value;
        return true;
    }

    // Clear the queue
    if(pdPASS!=xQueueReset(g_queue))
    {
//        printf("dht22_read error: cannot reset the queue!\n");
        return false;
    }

    // Re-init the timer
    if(ESP_OK!=timer_pause(DHT22_TIMER_GROUP, DHT22_TIMER))
    {
//        printf("dht22_read error: cannot reinit the timer!\n");
        return false;
    }

    if(ESP_OK!=timer_set_counter_value(DHT22_TIMER_GROUP, DHT22_TIMER, 0))
    {
//        printf("dht22_read error: cannot reinit the timer!\n");
        return false;
    }


    if(ESP_OK!=gpio_set_direction( DHT22_PIN, GPIO_MODE_OUTPUT ))
    {
//        printf("dht22_read error: gpio_set_direction as output fail!\n");
        return false;
    }

    // Pulse the signal
    if(ESP_OK!=gpio_set_level(DHT22_PIN, 0))
    {
//        printf("dht22_read error: gpio_set_level fail!\n");
        return false;
    }

    ets_delay_us(3000);

    if(ESP_OK!=gpio_set_level(DHT22_PIN, 1))
    {
//        printf("dht22_read error: gpio_set_level fail!\n");
        return false;
    }

    ets_delay_us( 25 );

    if(ESP_OK!=gpio_set_direction( DHT22_PIN, GPIO_MODE_INPUT ))
    {
//        printf("dht22_read error: gpio_set_direction as output fail!\n");
        return false;
    }

    last_read = now;

    if(ESP_OK!=timer_start(DHT22_TIMER_GROUP, DHT22_TIMER))
    {
//        printf("dht22_read error: cannot start the timer!\n");
        return false;
    }

    // Reset dht22_state
    g_dht22_state = dht22_idle;

    // Enable interrupts
    if(ESP_OK!=gpio_intr_enable(DHT22_PIN))
    {
//        printf("dht22_read error: cannot enable gpio interrupts!\n");
        return false;
    }

    // Wait for sensor response
    if(pdTRUE==xSemaphoreTake(g_semaphore, wait_for))
    {
        uint64_t last = -1;
        uint8_t v[5] = { 0 };

        // Enable interrupts
        if(ESP_OK!=gpio_intr_enable(DHT22_PIN))
        {
//            printf("dht22_read error: cannot enable gpio interrupts!\n");
            return false;
        }

        for(;;)
        {
            dht22_signal_interval_t interval = { 0 };

            if(pdTRUE == xQueueReceive(g_queue, &interval, 0))
            {
                uint64_t delta = (last==-1) ? 0 : (interval.time-last);
                uint8_t* p;

#if 0
                printf("dht22_read interval:");
                printf("  index = %04u", interval.index);
                printf("  level = %02u", (int)interval.level);
                printf("  time = 0x%08X%08X", (uint32_t)(interval.time>>32), (uint32_t)(interval.time));
                printf("  delta = %llu", delta );
                printf("\n");
#endif                
                last = interval.time;

                if (interval.index >2 && (interval.index%2)==1)
                {
                    int i = (interval.index-3)/2;

                    p = v+i/8;
                    *p <<= 1;

                    if(delta>150 && delta<=400)
                    {
                    }
                    else if(delta>400 && delta<900)
                    {
                        *p |= 1;
                    }
                    else
                    {
                      printf("dht22_read error: unexpected interval time!\n");
                      break;
                    }
                }
            }
            else
            {
                uint16_t sum = v[0] + v[1] + v[2] + v[3];
                if(v[4]==(0xFF & sum))
                {
                    value->raw[0] = v[0];
                    value->raw[1] = v[1];
                    value->raw[2] = v[2];
                    value->raw[3] = v[3];
                    value->raw[4] = v[4];
                    value->humidity  = value->raw[0] << 8 | value->raw[1];
//                    value->humidity /= 10;
                    value->temperature  = (0x7F & value->raw[2]) << 8 | value->raw[3];
//                    value->temperature /= 10;
                    if(value->raw[2] & 0x80)
                        value->temperature = -value->temperature;
                }
                else
                {
                    printf("dht22_read error: invalid checksum!\n");
                    break;
                }

                printf("dht22_read value = 0x%02X%02X%02X%02X%02X\n", v[0], v[1], v[2], v[3], v[4]);
                last_value = *value;
                return true;
            }
        }
    }
    else
    {
        printf("dht22_read error: waiting too much for a response!\n");
    }

    // Enable interrupts
    if(ESP_OK!=gpio_intr_disable(DHT22_PIN))
    {
//        printf("dht22_read error: cannot enable gpio interrupts!\n");
    }

    return false;
}


void dht22_init(void)
{
    int i;

    //create a semaphore to signal when the read is done
    g_semaphore = xSemaphoreCreateBinary();

    //create a queue to store signals intervals between edges
    g_queue = xQueueCreate(DHT22_SIGNAL_INTERVAL_MAX, sizeof(dht22_signal_interval_t));

    if(ESP_OK!=timer_init(DHT22_TIMER_GROUP, DHT22_TIMER, &g_timer_config))
    {   // Handle Error!
//        printf("ERROR during timer_init!\n");
    }


    for(i=0; g_gpio_config[i].pin_bit_mask; ++i)
    {
        if(ESP_OK!=gpio_config(&g_gpio_config[i]))
        {
//            printf("ERROR during gpio_config for 0x%016llX mask!\n", g_gpio_config[i].pin_bit_mask);
        }
    }

    //install gpio isr service
    gpio_install_isr_service(0);

    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(DHT22_PIN, dht22_isr_handler, (void*) DHT22_PIN);
}

bool dht22_read(uint16_t* humidity, int16_t* temperature)
{
    dht22_value_t value;

    if(!read(&value))
        return false;

    if(humidity) *humidity = value.humidity;
    if(temperature) *temperature = value.temperature;

    return true;
}

