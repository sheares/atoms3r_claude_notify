#pragma once
#include <stdbool.h>
#include <stddef.h>

// Connect to WiFi using credentials in secrets.h.
// Blocks up to 20 s. On success writes the IP string and returns true.
bool wifi_connect(char *ip_buf, size_t ip_len);
