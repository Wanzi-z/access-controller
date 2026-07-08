# Access Controller — Cloud Tunnel & Device Manager Integration

## Overview

The Access Controller communicates with the outside world through a reverse SSH tunnel
infrastructure that links the local network to `open-automation.org`. This document
describes exactly how the tunnelling works, what URLs are available, and how devices
register themselves with Device Manager.

---

## Architecture

```
                         Cloudflare (SSL termination)
                               │
                    open-automation.org
                    104.21.52.32 / 172.67.194.160
                               │
                               ▼
                    142.93.57.114 (DigitalOcean VPS)
                     hostname: open-automation
                     also: sales.pyfi.org
                               │
                    ┌──────────┼──────────┐
                    │ nginx (80/443)      │
                    │                      │
                    │ open-automation.org  │
                    │ → 127.0.0.1:3035    │
                    └──────────┬──────────┘
                               │
                    ┌──────────▼──────────┐
                    │ Reverse SSH Tunnel  │
                    │ autossh service     │
                    │ :3035 → :8102       │
                    └──────────┬──────────┘
                               │
                    ┌──────────▼──────────┐
                    │  192.168.1.40       │
                    │  hostname: sonic    │
                    │                      │
                    │  device-manager      │
                    │  Docker container    │
                    │  port 8102           │
                    │  (API + UI)          │
                    └─────────────────────┘
```

### How the reverse tunnel works

The open-automation server (142.93.57.114) hosts multiple websites via nginx.
Each site proxies to a local port that is actually a reverse SSH tunnel endpoint.
The tunnel is maintained by `autossh`, which keeps an SSH connection alive from
the local machine (sonic, 192.168.1.40) to the VPS.

**Tunnel mapping for device-manager:**

| Remote (VPS) | Local (sonic) | Service |
|---|---|---|
| `127.0.0.1:3035` | `localhost:8102` | Device Manager |

**Systemd service:** `device-manager-tunnel.service`
```ini
# /etc/systemd/system/device-manager-tunnel.service
Environment="LOCAL_HTTP_PORT=8102"
Environment="REMOTE_HTTP_PORT=3035"
ExecStart=/usr/bin/autossh -M 0 \
  -i /home/andy/.ssh/id_ed25519 \
  -R 127.0.0.1:${REMOTE_HTTP_PORT}:localhost:${LOCAL_HTTP_PORT} \
  andy@142.93.57.114
```

### Other tunnels on the same server (for reference)

| Domain | Remote Port | Local Port | Service |
|---|---|---|---|
| `gobotgo.ai` | 3033 | 4174 | GoBotGo |
| `vivonics.ai` | 3032 | 8094 | Vivonics |
| `needl.market` | 3031 / 3030 | 3001 / 3002 | Needl dev/prod |
| `city.radio-mapper.org` | 3034 | — | City Radio Mapper |
| `radio-mapper.org` | 4000-4002 | 7000, 4000, 8081 | Radio Mapper |

---

## URLs & Endpoints

**Security note:** The unauthenticated public surface is intentionally narrow.
Device punch-in is exposed publicly, while browser/UI access to `/devices/` is
protected with Basic auth. The full device-manager dashboard remains available
on the internal network at `http://192.168.1.40:8102/`.

### Exposed endpoints (public, HTTPS)

| Method | URL | Description |
|---|---|---|
| `POST` | `/api/devices/punch` | **Device self-registration** — ESP32 / IoT devices punch in here |
| `POST` | `/devices` | Alias for device self-registration; this is the URL the controller uses |
| `GET` | `/api/health` | Health check — returns `{"ok":true,"service":"device-manager"}` |

### Protected or blocked paths

- `GET /devices/` returns `401 Basic realm="Device Manager"` unless credentials
  are supplied.
- `/` must not expose the full UI dashboard to unauthenticated public traffic.
- Device inventory, individual details, agent management, OTA, and controller
  proxy/control routes are private surfaces. Use the local Device Manager
  (`http://192.168.1.40:8102/`) or authenticated public `/devices/` access.

### Internal endpoints (LAN only, not exposed)

These are available at `http://192.168.1.40:8102/` on the local network:

