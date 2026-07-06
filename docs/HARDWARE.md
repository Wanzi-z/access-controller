# Hardware

## Main Controller Board: `circuits/controller`

`circuits/controller` is the main access-controller KiCad project. It includes
schematics, PCB layout, generated netlist/XML, local symbols/footprints,
manufacturing outputs, and historical/cache files.

Primary project files:

- `access-controller.kicad_pro`: KiCad project.
- `access-controller.kicad_sch`: top-level schematic.
- `access-controller.kicad_pcb`: PCB layout.
- `access-controller.net` and `access-controller.xml`: generated netlist/XML.
- `sym-lib-table`, `fp-lib-table`: local symbol/footprint library maps.
- `Connector_Generic.kicad_sym`, `RXB6_RF_Module.kicad_sym`,
  `access-controller-cache.lib`, `access-controller-rescue.*`: symbol/cache
  support.
- `RF_MODULE.kicad_mod`, `RXB6_RF_MODULE.kicad_mod`,
  `Library.pretty/RXB6_RF_MODULE.kicad_mod`: RF module footprint artifacts.

### Schematic Organization

The top-level schematic instantiates these major sheets:

| Sheet | File | Purpose |
| --- | --- | --- |
| `Microcontroller` | `microcontroller.kicad_sch` | ESP32-S3-WROOM-1, USB/UART programming support, EN/PROG/FACT buttons, USB connectors, core ESP32 labels. |
| `Power` | `power.kicad_sch` | Power input/regulation, including MT2492-style regulator, AMS1117-3.3, MIC29302WU, power sense, and status LEDs. |
| `Ethernet` | `ethernet.kicad_sch` | W5500 Ethernet controller, RJ45 connector, SPI labels, link/status LEDs. |
| `Lock Control` | `control.kicad_sch` | Lock output transistor/MOSFET control circuitry. |
| `Keypad / FOB` | `quad-optocoupler.kicad_sch` | Reused quad optocoupler circuit for isolated inputs. |
| `Lock / Aux` | `quad-optocoupler.kicad_sch` | Reused optocoupler circuit for lock/auxiliary signals. |
| `Push / Motion` | `quad-optocoupler.kicad_sch` | Reused optocoupler circuit for push/motion signals. |
| `Wiegand` | `wiegand.kicad_sch` | Wiegand reader interface sheet. |

Other controller schematic files:

- `poe.kicad_sch`: PoE power design sheet.
- `poe_OLD.kicad_sch`: older PoE sheet retained for reference.
- `usb.kicad_sch`: USB/programming design sheet with an explicit warning about
  USB isolation when powered via PoE.
- `microcontroller_old.kicad_sch`: older microcontroller sheet.

### Major Components and Circuits

From the current schematics and BOMs, the controller board includes:

- ESP32-S3-WROOM-1 module
- MCP23017 I/O expander
- W5500 Ethernet controller
- RJ45 connector
- RXB6/RF receiver module footprint
- USB Mini-B connectors and USB/UART bridge circuitry
- CH340T/CP2102N USB serial circuitry in schematic variants
- EN, PROG, and FACT buttons
- MT2492-style buck regulator circuitry
- AMS1117-3.3 and MIC29302WU regulators
- TLP291-4 quad optocouplers
- AO3400A, MMBT5551, L8050/L8550 transistor/MOSFET circuits
- terminal blocks for power, Wiegand, lock, and auxiliary wiring
- buzzer/sounder part `TMB12A05`
- status LEDs and ESD/protection diodes

### Manufacturing Outputs

`circuits/controller/Manufacturer` contains:

- `access-controller.csv`: manufacturing BOM.
- `access-controller-top-pos.csv`: pick-and-place/position file.
- `gerbers/`: generated gerber and drill files.

Gerber/drill files include:

- copper: `F_Cu`, `B_Cu`, `In1_Cu`, `In2_Cu`
- paste: `F_Paste`, `B_Paste`
- mask: `F_Mask`, `B_Mask`
- silkscreen: `F_Silkscreen`, `B_Silkscreen`
- outline: `Edge_Cuts`
- drills: `PTH.drl`, `NPTH.drl`

