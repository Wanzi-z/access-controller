# Access Controller Documentation

This folder documents the current shape of the access-controller project as of
July 6, 2026. It is based on the checked-out repository contents, including the
ESP32 firmware, the Node.js tunnel server, KiCad hardware projects,
manufacturing outputs, enclosure models, images, and existing planning notes.

The project is a commercial-style access controller for electric strikes and
magnetic locks. The main device idea is a networked ESP32-S3 controller that can
manage two lock channels, read Wiegand RFID/keypad devices, accept physical
inputs such as exit buttons, fobs, and motion, expose a local web UI, store
credential/configuration state, and optionally connect outward to a tunnel
gateway so the device UI can be reached through a server.

## Documentation Map

- [Project Overview](PROJECT.md): device concept, major subsystems, and how the
  pieces fit together.
- [Repository Map](REPOSITORY_MAP.md): what each top-level folder and important
  subfolder is for.
- [Software](SOFTWARE.md): firmware apps, web UI, APIs, tests, tunnel server,
  and controller mini experiment.
- [Network Provisioning](NETWORK_PROVISIONING.md): Wi-Fi onboarding, server
  reachability policy, AP fallback, and retry recovery diagrams.
- [Controller Deploy and Test](CONTROLLER_DEPLOY_AND_TEST.md): complete
  programming, AP recovery, dual-network switching, Device Manager/proxy, OTA,
  and automated/UI verification runbook.
- [Hardware](HARDWARE.md): KiCad projects, board sheets, manufacturing files,
  connectors, enclosure models, and images.
- [Power and Energy Harvesting](POWER_AND_ENERGY_HARVESTING.md): design
  investigation (not implemented) for a battery-powered or energy-harvesting
  variant — WiFi power-save/latency architecture, 18650 battery sizing for a
  1-year single charge, doorknob-twist energy harvesting as a battery-dead
  fallback, and ESP-NOW vs. Zigbee radio tradeoffs.
- [Workflows](WORKFLOWS.md): build, flash, test, tunnel, hardware review, and
  documentation maintenance commands.
- [Modernization Plan](PLAN.md): existing implementation plan for the Wiegand,
  keypad, and UI modernization work.

## Current Project Snapshot

The repository is organized around these major areas:

- `code/controller`: the active ESP32-S3 access-controller firmware and embedded
  web UI.
- `code/controller_mini`: an alternate/experimental ESP32-S3 firmware layout
  with service-manager, radar, OTA, and network-manager modules.
- `code/tunnel`: a Node.js reverse HTTP tunnel server and mock ESP32 client.
- `circuits/controller`: the main controller KiCad design, generated PCB data,
  gerbers, BOM/placement files, symbols, footprints, and an adapter board.
- `circuits/strike`: a smaller strike-controller KiCad design with its own
  gerbers and manufacturing exports.
- `model`: enclosure and mechanical artifacts, including current controller
  STL/3MF/Blender/G-code files and older reference models.
- `images`: rendered board/schematic images and a PoE controller PDF reference.
- `docs`: this documentation set plus the existing modernization plan.

## Notes About Source State

There are active uncommitted source changes in this checkout outside `docs`.
This documentation describes the current worktree rather than only committed
history. When hardware, firmware, or UI behavior changes, update these docs in
the same change so the folder remains useful as the project map.