| Method | URL | Description |
|---|---|---|
| `GET` | `/api/devices` | List all discovered devices (currently 52+) |
| `GET` | `/api/devices/{device_id}` | Full detail for a single device including history |
| `GET` | `/api/agents` | List scanning agents (network, WiFi, BLE, Zigbee, etc.) |
| `GET` | `/api/agent/status` | Agent health and status |
| `GET` | `/api/agent/state` | Full device inventory + stats |
| `POST` | `/api/devices/{device_id}/actions/{action_id}` | Execute a device action (lock, toggle, reboot, etc.) |

### Device Punch-In (`POST /devices` or `POST /api/devices/punch`)

These are the endpoints that IoT devices, including the Access Controller, hit
to register their presence. The controller firmware is configured for
`https://open-automation.org/devices`. When a device POSTs, it immediately
appears in Device Manager as a recently seen device.

**Request:**
```http
POST /devices HTTP/1.1
Host: open-automation.org
Content-Type: application/json

{
  "name": "Access Controller",
  "type": "access_controller",
  "ip": "192.168.1.131",
  "mac": "AA:BB:CC:DD:EE:FF",
  "vendor": "ESP32",
  "model": "AC-Pro",
  "location": "Front Gate",
  "version": "2.1.0",
  "capabilities": ["access_control", "wiegand", "rfid", "relay"],
  "telemetry": {
    "uptime_sec": 3600,
    "wifi_rssi": -45,
    "free_heap": 128000
  },
  "metadata": {
    "firmware_build": "abc123",
    "sdk_version": "5.2"
  }
}
```

**All accepted fields:**

| Field | Type | Required | Description |
|---|---|---|---|
| `name` | string | recommended | Human-readable device name |
| `id` | string | no | Unique device identifier (defaults to `name`) |
| `type` | string | no | Device type (e.g. `access_controller`, `iot_sensor`) |
| `ip` | string | no | Device IP address (auto-detected from request if omitted) |
| `mac` | string | no | MAC address |
| `hostname` | string | no | Network hostname |
| `vendor` | string | no | Manufacturer |
| `model` | string | no | Model identifier |
| `location` | string | no | Physical location description |
| `version` | string | no | Firmware/software version |
| `capabilities` | string[] | no | Capability tags (e.g. `["access_control","wiegand"]`) |
| `telemetry` | object | no | Arbitrary sensor/state key-value pairs |
| `metadata` | object | no | Additional structured metadata |

**Response:**
```json
{
  "ok": true,
  "device_id": "Access Controller",
  "name": "Access Controller",
  "registered": true,
  "message": "Device 'Access Controller' punched in at 2026-07-07T01:23:08Z"
}
```

**What happens after a punch:**
1. Request arrives at Cloudflare → forwarded to open-automation server
2. nginx proxies to the reverse SSH tunnel (127.0.0.1:3035)
3. Tunnel delivers to device-manager container on sonic (localhost:8102)
4. device-manager writes the device record to `/data/discovery/punched.json`
5. On the next `/api/devices` call, the punched device merges into the inventory
6. The device appears in the Device Overview UI as "online" with "Seen: just now"

**Persistence:** Punched devices survive container restarts. Devices that haven't
punched in for 24 hours are automatically pruned.

**curl example:**
```bash
curl -X POST https://open-automation.org/api/devices/punch \
  -H "Content-Type: application/json" \
  -d '{"name":"Access Controller","type":"access_controller","vendor":"ESP32"}'
curl -X POST https://open-automation.org/devices \
  -H "Content-Type: application/json" \
  -d '{"name":"Access Controller","type":"access_controller","vendor":"ESP32"}'
```

---

## How the Access Controller connects

The Access Controller firmware punches into Device Manager with an HTTPS POST.
Device Manager then controls the controller over the LAN-reachable STA IP or
through its private proxy path. The ESP32-side raw TCP tunnel is experimental
and should stay disabled for normal deploy/test work unless specifically being
debugged.

### Firmware punch behavior

The ESP32 firmware posts to:

```
https://open-automation.org/devices
```

