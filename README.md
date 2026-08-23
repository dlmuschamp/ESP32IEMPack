# ESP32 Wireless IEM Pack

A custom-built, high-performance wireless In-Ear Monitor (IEM) system designed to drive high-fidelity audio hardware (such as the Moondrop Aria and Edge).

## Hardware Bill of Materials

Bench prototype (Arduino / 3xAA):

* **Microcontroller:** ESP32 (UI/Display logic currently being prototyped on Arduino)
* **Display:** 1.54" Full Color TFT Display Module (SPI)
* **Power Management:** TP4056 Type-C USB 5V 1A Lithium Battery Charger
* **Battery:** EEMB 3.7V 103454 2000mAh LiPo (Prototyping phase: 4.5V via 3xAA pack)
* **Wire:** 30AWG Tinned Copper Silicone

RX 1.0.0 KiCad schematic (`KiCad/RX_SKETCHES/`, still a WIP breadboard transfer):

* **Module:** ESP32-WROOM-32U (U.FL — fit a pigtail + 2.4 GHz antenna; no PCB antenna)
* **I2S (matches `main/square_wave_test.c`):** BCK = IO33, LRCLK = IO32, DIN = IO25
* **DAC:** PCM5102, I2S (`FMT` low), SCK grounded (PLL), 470 Ω + 2.2 nF on each analog out
* **Volume:** PTN09S2 dual-gang log pot with 0% switch → amp `~{SHDN}`
* **Amp:** MAX97220A (fixed +3.5 dB). Input 10 kΩ resistors are DNP on hand-built boards so gain/attenuation can be tried
* **Power:** USB-C 5 V power bank → LM3940 3.3 V. No LiPo charger on this sheet
* **Hand assembly:** 0805 passives (0603 only if you are comfortable). Do not use 0201
* **PCB file:** footprint dump / ratsnest only — re-import after schematic net changes

## Software Architecture

The firmware is written in **C** and designed with a **Hardware Abstraction Layer (HAL)**.

All hardware-specific interactions (such as SPI display commands) are wrapped in custom functions. This allows the core UI and application logic to be safely prototyped on an Arduino environment today and seamlessly ported to the target ESP32 framework later by simply swapping the low-level hardware transmission lines.

## Display SPI Pinout

The 1.54" TFT displays use the following SPI pins, actively mapped for the prototype:

* `SCL` - Serial Clock
* `SDA` - Serial Data (MOSI)
* `RES` - Reset
* `DC`  - Data/Command
* `CS`  - Chip Select
* `BLK` - Backlight
* `VCC` - Power
* `GND` - Ground

## Current Status

* [x] Initial repository and branch setup.
* [x] Component sourcing and power routing.
* [x] RX schematic transferred from breadboard into `KiCad/RX_SKETCHES` (WIP; not a finished layout).
* [ ] Prototyping 1.54" TFT UI on Arduino via SPI (3xAA power).
* [ ] Porting C codebase to ESP32.
* [ ] Floorplan / route the RX PCB after ERC is clean.

## Author

Damian Luciano Muschamp
