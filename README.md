# ESP32 Wireless IEM Pack

A first-pass wireless in-ear monitor (IEM) system: one ESP32 transmitter pack and one ESP32 receiver pack. The RX path is `ESP-NOW → ESP32 → I2S → PCM5102 DAC → volume pot → MAX97220A headphone amp → 3.5 mm jack`.

This repo is a work in progress. The breadboard version plays audio. The KiCad project in `KiCad/RX_SKETCHES/` is that breadboard transferred onto hierarchical sheets — not a finished layout. There is **no display on this version**.

## What this version is (and is not)

| | This version | Not this version |
|---|---|---|
| Display | None | The old 1.54" TFT / Arduino UI idea |
| RX power | USB-C from a 10,000 mAh 5 V power bank → LM3940 3.3 V | TP4056 + 2000 mAh LiPo / 3xAA |
| TX power | The same kind of 10,000 mAh 5 V power bank as the RX | A different battery chemistry or a shared bank |
| Radio | ESP32-WROOM-32U + **`ANT1` 2.4 GHz U.FL pigtail** on `esp_rx` | On-module PCB antenna (that is the 32E / 32D) |
| Audio | I2S to a PCM5102, then a headphone amp | I2C (the DAC is I2S) |
| PCB | Footprint dump / ratsnest only | Routed board ready to order |

TX and RX each get their **own** identical 10,000 mAh bank. Do not power both packs from one bank if you want them to roam independently.

## Repo map

```
main/                  ESP-IDF C firmware
  rig_shared.h         Shared packet / state types (TX and RX)
  tx.c                 Transmitter (ESP-NOW + ADC pot + LED). Audio fill is still TODO.
  rx.c                 Receiver stub. Set IEM_NODE=RX when it has an app_main.
  square_wave_test.c   Bench I2S tone used to prove the DAC path
  sniffer.c            Monitor / debug helper
KiCad/RX_SKETCHES/     RX hierarchical schematic (WIP)
KiCad/TX_SKETCHES/     TX placeholder
KiCad/Custom_Part_Downloads/  SnapEDA symbols / footprints used by the RX project
```

Build a node with ESP-IDF after exporting the target:

```bash
IEM_NODE=TX idf.py build
# IEM_NODE=RX or IEM_NODE=MON once those mains are complete
```

`main/CMakeLists.txt` refuses to build if `IEM_NODE` is missing. That is intentional so you do not flash the wrong image.

## Hardware (RX 1.0.0 KiCad)

Hierarchical sheets, in signal order:

1. **`pwr_mgmt`** — USB-C (CC pulldowns), CP2102C UART bridge, LM3940 5 V → 3.3 V
2. **`esp_rx`** — ESP32-WROOM-32U + `ANT1` (U.FL pigtail / 2.4 GHz antenna)
3. **`dac`** — PCM5102
4. **`vol_ctrl`** — dual-gang log pot + 0% switch
5. **`amp`** — MAX97220A to the 3.5 mm jack

### Firmware I2S map (do not "improve" these GPIOs)

These match the working breadboard and `main/square_wave_test.c`:

| Hierarchical label | ESP32 GPIO | Firmware name |
|---|---|---|
| `BCK` | IO33 | `BCK_PIN` |
| `LRCLK` | IO32 | `LRCK_PIN` |
| `DIN` | IO25 | `DOUT_PIN` (ESP data **out** to the DAC data **in**) |

UART programming: `TXD` = TXD0/IO1, `RXD` = RXD0/IO3. Auto-program straps: `ESP_EN` = EN, `ESP_IO0` = IO0.

### Antenna (`ANT1` on `esp_rx`)

The **32U** module has a U.FL connector and **no PCB antenna**. `ANT1` is a BOM / documentation part: a **2.4 GHz, 50 Ω antenna on a U.FL / IPEX pigtail** that mates to the connector already on the module. It is not wired to a GPIO. It has no PCB footprint — you buy the pigtail, you do not etch an antenna.

If you would rather have an on-board antenna later, that is a different module (WROOM-32E / 32D), not a missing copper pour on the 32U.

### Analog (PCM5102 datasheet vs experiments)

The PCM5102 datasheet analog network for a ~10 kΩ load is already on the DAC sheet (`R16`/`R17` 470 Ω, `C20`/`C21` 2.2 nF):

`OUTL/OUTR → 470 Ω series → PRE_POT`, with **2.2 nF from that node to GND**.

The MAX97220A input impedance is also about **10 kΩ**, so that filter is the right starting point. The experimental 1 kΩ / 10 Ω pad on `vol_ctrl` is **DNP**, with a short across the 1 kΩ positions so an empty series part does not open the audio path.

MAX97220A voltage gain is **fixed +3.5 dB**. Input network resistors `R1–R6`, `R9`, `R10` are **DNP** on hand-built protos so you can try values. Populate them (start 10 kΩ) only if you order a fully assembled board. Use a MAX97220**D** if you want resistor-programmed gain.