The HTTPS client uses the ESP-IDF certificate bundle, so publicly signed
certificates for `open-automation.org` are trusted without pinning a single
leaf certificate. The JSON body should include at minimum:
```json
{
  "name": "Access Controller",
  "type": "access_controller",
  "vendor": "ESP32-S3",
  "version": "<firmware_version>",
  "ip": "<local_ip>",
  "telemetry": {
    "uptime_sec": <uptime>,
    "wifi_rssi": <rssi>,
    "free_heap": <heap>
  }
}
```

The `esp_http_client` library in ESP-IDF supports HTTPS. Use the existing
`server_ip` / `server_port` config values (stored in NVS) as the host.
Since open-automation.org is now behind Cloudflare with a valid SSL cert,
the ESP32's TLS stack will verify the certificate normally.

### ESP32 HTTP client snippet (conceptual)

```c
#include "esp_http_client.h"

void punch_device_heartbeat(void) {
    char url[256];
    snprintf(url, sizeof(url), "https://open-automation.org/devices");

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "name", "Access Controller");
    cJSON_AddStringToObject(body, "type", "access_controller");
    cJSON_AddStringToObject(body, "ip", local_ip_string);
    // ... add telemetry

    char *json_str = cJSON_PrintUnformatted(body);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_str, strlen(json_str));
    esp_http_client_perform(client);
    esp_http_client_cleanup(client);

    cJSON_Delete(body);
    free(json_str);
}
```

---

## Deploy and Test

For the full controller programming, Wi-Fi/AP recovery, Device Manager proxy,
and OTA validation flow, see `docs/CONTROLLER_DEPLOY_AND_TEST.md`.

Minimum live checks:

```bash
curl -sS -i -X POST https://open-automation.org/devices \
  -H 'Content-Type: application/json' \
  -d '{"id":"route-smoke","name":"Route Smoke","type":"access_controller"}'
curl -sS -i https://open-automation.org/devices/ | sed -n '1,12p'
curl -sf http://192.168.1.40:8102/api/health
```

Expected: public punch returns `200`, public UI returns `401` without auth, and
local Device Manager health returns `{"ok":true}`.

## SSL Certificate

`open-automation.org` is behind Cloudflare and presents a publicly trusted TLS
chain. The controller must rely on the ESP-IDF certificate bundle rather than a
single pinned certificate so normal Cloudflare/Google Trust Services rotations
continue to work.

---

## DNS

open-automation.org is managed through Cloudflare:

- **Nameservers:** `candy.ns.cloudflare.com`, `lennon.ns.cloudflare.com`
- **A record:** `@` → `142.93.57.114` (proxied, orange cloud)
- **CNAME:** `www` → `open-automation.org` (proxied)
- **SSL/TLS mode:** Full (origin has valid cert)

Cloudflare API token for programmatic DNS management:
- Stored at: `~/.config/cloudflare-api-token` (chmod 600)
- Zone ID: `a858aa721bbdbc1cbc11f19923d67c06`

---

## Troubleshooting

### "HTTPS returns wrong site"
Check that the nginx config on open-automation has the correct `server_name`:
```bash
ssh open-automation
sudo nginx -T | grep -A5 "open-automation.org"
sudo systemctl reload nginx
```

### "Tunnel not responding"
Check the autossh service on sonic:
```bash
systemctl status device-manager-tunnel
# Restart if needed:
sudo systemctl restart device-manager-tunnel
```

### "Device punched but not showing in overview"
1. Check the punched file was written:
   ```bash
   cat /home/andy/projects/device-manager/data/discovery/punched.json
   ```
2. Force a device list refresh from the internal Device Manager network:
   ```bash
   curl http://192.168.1.40:8102/api/devices | jq '.devices[] | select(.source | contains("punched"))'
   ```
3. Restart the device-manager container:
   ```bash
   docker restart device-manager
   ```

### "certificate or TLS failed"

The controller trusts the public chain through the ESP-IDF certificate bundle.
If TLS fails, first verify the Cloudflare/public chain from a normal client and
then confirm the firmware was built with the cert bundle enabled. If an origin
certbot certificate is still in use on the VPS, standard renewal is:
```bash
ssh open-automation
sudo certbot renew --dry-run   # test first
sudo certbot renew             # actual renewal
sudo systemctl reload nginx
```
