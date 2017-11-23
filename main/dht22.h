/**
 *  @brief     Proof of concept of a simple thermostat using a ESP32 module and a DHT22 sensor.
 *
 *  @file      dht22.c
 *  @author    Hernan Bartoletti - hernan.bartoletti@gmail.com
 *  @copyright MIT License
 */
#include <stdint.h>

void dht22_init(void);
bool dht22_read(uint16_t* humidity, int16_t* temperature);