There is also an older/root BOM file:

- `access-controller_jlcpcb_bom_feb_18_1848.csv`

### Adapter Board

`circuits/controller/adapter/Adapter-TE-52` contains a separate adapter PCB:

- `Adapter-TE-52.pro`
- `Adapter-TE-52.sch`
- `Adapter-TE-52.kicad_pcb`
- `Adapter-TE-52.net`
- `gerbers/` with copper, mask, paste, silkscreen, edge cuts, and drill files.

## Strike Controller Board: `circuits/strike`

`circuits/strike` is a smaller strike-controller KiCad project. It has both
modern `.kicad_sch` files and legacy `.sch` files.

Primary files:

- `strike-controller.kicad_pro`: KiCad project.
- `strike-controller.kicad_sch`: top-level schematic.
- `strike-controller.kicad_pcb`: PCB layout.
- `strike-controller.net` and `strike-controller.xml`: generated netlist/XML.
- `power.kicad_sch`
- `microcontroller.kicad_sch`
- `usb.kicad_sch`
- `transistor_array.kicad_sch`

Major parts from the schematic/BOM:

- ESP32-WROOM-32 module
- CH340T USB/UART bridge
- USB Mini-B connector
- MT2492 regulator
- AMS1117-3.3 regulator
- transistor array based on MMBT5551 devices
- flyback/protection diodes for driven outputs
- WS2812B LED
- PIR digital sensor footprint
- Molex PicoBlade 6-pin connectors
- mounting holes and power/status LEDs

Manufacturing outputs are in `circuits/strike/Manufacturer`:

- `strike-controller_jlcpcb_bom_jun_15_0457.csv`
- `strike-controller-top-pos.csv`
- `gerbers/` with copper, mask, silkscreen, edge cuts, and drill outputs.

## Images and References

`images` contains project visual references:

- `controller-schematic.png`: schematic image in the top-level README.
- `controller.png`: PCB image in the top-level README.
- `controller-3d.png`: 3D board render in the top-level README.
- `tps23754.pdf`: PoE controller datasheet/reference.
- `Screenshot from 2023-02-12 17-47-00.png`: historical UI or project
  screenshot.

## Enclosure and Mechanical Files: `model`

`model/controller` contains current controller enclosure artifacts:

- `controller.blend` and `controller.blend1`: Blender source/back-up files.
- `controller.stl`: controller mesh.
- `controller.3mf`: slicer/project format.
- `controller-cover.stl`, `controller-cover.3mf`: cover model.
- `controller-plate.stl`, `controller-plate.3mf`: plate model.
- `controller_0828.gcode`, `controller_1354.gcode`: print outputs.
- `controller_plate_1007.gcode`, `controller-plate_1543.gcode`: plate print
  outputs.
- `tire_0852.gcode`, `tire_0858.gcode`: present in the worktree and currently
  untracked.

`model/residential` contains a residential variant:

- `residential.blend`
- `residential.blend1`
- `residential.stl`

`model/old` contains older or reference models:

- old box/controller/keypad/NFC/wallplate/lid STL and Blender files
- `Arduino_3x4___4x4_Keypad_Case` third-party reference files, license,
  README, images, and STL

## Hardware and Firmware Alignment Notes

- Firmware currently uses ESP32-S3 GPIO4-GPIO7 for two Wiegand channels.
- Firmware currently uses GPIO13/GPIO14 for I2C and MCP23017 address `0x20`.
- Firmware currently reads 433 MHz RF data on GPIO15.
- Most lock/button/motion/fob I/O is routed through MCP23017 pins A0-B7.
- The KiCad design includes Ethernet/W5500 hardware, but the active firmware
  network path currently centers on Wi-Fi and the HTTP tunnel client.
- The USB schematic warns about using an isolated USB connection when the board
  is powered via PoE.
