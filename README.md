# ESP32 Wireless IEM Pack

A custom-built, high-performance wireless In-Ear Monitor (IEM) system designed to drive high-fidelity audio hardware (such as the Moondrop Aria and Edge).

## Hardware Bill of Materials

* **Microcontroller:** ESP32 (UI/Display logic currently being prototyped on Arduino)
* **Display:** 1.54" Full Color TFT Display Module (SPI)
* **Power Management:** TP4056 Type-C USB 5V 1A Lithium Battery Charger
* **Battery:** EEMB 3.7V 103454 2000mAh LiPo (Prototyping phase: 4.5V via 3xAA pack)
* **Wire:** 30AWG Tinned Copper Silicone

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
* [ ] Prototyping 1.54" TFT UI on Arduino via SPI (3xAA power).
* [ ] Porting C codebase to ESP32.

## Author

Damian Luciano Muschamp
