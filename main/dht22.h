#include <stdint.h>

void dht22_init(void);
bool dht22_read(uint16_t* humidity, int16_t* temperature);

