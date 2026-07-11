#ifndef IP_TIMEZONE_H
#define IP_TIMEZONE_H

#include <stdint.h>
#include <stdbool.h>

// Starts a background task (idempotent -- safe to call on every Wi-Fi connect/reconnect, it only
// ever spawns the task once) that resolves the device's UTC offset from its public IP via an
// IP-geolocation lookup. Refreshes periodically so DST transitions are picked up without a reboot,
// and retries with backoff on failure.
void ip_timezone_start(void);

// Seconds to ADD to a UTC unix timestamp to get local wall-clock time (e.g. -18000 for US Central
// Standard Time). Returns 0 (i.e. UTC) until a lookup has succeeded at least once this boot.
int32_t ip_timezone_offset_seconds(void);

// True once at least one successful IP-geolocation lookup has completed this boot.
bool ip_timezone_is_resolved(void);

#endif // IP_TIMEZONE_H
