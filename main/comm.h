#include <string.h>

typedef void (*comm_on_data_t)(const char* topic, const char* msg);

void comm_init(comm_on_data_t on_data);
bool comm_send(const char* topic, const char* buff, size_t buffsz);
bool comm_send_string(const char* topic, const char* s);

