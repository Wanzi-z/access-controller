# Repository Map

## Top-Level Layout

| Path | Purpose |
| --- | --- |
| `README.md` | Original high-level project overview and older ESP-IDF/lws-factory installation notes. |
| `code/` | Firmware, tunnel server, tests, and device-facing application code. |
| `circuits/` | KiCad hardware projects, board layouts, schematics, gerbers, BOMs, and placement files. |
| `model/` | 3D enclosure, plate, cover, old keypad/NFC/wallplate models, and print G-code. |
| `images/` | Board/schematic renderings and hardware reference PDFs. |
| `docs/` | Current documentation and planning notes. |
| `.vscode/` | Local/editor configuration folder. |
| `.agents/` | Local agent/tooling state folder present in the worktree; not part of firmware or hardware deliverables. |
| `.deepseek/` | Local agent/tooling state. Currently contains `state/subagents.v1.json`; not part of the access-controller product. |

## `code`

| Path | Purpose |
| --- | --- |
| `code/.vscode` | Editor configuration for the code workspace. |
| `code/controller` | Active ESP32-S3 controller firmware. This is the main application. |
| `code/controller/main` | Firmware entrypoint, service modules, Kconfig, CMake component file, and embedded web UI. |
| `code/controller/main/services` | Door-control services, storage, APIs, web/tunnel servers, Wiegand/RF registries, drivers. |
| `code/controller/main/public` | Embedded single-page device UI served by the ESP32. |
| `code/controller/tests` | Node.js test suite for live device API, UI, stress, and physical hardware walkthroughs. |
| `code/controller_mini` | Alternate/experimental controller firmware with service manager, radar support, and refactored managers. |
| `code/tunnel` | Node.js reverse HTTP tunnel gateway, Docker/systemd deployment, mock ESP32 client, and smoke test. |

Important firmware files in `code/controller`:

- `CMakeLists.txt`: ESP-IDF project definition for the main firmware.
- `main/CMakeLists.txt`: builds `main.c` plus `services/log_store.c` and embeds
  `public/favicon.ico`, `public/index.html`, `public/script.js`, and
  `public/style.css`.
- `partitions.csv`: 16 MB flash map with NVS, OTA data, two 2 MB app slots, and
  an 11 MB SPIFFS partition.
- `flash_esp32s3.sh`: interactive build/flash/monitor helper with serial port
  auto-detection.
- `flash_now.sh`: direct flasher that waits for manual ESP32-S3 download mode on
  `/dev/ttyUSB0`.
- `dependencies.lock`: dependency lock artifact for the firmware environment.
- `todo.md`: short current task list.

## `circuits`

| Path | Purpose |
| --- | --- |
| `circuits/controller` | Main controller board KiCad project. |
| `circuits/controller/Manufacturer` | Main controller manufacturing exports: BOM, top placement, and gerbers. |
| `circuits/controller/adapter/Adapter-TE-52` | Adapter PCB project and gerbers. |
| `circuits/controller/Library.pretty` | Local footprint library, including RXB6 RF module footprint. |
| `circuits/controller/pics` | Hardware reference photo(s). |
| `circuits/controller/.history` | Local/history folder present in the worktree; treat as historical/tool state unless intentionally promoted. |
| `circuits/controller/access-controller-backups` | KiCad backup folder. |
| `circuits/strike` | Smaller strike-controller KiCad project. |
| `circuits/strike/Manufacturer` | Strike-controller BOM, placement, and gerbers. |
| `circuits/strike/strike-controller-backups` | KiCad backup folder. |

Generated KiCad/cache/backups files exist alongside source schematics. The
current docs treat `.kicad_sch`, `.kicad_pcb`, `.kicad_pro`, gerbers, BOMs, and
placement files as authoritative project artifacts; `.bak`, cache, rescue, and
legacy `.sch` files are historical/supporting artifacts.

## `model`

| Path | Purpose |
| --- | --- |
| `model/controller` | Current controller enclosure/plate/cover 3D source and print outputs. |
| `model/residential` | Residential model variant in Blender and STL form. |
| `model/old` | Older box, keypad, NFC, lid, wallplate, and third-party keypad case assets. |

The controller model folder contains Blender source, STL meshes, 3MF slicer
project files, and G-code outputs. The `tire_*.gcode` files are currently
untracked in git but present in the worktree.

## `images`

| Path | Purpose |
| --- | --- |
| `images/controller.png` | Board image used by the top-level README. |
| `images/controller-schematic.png` | Schematic image used by the top-level README. |
| `images/controller-3d.png` | 3D board image used by the top-level README. |
| `images/tps23754.pdf` | PoE controller reference PDF. |
| `images/Screenshot from 2023-02-12 17-47-00.png` | Historical screenshot/reference image. |

## `docs`

| Path | Purpose |
| --- | --- |
| `docs/README.md` | Documentation entrypoint. |
| `docs/PROJECT.md` | Device/project concept and system architecture. |
| `docs/REPOSITORY_MAP.md` | Repository and folder organization. |
| `docs/SOFTWARE.md` | Firmware, UI, API, tunnel, mini controller, and tests. |
| `docs/HARDWARE.md` | KiCad hardware, manufacturing, models, and images. |
| `docs/WORKFLOWS.md` | Common build, flash, test, tunnel, and hardware workflows. |
| `docs/PLAN.md` | Existing modernization implementation plan. |
