#!/usr/bin/env python3
"""Discover an Access Controller on the LAN and upload an ESP32 app binary."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import ipaddress
import json
from pathlib import Path
import socket
import sys
import time
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import urljoin
from urllib.request import Request, urlopen


DEFAULT_BINARY = Path(__file__).resolve().parents[1] / "build" / "controller.bin"
DEFAULT_TIMEOUT = 1.5
OTA_TIMEOUT = 180


def http_json(url: str, timeout: float = DEFAULT_TIMEOUT) -> dict[str, Any] | None:
    try:
        request = Request(url, headers={"Accept": "application/json", "User-Agent": "access-controller-ota/1.0"})
        with urlopen(request, timeout=timeout) as response:
            content_type = response.headers.get("content-type", "").lower()
            if "json" not in content_type:
                return None
            payload = json.loads(response.read().decode("utf-8"))
            return payload if isinstance(payload, dict) else None
    except (HTTPError, URLError, TimeoutError, OSError, json.JSONDecodeError):
        return None


def normalize_base(host: str) -> str:
    value = host.strip()
    if not value:
        raise ValueError("host is empty")
    if not value.startswith(("http://", "https://")):
        value = f"http://{value}"
    return value.rstrip("/") + "/"


def is_access_controller(payload: dict[str, Any] | None) -> bool:
    if not isinstance(payload, dict):
        return False
    if payload.get("service") == "access-controller" or payload.get("deviceKind") == "access_controller":
        return True
    device = payload.get("device") if isinstance(payload.get("device"), dict) else {}
    system = payload.get("system") if isinstance(payload.get("system"), dict) else {}
    firmware = system.get("firmware") if isinstance(system.get("firmware"), dict) else {}
    return bool(
        device.get("uuid")
        and firmware.get("runningPartition")
        and all(isinstance(payload.get(key), list) for key in ("locks", "exits", "fobs", "keypads", "motions"))
    )


def state_from_payload(base_url: str, payload: dict[str, Any]) -> dict[str, Any] | None:
    if "locks" in payload and "system" in payload:
        return payload
    api = payload.get("api") if isinstance(payload.get("api"), dict) else {}
    state_path = str(api.get("state") or "/api/state")
    return http_json(urljoin(base_url, state_path), timeout=DEFAULT_TIMEOUT)


def probe(base_url: str) -> dict[str, Any] | None:
    endpoints = (
        ".well-known/access-controller.json",
        "api/discovery",
        "api/state",
    )
    for endpoint in endpoints:
        payload = http_json(urljoin(base_url, endpoint), timeout=DEFAULT_TIMEOUT)
        if not is_access_controller(payload):
            continue
        state = state_from_payload(base_url, payload or {})
        if not is_access_controller(state):
            state = payload
        return {"base_url": base_url, "discovery": payload, "state": state}
    return None


def local_ipv4_networks() -> list[str]:
    networks: set[str] = set()
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.connect(("8.8.8.8", 80))
            ip = sock.getsockname()[0]
            networks.add(f"{'.'.join(ip.split('.')[:3])}.0/24")
    except OSError:
        pass
    networks.add("192.168.4.0/24")
    return sorted(networks)


def host_candidates(networks: list[str], extra_hosts: list[str]) -> list[str]:
    hosts: list[str] = []
    for host in extra_hosts:
        if host.strip():
            hosts.append(normalize_base(host))
    for network_text in networks:
        network = ipaddress.ip_network(network_text, strict=False)
        if network.version != 4 or network.num_addresses > 1024:
            raise ValueError(f"Refusing to scan oversized network: {network_text}")
        for ip in network.hosts():
            hosts.append(normalize_base(str(ip)))
    return list(dict.fromkeys(hosts))


def discover(args: argparse.Namespace) -> list[dict[str, Any]]:
    if args.host:
        result = probe(normalize_base(args.host))
        return [result] if result else []

    extra = []
    if args.extra_host:
        extra.extend(args.extra_host)
    networks = args.network or local_ipv4_networks()
    candidates = host_candidates(networks, extra)
    found: list[dict[str, Any]] = []

    with ThreadPoolExecutor(max_workers=max(1, args.workers)) as executor:
        futures = {executor.submit(probe, base_url): base_url for base_url in candidates}
        for future in as_completed(futures):
            result = future.result()
            if result:
                found.append(result)

    return sorted(found, key=lambda item: item["base_url"])


def first_present(*values: Any) -> Any:
    for value in values:
        if value not in (None, "", []):
            return value
    return None


def device_summary(device: dict[str, Any]) -> dict[str, str]:
    state = device.get("state") if isinstance(device.get("state"), dict) else {}
    discovery = device.get("discovery") if isinstance(device.get("discovery"), dict) else {}
    state_device = state.get("device") if isinstance(state.get("device"), dict) else {}
    discovery_device = discovery.get("device") if isinstance(discovery.get("device"), dict) else {}
    network = first_present(state_device.get("network"), discovery_device.get("network"), {}) or {}
    system = state.get("system") if isinstance(state.get("system"), dict) else {}
    firmware = system.get("firmware") if isinstance(system.get("firmware"), dict) else {}
    running = firmware.get("runningPartition") if isinstance(firmware.get("runningPartition"), dict) else {}
    next_slot = firmware.get("nextUpdatePartition") if isinstance(firmware.get("nextUpdatePartition"), dict) else {}

    return {
        "url": device["base_url"].rstrip("/"),
        "uuid": str(first_present(state_device.get("uuid"), discovery_device.get("uuid"), "unknown")),
        "mac": str(first_present(network.get("wifi_sta_mac"), network.get("wifi_ap_mac"), network.get("eth_mac"), "unknown")),
        "ip": str(first_present(network.get("wifi_sta_ip"), network.get("eth_ip"), network.get("wifi_ap_ip"), "unknown")),
        "version": str(first_present(firmware.get("projectVersion"), firmware.get("gitCommit"), "unknown")),
        "branch": str(first_present(firmware.get("gitBranch"), "unknown")),
        "commit": str(first_present(firmware.get("gitCommit"), "unknown"))[:12],
        "running": str(first_present(running.get("label"), "unknown")),
        "next": str(first_present(next_slot.get("label"), "unknown")),
        "ota_state": str(first_present(firmware.get("otaState"), "unknown")),
        "max_upload": str(first_present(firmware.get("maxUploadBytes"), "unknown")),
    }


def print_devices(devices: list[dict[str, Any]]) -> None:
    for index, device in enumerate(devices, start=1):
        summary = device_summary(device)
        print(f"[{index}] {summary['url']}")
        print(f"    UUID: {summary['uuid']}")
        print(f"    IP/MAC: {summary['ip']} / {summary['mac']}")
        print(f"    Firmware: {summary['branch']}@{summary['commit']} version={summary['version']}")
        print(f"    OTA: running={summary['running']} next={summary['next']} state={summary['ota_state']} max={summary['max_upload']} bytes")


def choose_device(devices: list[dict[str, Any]], yes: bool) -> dict[str, Any]:
    if not devices:
        raise RuntimeError("No Access Controller devices were found")
    if len(devices) == 1 or yes:
        return devices[0]
    while True:
        value = input(f"Upload to which device? [1-{len(devices)}] ").strip()
        try:
            index = int(value)
        except ValueError:
            continue
        if 1 <= index <= len(devices):
            return devices[index - 1]


def upload_firmware(device: dict[str, Any], binary: Path) -> dict[str, Any]:
    data = binary.read_bytes()
    url = urljoin(device["base_url"], "api/ota/upload")
    request = Request(
        url,
        data=data,
        method="POST",
        headers={
            "Content-Type": "application/octet-stream",
            "Content-Length": str(len(data)),
            "X-Firmware-Filename": binary.name,
            "User-Agent": "access-controller-ota/1.0",
        },
    )
    with urlopen(request, timeout=OTA_TIMEOUT) as response:
        payload = response.read().decode("utf-8")
        return json.loads(payload) if payload else {}


def wait_for_reboot(device: dict[str, Any], previous_running: str | None) -> dict[str, Any] | None:
    time.sleep(3.0)
    for _ in range(60):
        state = http_json(urljoin(device["base_url"], "api/state"), timeout=2.0)
        if is_access_controller(state):
            firmware = (state.get("system") or {}).get("firmware") or {}
            running = (firmware.get("runningPartition") or {}).get("label")
            if running and running != previous_running:
                return state
            uptime = ((state.get("system") or {}).get("uptimeSeconds"))
            if isinstance(uptime, (int, float)) and uptime < 120:
                return state
        time.sleep(2.0)
    return None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Find an Access Controller and OTA-upload an ESP32 app binary.")
    parser.add_argument("--host", help="Controller host or URL. Skips LAN scan when provided.")
    parser.add_argument("--network", action="append", help="IPv4 CIDR to scan, for example 192.168.1.0/24. May be repeated.")
    parser.add_argument("--extra-host", action="append", help="Additional host/IP to probe before scanned networks.")
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY, help=f"Firmware .bin path. Default: {DEFAULT_BINARY}")
    parser.add_argument("--yes", "-y", action="store_true", help="Do not prompt before upload.")
    parser.add_argument("--workers", type=int, default=96, help="Concurrent scan workers.")
    args = parser.parse_args(argv)

    binary = args.binary.expanduser().resolve()
    if not binary.exists():
        print(f"Firmware binary not found: {binary}", file=sys.stderr)
        return 2

    print("Finding Access Controller devices...")
    devices = discover(args)
    print_devices(devices)
    device = choose_device(devices, args.yes)
    summary = device_summary(device)
    size = binary.stat().st_size

    if not args.yes:
        answer = input(f"Upload {binary} ({size} bytes) to {summary['uuid']} at {summary['url']}? [y/N] ").strip().lower()
        if answer not in {"y", "yes"}:
            print("Cancelled.")
            return 1

    previous_running = summary.get("running")
    print(f"Uploading {size} bytes to {summary['url']}...")
    result = upload_firmware(device, binary)
    print(json.dumps(result, indent=2, sort_keys=True))

    print("Waiting for device to reboot and answer /api/state...")
    final_state = wait_for_reboot(device, previous_running)
    if not final_state:
        print("Upload returned OK, but the device did not come back before the timeout.", file=sys.stderr)
        return 3

    final_device = {"base_url": device["base_url"], "state": final_state, "discovery": device.get("discovery")}
    final_summary = device_summary(final_device)
    print("Device is back online.")
    print(f"    Firmware: {final_summary['branch']}@{final_summary['commit']} version={final_summary['version']}")
    print(f"    OTA: running={final_summary['running']} next={final_summary['next']} state={final_summary['ota_state']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