### Hand assembly

Schematic footprint fields are **0805**. Use 0805 for every resistor and capacitor you will solder yourself, including 10 / 22 / 33 µF. 0603 is OK if you are already comfortable. Do not use 0402 or 0201. The `.kicad_pcb` still shows 0201 until you **Update PCB from Schematic**.

ICs stay their real packages (WROOM module, TSSOP-20, TQFN-16, SOT-223, WQFN-24).

## Current status

- [x] Breadboard RX path plays (ESP32 I2S → PCM5102 → amp → IEMs)
- [x] Repo + KiCad folder split (`RX_SKETCHES` / `TX_SKETCHES`)
- [x] RX hierarchical schematic transferred from the breadboard (WIP)
- [x] Hierarchical labels rewired; `ANT1` added on `esp_rx`
- [ ] Buy / fit the U.FL pigtail; confirm ESP-NOW range
- [ ] Finish `tx.c` audio packet fill / `rx.c` I2S consume
- [ ] Run ERC, then floorplan and route the RX PCB
- [ ] TX schematic (same 10,000 mAh USB-C bank idea)

No TFT / SPI display work on this revision.

## Common KiCad / first-board mistakes

This is a first KiCad project *and* a first C / ESP-IDF / multi-IC audio board. The list below is the stuff that already bit this schematic. Check it every time you move a part or add a hierarchical label.

### 1. Rotating a symbol does not remap pin numbers

Pin 3 is still `EN` after a 270° rotate. KiCad only rotates the **drawing** and the **sheet coordinates** of the pins around the symbol origin. Hierarchical labels and wires that stay put will attach to whichever pin rotated into that spot.

After any rotate: click each pin, read the name, then move the label onto **that** pin. Do not assume “left side is still EN.”

### 2. Hierarchical labels connect through wires, not vibes

A label sitting near a pin is not connected. ERC and the ratsnest follow **wires and pin ends**. After a rotate, a label can look “on” the chip and still sit on empty space or on the wrong pin (`ESP_IO0` on `EN`, `BCK` on `DEMP`, `SHDN_SIGNAL` on `C1N`).

Triple-check every hierarchical label against the **pin name**, not the side of the box.

### 3. Same-ish names are different pins

| You meant | Easy to hit instead | What happens |
|---|---|---|
| PCM5102 `BCK` / `DIN` / `LRCK` | `DEMP` / `XSMT` / `FMT` | DAC never gets PCM; clocks toggle config bits |
| MAX97220 `~{SHDN}` | `C1N` (charge-pump cap) | Volume switch does not mute; VSS pump breaks |
| ESP `TXD0` / `RXD0` | `SENSOR_VN` / empty space | USB-C UART programming dies |
| I2S | I2C | Wrong bus, wrong pins, wrong mental model |

Read the pin **name** in the status bar before you drop the label.

### 4. Firmware GPIO numbers are the contract

`#define BCK_PIN 33` is IO33 on the module, period. Do not “clean up” the schematic onto IO18/19/23 unless you change the C defines too. Label `DIN` on the DAC is the ESP’s **DOUT**.

### 5. The footprint field is a part you have to buy and solder

If the field says `R_0201_0603Metric`, you will get 0201s. Bulk 10 µF parts basically do not exist in 0201, and you will not hand-assemble them. Set 0805 (or 0603) **before** you Update PCB from Schematic.

### 6. DNP does not delete the net

KiCad “Do not populate” still leaves the schematic wires. If you DNP a **series** resistor, the path goes open unless you add a solder-bridge / short. If you DNP a **shunt**, leaving the pads empty is correct (no short to GND).

### 7. 32U vs 32E, A vs D, datasheet vs doodle

- **32U** = U.FL only. **32E/32D** = PCB antenna. Pick one and BOM the matching RF parts.
- **MAX97220A** ≠ **MAX97220D**. The letter is the gain mode.
- Prefer the datasheet application circuit (470 Ω + 2.2 nF here) over a pad you tried on the bench, unless you measured why the pad is better.

### 8. The `.kicad_pcb` is not a board until it has Edge.Cuts and tracks

A pile of footprints with a ratsnest is a placeholder. After schematic net or footprint changes, **Update PCB from Schematic**. Do not send that file to a fab.

### 9. Power and analog will buzz on a breadboard

Long jumpers, no ground plane, and an ESP32 radio next to a DAC is a buzz/hiss machine. That does not automatically mean the circuit is wrong. A PCB with short I2S, local decoupling, and a solid GND will almost certainly be quieter. It is not guaranteed to be silent on the first spin.

### 10. Keep the README honest

If the schematic is USB-C + LDO and no display, the README must not still say TP4056 + LiPo + TFT. Future-you (and anyone reviewing a PR) will trust the wrong power tree.

## Author

Damian Luciano Muschamp
