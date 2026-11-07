# WQM Edge Sensing Node — Sensor Wiring & Testing Guide

> **Board:** NUCLEO-F722ZE (STM32F722ZET6)  
> **Date:** 2026-10-20  
> **Status:** DO sensor fixed, single DS18B20 configured, ORP formula fixed & 222 mV standard solution calibrated. Active hardware bring-up.

---

> [!NOTE]
> Git merge conflicts in `MX_GPIO_Init` have been resolved. The firmware compiles cleanly for the NUCLEO-F722ZE target.

---

## Testing Strategy: One Sensor At A Time

Wire and verify each sensor individually before adding the next. This isolates problems to exactly one connection at a time.

**Testing order** (easiest → hardest, matching settling-time dependencies):

| Phase | Sensor | Interface | Why This Order |
|-------|--------|-----------|----------------|
| 0 | Fix merge conflicts | — | Build won't compile otherwise |
| 1 | UART debug output | USART3 (PD8/PD9) | You need serial output to see anything |
| 2 | BME280 | I2C1 (PB8/PB9) | Simplest digital sensor, validates I2C bus |
| 3 | BNO085 | I2C1 (PB8/PB9) | Second I2C device, validates bus sharing |
| 4 | DS18B20 ×1 | OneWire (PE6) | Digital, no ADC — validates bit-bang timing |
| 5 | pH (SEN0161) | ADC CH3 (PA3) | First analog — validates ADC + voltage divider |
| 6 | Turbidity (SEN0189) | ADC CH10 (PC0) | Second analog channel |
| 7 | TDS (SEN0244) | ADC CH5 (PA5) | Needs water temp for compensation |
| 8 | DO (SEN0237-A) | ADC CH4 (PA4) | Needs temp + pressure compensation |
| 9 | Conductivity (DFR0300) | ADC CH5 (PA5) | Needs temp compensation |
| 10 | ORP (SEN0165) | ADC CH13 (PC3) | Last analog |
| 11 | Depth sensor | UART RX via USART3 (PD9) | Shares USART3 RX line |
| 12 | GPS (NEO-6M/BN-220) | UART4 (PC10/PC11) | Interrupt-driven, test last |

---

## 0. Fix Merge Conflicts First

Open [`main.c`](file:///c:/Users/karti/OneDrive/Desktop/PROJECTS/edge-sensing-node-temp/Core/Src/main.c) and search for `<<<<<<<`. There are **two conflict blocks** in `MX_GPIO_Init()` around lines 1460–1485.

**What to keep:** The version from the `bf071aa` branch (the `>>>>>>>` side), because it includes PE4 as output and PE5+PE6 as input, which matches your CubeMX `.ioc` configuration.

After resolving:
1. Build → `Ctrl+B`
2. Flash → `Run → Debug`
3. You should see the boot banner on the serial terminal

---

## 1. UART Debug Output (USART3 — ST-Link VCP)

### No Extra Wiring Needed

The NUCLEO-F722ZE routes USART3 through the on-board ST-Link as a **Virtual COM Port (VCP)**. Pins PD8 (TX) and PD9 (RX) are already connected on the board.

### Test Steps

1. Connect the NUCLEO board via USB to your PC
2. Open a serial terminal (PuTTY, Tera Term, or STM32CubeIDE's built-in terminal)
3. Settings: **115200 baud, 8N1** (matches [`MX_USART3_UART_Init`](file:///c:/Users/karti/OneDrive/Desktop/PROJECTS/edge-sensing-node-temp/Core/Src/main.c#L1372-L1400))
4. Flash the firmware and reset the board
5. **Expected output:**
```
========================================
  WQM Sensor Suite — NUCLEO-F722ZE
========================================
[ERR] BME280 not found! Check I2C wiring.
[OK]  DS18B20 sensors found: 0
[ERR] GPS UART failed to initialize!
[ERR] BNO085 failed to initialize! Check I2C wiring.
========================================
```

> [!TIP]
> All `[ERR]` messages are expected at this stage — no sensors are wired yet. The important thing is that you see text at all. If you see nothing, check your COM port selection and baud rate.

### ✅ Pass Criteria
- Boot banner prints on serial terminal
- JSON packets print every 5 seconds (all values will be 0.00 or defaults)

---

## 2. BMP280 (I2C1 — Air Temp / Pressure)

> [!IMPORTANT]
> The firmware uses a **BMP280**, not a BME280. The BMP280 has **no humidity sensor**. The `"humidity"` field does not exist in the JSON output.

### Wiring

| BMP280 Pin | NUCLEO Pin | Wire Color (suggested) |
|------------|------------|------------------------|
| **VCC** | 3.3V (CN8 pin 7 or CN7 pin 16) | 🔴 Red |
| **GND** | GND (CN8 pin 11 or CN7 pin 20) | ⚫ Black |
| **SCL** | PB8 (CN7 pin 2) | 🟡 Yellow |
| **SDA** | PB9 (CN7 pin 4) | 🔵 Blue |

> [!IMPORTANT]
> - The BMP280 address in firmware is `0x76` (SDO/CSB tied to GND or floating on most breakouts).
> - If your breakout pulls SDO HIGH, the address becomes `0x77` — init will fail at `0x76`.
> - Most BMP280 breakouts have **on-board pull-ups** (4.7kΩ). Do NOT add external pull-ups unless your scope shows sluggish edges.

### Test Steps

1. Wire BMP280 with board **powered off**
2. Flash and reset
3. Check serial output:
   ```
   [OK]  BMP280 initialized (ID=0x58 at 0x76)
   ```
4. Wait for JSON packets and check fields:
   ```json
   "temp_air": 28.60, "pressure_hpa": 1013.25
   ```

### Sanity Checks

| Field | Reasonable Range (indoor) | If Out Of Range |
|-------|---------------------------|-----------------|
| `temp_air` | 20–40 °C | Wiring or I2C address issue |
| `pressure_hpa` | 900–1100 hPa | Calibration data read failed |

> [!NOTE]
> There is **no `humidity` field** — BMP280 does not measure humidity. If you need humidity, you'd need a BME280 (same pinout, different chip ID `0x60`, different firmware driver).

### Debugging I2C

If `[ERR] BMP280 not found!`:

1. **Swap SDA/SCL** — most common mistake
2. **Check voltage** — BMP280 is 3.3V; the NUCLEO I2C is 3.3V
3. **I2C bus scan** — temporarily add this code after `MX_I2C1_Init()` in main to scan all addresses:
```c
// Temporary I2C bus scanner — remove after testing
char scan_msg[64];
Debug_Print("[I2C] Scanning bus...\r\n");
for (uint8_t addr = 1; addr < 127; addr++) {
    if (HAL_I2C_IsDeviceReady(&hi2c1, addr << 1, 2, 50) == HAL_OK) {
        snprintf(scan_msg, sizeof(scan_msg), "[I2C] Device found at 0x%02X\r\n", addr);
        Debug_Print(scan_msg);
    }
}
Debug_Print("[I2C] Scan complete.\r\n");
```

### ✅ Pass Criteria
- `[OK]  BMP280 initialized (ID=0x58 at 0x76)` on boot
- `temp_air` and `pressure_hpa` show reasonable values
- Moving to higher/lower elevation over time shows pressure change

---

## 3. BNO085 (I2C1 — 9-DOF IMU / Heading)

### Wiring

The BNO085 shares the **same I2C1 bus** as the BME280. Keep the BME280 connected.

| BNO085 Pin | NUCLEO Pin | Wire Color (suggested) |
|------------|------------|------------------------|
| **VCC** / **3V3** | 3.3V | 🔴 Red |
| **GND** | GND | ⚫ Black |
| **SCL** | PB8 (same wire as BME280) | 🟡 Yellow |
| **SDA** | PB9 (same wire as BME280) | 🔵 Blue |
| **RST** | Leave unconnected (driver does soft-reset) | — |
| **INT** | Leave unconnected (polled mode) | — |

> [!IMPORTANT]
> - BNO085 address: `0x4A` (SA0 = LOW, which is the default on SmartElex breakout)
> - If your breakout has SA0 pulled HIGH, address is `0x4B` — update `BNO085_I2C_ADDR` in [`bno085.h`](file:///c:/Users/karti/OneDrive/Desktop/PROJECTS/edge-sensing-node-temp/Core/Inc/bno085.h#L41)
> - The BNO085 takes **~300 ms to boot** after power-on/reset — the driver handles this with `HAL_Delay(300)` in [`BNO085_Init`](file:///c:/Users/karti/OneDrive/Desktop/PROJECTS/edge-sensing-node-temp/Core/Src/bno085.c#L241-L290)

### Test Steps

1. Wire BNO085 to the same I2C bus (BME280 stays connected)
2. Flash and reset
3. Check serial:
   ```
   [OK]  BME280 initialized (ID=0x60)
   [OK]  BNO085 initialized.
   ```
4. Check JSON fields:
   ```json
   "heading": 127.5, "pitch": -2.3, "roll": 0.8
   ```
5. **Rotate the board** — `heading` should change smoothly 0–360°
6. **Tilt the board** forward/backward — `pitch` should change ±90°
7. **Roll the board** left/right — `roll` should change ±180°

### Debugging

If `[ERR] BNO085 failed to initialize!`:

1. Run the I2C scanner from Phase 2 — you should see **two** devices: `0x4A` and `0x76`
2. Check that VCC is really 3.3V (BNO085 is NOT 5V tolerant)
3. Try adding a 100 ms delay before `BNO085_Init()` — some breakouts boot slower
4. If using a different BNO085 breakout (Adafruit, SparkFun), check SA0 jumper

### ✅ Pass Criteria
- Both I2C devices detected on the same bus (no bus contention)
- `heading` responds smoothly to rotation
- `pitch` and `roll` respond to tilting
- BME280 readings remain stable while BNO085 is active

---

## 4. DS18B20 ×1 (OneWire — Water Temperature)

> [!NOTE]
> Firmware is configured for **one DS18B20 only**. `sensor_data.water_temp_2` is always `0.0` and `temp_w2` in the JSON is always `0.00`. Only `temp_w1` carries a live reading.

### What You Need

- 1× DS18B20 waterproof probe (3-wire: Red, Yellow/White, Black)
- 1× **4.7 kΩ resistor** (pull-up — mandatory for OneWire to work)
- Breadboard or soldered connection

---

### Why the Pull-Up Resistor?

OneWire is an **open-drain** bus. The DS18B20 can only pull the data line LOW; it cannot drive it HIGH. The 4.7 kΩ resistor connected between the data line and 3.3V acts as the only mechanism to bring the line back HIGH between pulses. Without it, the STM32 reads garbage or no presence pulse at all.

---

### Wiring — Step by Step

#### Probe Wire Colours

| Probe Wire Colour | Function | Connect To |
|-------------------|----------|------------|
| **Red** | VDD (power) | 3.3V |
| **Yellow / White** | DQ (data) | PE6 **and** one end of the 4.7 kΩ resistor |
| **Black** | GND | GND |

> [!CAUTION]
> Some cheap DS18B20 probes have **Red and Yellow swapped**. If you get -127°C or 85°C on first boot, swap those two wires.

---

#### Pull-Up Resistor Placement

The resistor bridges **3.3V → PE6 (data line)**. It does not go in series with anything — it is a shunt to the supply rail.

```
                          4.7 kΩ
  3.3V ───────────────────[===]────┬──── PE6  (NUCLEO CN10 pin 28)
                                   │
                              Yellow/White
                             (DQ of probe)

  Red ──────────────────────────────────── 3.3V
  Black ────────────────────────────────── GND
```

#### On a Breadboard

```
          NUCLEO                       Breadboard
    ┌───────────────┐          ┌──────────────────────────┐
    │               │          │                          │
    │  3.3V (CN8-7) ├──────────┤ Rail (+) ───┬────────── │
    │               │          │              │           │
    │  GND  (CN8-11)├──────────┤ Rail (−)    │           │
    │               │          │              ▼           │
    │  PE6 (CN10-28)├──────────┤ Row A   [4.7kΩ]         │
    │               │          │              ▼           │
    └───────────────┘          │         Row B ◄─── Yellow │
                               │                          │
                               │  Red   ◄─────── (+) Rail  │
                               │  Black ◄─────── (−) Rail  │
                               └──────────────────────────┘
```

**Key rules:**
- One leg of the 4.7 kΩ resistor connects to the **3.3V rail**
- The other leg connects to the **same row as the Yellow wire and PE6**
- Red probe wire connects to the **3.3V rail**
- Black probe wire connects to the **GND rail**

---

### NUCLEO Pin Location

| Signal | NUCLEO Connector | Pin Number |
|--------|-----------------|------------|
| **PE6** (OneWire DQ) | CN10 (right Morpho) | Pin 28 |
| **3.3V** | CN8 (Arduino) | Pin 7 |
| **GND** | CN8 (Arduino) | Pin 11 |

> [!TIP]
> Use a **multimeter in continuity mode** to verify: probe from PE6 (CN10 pin 28) to 3.3V through the resistor. You should measure ~4.7 kΩ. Probe from Yellow wire to PE6 — you should measure ~0 Ω (direct connection).

---

### Test Steps

1. Wire the sensor with pull-up resistor as above (board **powered off**)
2. Flash and reset
3. Check serial:
   ```
   [OK]  DS18B20 sensors found: 1
   ```
4. Check JSON:
   ```json
   "temp_w1": 23.50, "temp_w2": 0.00
   ```
   > `temp_w2` is always `0.00` — this is expected; second sensor is disabled in firmware.
5. **Hold the probe** in your hand — temperature should rise slowly over ~10 seconds
6. **Dip in cold water** — should drop quickly

---

### Common Issues

| Symptom | Cause | Fix |
|---------|-------|-----|
| `sensors found: 0` | Pull-up resistor missing or wrong pin | Verify 4.7kΩ between PE6 and 3.3V |
| `sensors found: 0` | Yellow wire not on same row as PE6 | Check breadboard row connections |
| `temp_w1: 85.00` | Conversion not completing (power issue) | Ensure Red wire is on 3.3V, check pull-up |
| `temp_w1: -127.00` | No presence pulse / CRC error | Check continuity of Yellow wire to PE6 |
| `temp_w1` stuck at same value | Wire loose / bad contact | Reseat wires, try shorter cables |

### ✅ Pass Criteria
- `[OK]  DS18B20 sensors found: 1` on boot
- `temp_w1` shows ambient temperature (±2°C of room temp)
- `temp_w2` is always `0.00`
- Hand-warming causes a gradual, smooth rise

---

## 5. pH Sensor (SEN0161 — ADC Channel 3)

> [!IMPORTANT]
> **Confirmed model: DFRobot Gravity SEN0161 V1 (the version sold on Robu.in).** This is the **V1** (not V2):
> - Supply voltage: **strictly 5V only** — no onboard regulator. Do not power from 3.3V.
> - Analog output: **0–5V** (full rail output) — the 2:1 voltage divider is **mandatory** to protect the STM32 ADC.
> - Calibration: **manual** — via potentiometer on board (hardware coarse-trim) + `PH_CAL_SLOPE` / `PH_CAL_OFFSET` in firmware (software fine-trim). V1 does **not** support auto two-point calibration libraries.

### Wiring

| SEN0161 Signal Board Pin | NUCLEO Pin |
|--------------------------|------------|
| **V+** | **5V (CN8 pin 9) — mandatory, V1 is 5V-only** |
| **GND** | GND |
| **A (Analog Out)** | PA3 — **through 2:1 voltage divider (mandatory)** |

> [!CAUTION]
> **Voltage Divider is mandatory for SEN0161 V1.** The signal board outputs **0–5V**. The STM32F7 ADC maximum is 3.3V. Without the divider you will permanently damage the ADC input. Use two **1% tolerance** 10kΩ resistors:

```
SEN0161 Analog Out ──── 10kΩ ──┬── PA3 (ADC_CH3)
                                │
                               10kΩ
                                │
                               GND
```

The firmware's [`VDIV_SCALE = 2.0`](file:///c:/Users/karti/OneDrive/Desktop/PROJECTS/edge-sensing-node-temp/Core/Src/main.c#L106) accounts for this divider.

**BNC Probe:** Connect the pH probe BNC connector to the signal board. If no probe is connected, the output will float randomly.

### Test Steps

1. Wire the signal board with voltage divider
2. **Do NOT connect the BNC probe yet** — verify the circuit first
3. Flash and reset
4. Check JSON `"pH"` field — it will be random without a probe (expected)
5. **Connect the pH probe** (keep it in storage solution or pH 7.0 buffer)
6. With probe in **pH 7.0 buffer**: expect `"pH": 6.8–7.2`
7. Move probe to **pH 4.0 buffer**: expect `"pH": 3.8–4.2`

### Calibration

If readings are off, adjust [`PH_CAL_SLOPE`](file:///c:/Users/karti/OneDrive/Desktop/PROJECTS/edge-sensing-node-temp/Core/Src/main.c#L112) and [`PH_CAL_OFFSET`](file:///c:/Users/karti/OneDrive/Desktop/PROJECTS/edge-sensing-node-temp/Core/Src/main.c#L113) in main.c:

1. Record voltage at pH 7.0: `V7`
2. Record voltage at pH 4.0: `V4`
3. Calculate:
   ```
   PH_CAL_SLOPE  = (7.0 - 4.0) / (V7 - V4)
   PH_CAL_OFFSET = 7.0 - (PH_CAL_SLOPE × V7)
   ```

### ✅ Pass Criteria
- ADC reads non-zero voltage when signal board powered
- pH 7.0 buffer reads 6.8–7.2
- pH 4.0 buffer reads 3.8–4.2
- Readings are stable (±0.1 after 30s settling)

---

## 6. Turbidity Sensor (SEN0189 — ADC Channel 10)

### Wiring

| SEN0189 Signal Board Pin | NUCLEO Pin |
|--------------------------|------------|
| **V+** | 5V |
| **GND** | GND |
| **A (Analog Out)** | PC0 — **through voltage divider** |

Same 2:1 voltage divider as pH:
```
SEN0189 Analog Out ──── 10kΩ ──┬── PC0 (ADC_CH10)
                                │
                               10kΩ
                                │
                               GND
```

### Test Steps

1. Wire with voltage divider
2. Flash and reset
3. **In clean water / air**: `"turb_ntu"` should be near 0
4. **In murky water** (stir in some dirt): value should increase toward 100+ NTU
5. **Block the optical window** with your finger: should read high NTU (sensor saturated)

### ✅ Pass Criteria
- Clear water/air reads < 10 NTU
- Murky water reads noticeably higher
- Values clamped 0–3000 NTU (firmware clamp)

---

## 7. TDS Sensor (SEN0244 — ADC Channel 5)

> [!NOTE]
> **Confirmed model: DFRobot Gravity SEN0244.** The signal board operates on **3.3V–5.5V** and its analog output is **0–2.3V max** — it never reaches 5V. This means the STM32F7 ADC (3.3V tolerant) is **safe even without a voltage divider** when the board is powered at 3.3V. However, the firmware uses `VDIV_SCALE = 2.0` for all analog channels, so keep the 2:1 divider in place to match the firmware's assumption, or power at 3.3V and change `VDIV_SCALE` for this channel.

### Wiring

| SEN0244 Pin | NUCLEO Pin |
|-------------|------------|
| **V+** | **3.3V** (safe; board accepts 3.3V–5.5V; output stays ≤ 2.3V at any VCC) |
| **GND** | GND |
| **A (Analog Out)** | PA5 — **through 2:1 voltage divider** (required to match firmware `VDIV_SCALE = 2.0`) |

> [!TIP]
> Powering the SEN0244 at **3.3V** is the cleanest option: the signal board works correctly, the output is ≤ ~1.5V, and the divider gives ≤ 0.75V at the ADC pin — well within range. If you power at 5V the output is ≤ 2.3V; the divider gives ≤ 1.15V at PA5 — also safe.

Voltage divider (keep regardless of VCC choice, to match firmware):
```
SEN0244 Analog Out ──── 10kΩ ──┬── PA5 (ADC_CH5)
                                │
                               10kΩ
                                │
                               GND
```

### Test Steps

1. Wire with divider
2. **In air**: `"tds_ppm"` should be 0 or near-zero
3. **In tap water**: ~100–500 ppm typically
4. **Add a pinch of salt**: ppm should jump significantly

> [!NOTE]
> TDS uses temperature compensation from DS18B20. If DS18B20 isn't connected, it falls back to 25°C default — readings will still work but won't be fully compensated.

### ✅ Pass Criteria
- Air reads ~0 ppm
- Tap water reads 100–500 ppm (varies by location)
- Adding salt causes clear increase

---

## 8. Dissolved Oxygen (SEN0237-A — ADC Channel 4)

> [!NOTE]
> **Confirmed model: DFRobot Gravity SEN0237-A (Galvanic DO Probe).** Key verified specs:
> - Supply voltage: **3.3V–5.5V** (not 5V-only — the board has a wide-range regulator)
> - Analog output: **0–3.0V maximum** (the signal board hardware-limits the output to 3.0V regardless of VCC)
> - Probe type: **Galvanic** (requires no polarization warm-up time — ready immediately on power-on)
> - Probe life: ~1 year; membrane cap replacement every 1–4 months depending on water clarity

### Wiring

| SEN0237-A Pin | NUCLEO Pin |
|---------------|------------|
| **V+** | **3.3V or 5V** (both work; board accepts 3.3V–5.5V) |
| **GND** | GND |
| **A (Analog Out)** | PA4 — **through 2:1 voltage divider** |

> [!IMPORTANT]
> The SEN0237-A output is **hardware-capped at 3.0V** by the signal board — it cannot exceed 3.3V. The 2:1 divider is still required because the firmware uses a **unified `VDIV_SCALE = 2.0`** for all analog channels. Removing the divider would require a separate scale factor for this channel in `Sensor_ReadDO()`. Keep the divider to maintain firmware consistency.

### Calibration Constants (current values)

| Define | Value | Meaning |
|--------|-------|---------|
| [`DO_CAL1_V`](file:///c:/Users/karti/OneDrive/Desktop/PROJECTS/edge-sensing-node-temp/Core/Src/main.c#L118) | `1320` | Measured **1.32V** in open air at 25°C (reconstructed after ÷2 divider) |
| [`DO_CAL1_T`](file:///c:/Users/karti/OneDrive/Desktop/PROJECTS/edge-sensing-node-temp/Core/Src/main.c#L121) | `25` | Temperature (°C) at which the above voltage was measured |
| [`DO_TEMP_COMP_MV`](file:///c:/Users/karti/OneDrive/Desktop/PROJECTS/edge-sensing-node-temp/Core/Src/main.c#L122) | `35` | ~35 mV correction per °C deviation from `CAL1_T` |

### Recalibration Procedure

If the probe is replaced or drifts:

1. Wet the probe membrane, expose to **open air** at a known temperature
2. Let it settle for ~30 seconds
3. Add a temporary `Debug_Print` of `voltage_mv` inside [`Sensor_ReadDO`](file:///c:/Users/karti/OneDrive/Desktop/PROJECTS/edge-sensing-node-temp/Core/Src/main.c#L881) to read the raw mV
4. Update [`DO_CAL1_V`](file:///c:/Users/karti/OneDrive/Desktop/PROJECTS/edge-sensing-node-temp/Core/Src/main.c#L118) to that measured value and `DO_CAL1_T` to the current temperature

### Test Steps

1. Wire with divider
2. **In open air at ~25°C**: should read **~8.25 mg/L** (100% O₂ saturation)
3. **Submerge in freshly stirred water**: similar to air value (near-saturated)
4. **Let water go stagnant**: DO should gradually decrease over time
5. **DO uses both temp and pressure compensation** — connect BMP280 and DS18B20 first for accurate readings

### How the Formula Works

```
V_saturation = CAL1_V + 35 × temp_°C − CAL1_T × 35
DO (mg/L)   = (voltage_mv × DO_Table[temp]) / V_saturation / 1000.0
```

- [`DO_Table`](file:///c:/Users/karti/OneDrive/Desktop/PROJECTS/edge-sensing-node-temp/Core/Src/main.c#L225-L229) stores values as **mg/L × 1000** (e.g., `8250` = 8.250 mg/L at 25°C)
- Pressure compensation: `DO = DO_raw × (BMP280_pressure / 1013.25)`
- Output clamped to **0–20 mg/L**

> [!WARNING]
> If you ever see a constant **20.0 mg/L**, the formula is producing a value above the clamp. Check: (1) `DO_CAL1_V` matches your actual measured voltage, (2) voltage divider is wired correctly, (3) the divisor is `1000.0f` not `100.0f` inside `Sensor_ReadDO`.

### ✅ Pass Criteria
- Open-air reading: **7.5–9.0 mg/L** at 20–30°C, near sea level
- Value tracks temperature (higher temp → lower saturation)
- Never stuck at 20.0 mg/L

---

## 9. Conductivity Sensor (DFR0300 — ADC Channel 5)

### Wiring

| DFR0300 Pin | NUCLEO Pin |
|-------------|------------|
| **V+** | 5V |
| **GND** | GND |
| **A (Analog Out)** | PA5 — **through voltage divider** |

### Test Steps

1. Wire with divider
2. **In air**: should read ~0 µS/cm
3. **In tap water**: ~200–800 µS/cm
4. **Add salt**: conductivity should spike

### ✅ Pass Criteria
- Air reads ~0
- Tap water reads a reasonable value
- Cross-check: TDS ≈ EC × 0.5 (approximately)

---

## 10. ORP Sensor (SEN0165 — ADC Channel 6)

### How the SEN0165 Works & Why Readings Were in the 900s

The DFRobot SEN0165 signal conditioner uses an op-amp circuit centered around a **2.00V midpoint reference** (representing 0 mV ORP):
- A **positive ORP potential** (oxidizing environment, e.g. 222 mV standard solution) drives the board output **below 2.00V** (~1.778V).
- A **negative ORP potential** (reducing environment) drives the board output **above 2.00V**.

#### Mathematical Formula

The relationship is linear:
$$\text{ORP}_{\text{mV}} = (2.000 - V_{\text{board}}) \times 1000 - \text{ORP\_OFFSET}$$

where $V_{\text{board}}$ is the actual 0–5V output voltage of the SEN0165 signal board, reconstructed through our 2:1 voltage divider:
$$V_{\text{board}} = V_{\text{ADC\_pin}} \times \text{VDIV\_SCALE} = \left(\frac{\text{raw\_ADC}}{4095}\right) \times 3.3\text{V} \times 2.0$$

> [!IMPORTANT]
> **Why were readings in the 900s before?**  
> DFRobot's original Arduino sample code used:  
> `((30 * 5.0 * 1000) - (75 * ADC * 5.0 * 1000 / 1024)) / 75`  
> Notice that `30 * 5.0 * 1000 / 75 = 2000 mV`.  
> Previously in firmware, `5.0V` was substituted with `VREF * VDIV_SCALE = 6.6V`.  
> This made `30 * 6.6 * 1000 / 75 = 2640 mV` — adding an artificial **+640 mV error**!  
> In 222 mV standard solution ($V_{\text{board}} \approx 1.74\text{V}$), the firmware was calculating:  
> `2640 - 1740 = 900 mV`!  
> The new simplified formula in `Sensor_ReadORP()` correctly reconstructs $V_{\text{board}}$ and uses the 2.0V center point, giving accurate readings.

---

### Wiring

| SEN0165 Pin | NUCLEO Pin | Notes |
|-------------|------------|-------|
| **V+** | **5V** | SEN0165 requires clean 5.0V supply |
| **GND** | **GND** | Common ground with STM32 |
| **A (Analog Out)** | **PC3** | **MUST go through 2:1 voltage divider (10kΩ / 10kΩ)** |

> [!CAUTION]
> **DO NOT PRESS THE ON-BOARD CALIBRATION BUTTON!**  
> The SEN0165 board has a small physical calibration button. **Never press this button while the ORP probe is attached to the BNC connector!** Doing so can short-circuit or permanently damage the delicate platinum electrode. All calibration should be performed purely in software via `ORP_OFFSET`.

---

### Standard Solution Calibration (222 mV Standard at 25°C)

The standard calibration solution provides **222 mV ± 15 mV at 25°C** (using 3.5 mol/L KCl reference).

#### Temperature Sensitivity of 222 mV Standard:
| Temperature (°C) | Expected Potential (mV) |
|---|---|
| 15°C | ~235 mV |
| 20°C | ~227 mV |
| **25°C** | **222 mV (Standard Reference)** |
| 30°C | ~215 mV |

#### Step-by-Step Calibration Procedure:

1. **Rinse probe:** Thoroughly rinse the ORP electrode with deionized or distilled water and gently pat dry with a lint-free tissue (do **not** rub the platinum tip).
2. **Submerge probe:** Place the electrode into the 222 mV standard calibration solution. Swirl gently for a few seconds.
3. **Allow stabilization:** ORP electrodes have high internal impedance. Wait **1 to 2 minutes** for the reading on the serial terminal (`"orp_mv"`) to stabilize.
4. **Determine error:**
   - Look at the reported `"orp_mv"` in the JSON packet.
   - Example: If serial reports `238.5 mV` at 25°C, the error is $238.5 - 222.0 = +16.5\text{ mV}$.
5. **Apply Software Offset in [`main.c`](file:///c:/Users/karti/OneDrive/Desktop/PROJECTS/edge-sensing-node-temp/Core/Src/main.c#L131):**
   ```c
   /* In Core/Src/main.c around line 131: */
   #define ORP_OFFSET 16.5f  /* measured_mv - expected_222_mv */
   ```
6. **Rebuild & Flash:** Verify that the output now reads `222.0 ± 5 mV` in the standard solution.

---

### Voltage Cross-Check Table

Use a digital multimeter to verify voltages at various stages:

| State / Solution | Expected $V_{\text{board}}$ (Pin A) | Voltage at STM32 PC3 (after divider) | Expected ORP Value |
|---|---|---|---|
| **0 mV Potential (shorted BNC)** | 2.00 V | 1.00 V | ~0 mV |
| **222 mV Standard Solution (25°C)** | ~1.778 V | ~0.889 V | **+222 mV (±15 mV)** |
| **Tap Water (aerobic)** | 1.60 V – 1.80 V | 0.80 V – 0.90 V | +200 mV to +400 mV |
| **Diluted Bleach (oxidizing)** | < 1.40 V | < 0.70 V | > +600 mV |

---

### ✅ Pass Criteria
- **In 222 mV standard solution:** Reads $222 \pm 15\text{ mV}$ (or within ±5 mV after software offset).
- **In tap water:** Stable between $+200$ and $+400\text{ mV}$.
- **Reading stability:** Settles within 60–90 seconds without erratic jumping.

---

## 11. Depth Sensor (IP68 UART — USART3 RX)

### Wiring

| Depth Sensor Wire | NUCLEO Pin |
|-------------------|------------|
| **V+ (Red)** | 5V |
| **GND (Black)** | GND |
| **TX (data out)** | PD9 (USART3_RX) |

> [!WARNING]
> The depth sensor TX may be 5V logic. The STM32F7's USART3 RX (PD9) is **5V tolerant** on most pins — check the datasheet to confirm PD9 is FT (five-volt tolerant). If not, use a voltage divider on the RX line.

### Test Steps

1. Wire the depth sensor
2. Point it at a flat surface (wall, floor) at a known distance
3. Check JSON: `"depth_cm"` should approximate the actual distance
4. Move the sensor closer/farther — value should track

### Frame Format

The depth sensor sends 4-byte frames: `[0xFF] [HIGH] [LOW] [CHECKSUM]`  
Distance = `(HIGH << 8) | LOW` in mm, converted to cm in firmware.

### ✅ Pass Criteria
- Known distances match within ±5 cm
- Values update each sample cycle
- Max range ~600 cm

---

## 12. GPS Module (NEO-6M / BN-220 — UART4)

### Wiring

| GPS Module Pin | NUCLEO Pin |
|----------------|------------|
| **VCC** | 3.3V (most modules) or 5V (check your module) |
| **GND** | GND |
| **TX** | PC11 (UART4_RX) |
| **RX** | PC10 (UART4_TX) — optional, for sending config |

> [!IMPORTANT]
> UART4 baud rate in the `.ioc` is set to `96000` which looks like a typo. GPS modules typically use **9600 baud**. This is defined as `GPS_BAUD_RATE = 9600` in [`gps.h`](file:///c:/Users/karti/OneDrive/Desktop/PROJECTS/edge-sensing-node-temp/Core/Inc/gps.h#L61), but the actual UART4 init in [`MX_UART4_Init`](file:///c:/Users/karti/OneDrive/Desktop/PROJECTS/edge-sensing-node-temp/Core/Src/main.c#L1348) has `BaudRate = 96000`. **Fix this to 9600** before testing GPS.

### Test Steps

1. **Fix UART4 baud rate** — change line 1348 in main.c from `96000` to `9600`
2. Wire GPS module
3. Place near a window or outdoors (GPS needs sky view)
4. Wait 30–90 seconds for cold start fix
5. Check serial for:
   ```
   [OK]  GPS UART initialized.
   ```
6. Check JSON:
   ```json
   "lat": 31.1048, "lon": 77.1734, "alt_m": 2050.0,
   "gps_fix": 1, "sats": 7
   ```
7. Walk around — position should change

### Debugging

- **No fix after 5 minutes outdoors**: Antenna connection issue, or module not getting power
- **`gps_fix: 0` consistently**: Check TX/RX aren't swapped, verify baud rate
- **Garbage characters in serial**: Baud rate mismatch (96000 vs 9600)

### ✅ Pass Criteria
- `gps_fix` changes from 0 to 1/2 after acquiring satellites
- Latitude/longitude match your actual location
- `sats` > 0

---

## Complete Wiring Summary (All Sensors)

### NUCLEO-F722ZE Pin Assignments

```
NUCLEO CN7 (Morpho Left)                NUCLEO CN10 (Morpho Right)
┌─────────────────────────┐              ┌─────────────────────────┐
│ Pin 2  PB8  → I2C1_SCL  │              │ Pin 28 PE6  → OneWire   │
│ Pin 4  PB9  → I2C1_SDA  │              │                         │
│ Pin 16 3.3V → Sensor VCC │              │                         │
│ Pin 20 GND  → Sensor GND │              │                         │
└─────────────────────────┘              └─────────────────────────┘

NUCLEO CN8 (Arduino Left)               NUCLEO CN9 (Arduino Right)
┌─────────────────────────┐              ┌─────────────────────────┐
│ Pin 1  PA3  → pH ADC     │              │                         │
│ Pin 3  PA4  → DO ADC     │              │                         │
│ Pin 5  PA5  → EC ADC     │              │                         │
│ Pin 7  PA6  → ORP ADC    │              │                         │
│ Pin 9  5V   → Sensor VCC │              │                         │
│ Pin 11 GND  → Sensor GND │              │                         │
└─────────────────────────┘              └─────────────────────────┘
```

### Power Rails

| Rail | Source | Consumers | Notes |
|------|--------|-----------|-------|
| **3.3V** | NUCLEO on-board regulator | BME280, BNO085, DS18B20, GPS, **SEN0244 (TDS)**, **SEN0237-A (DO)** | TDS and DO boards accept 3.3V-5.5V |
| **5V** | NUCLEO USB (U5V) or external | **SEN0161 (pH V1)**, Turbidity (SEN0189), EC (DFR0300), **SEN0165 (ORP)**, Depth sensor | pH V1 and ORP are strictly 5V-only |

> [!WARNING]
> **SEN0161 pH V1 and SEN0165 ORP require exactly 5V** - their signal conditioning circuits are referenced to 5V. Running them at 3.3V will produce incorrect readings. TDS (SEN0244) and DO (SEN0237-A) can run from 3.3V, reducing load on the USB 5V rail. Even so, monitor total current draw: the NUCLEO USB 5V rail is limited to ~500 mA. For full field deployment, use an external regulated 5V supply.

### I2C1 Bus Topology

```
        3.3V
         │
        4.7kΩ (on BME280 breakout, usually built-in)
         │
PB8 ────┬────── BME280 SCL ────── BNO085 SCL
PB9 ────┬────── BME280 SDA ────── BNO085 SDA
         │
        4.7kΩ
         │
        3.3V
```

---

## Voltage Divider Board

Since **6 sensors** need identical 2:1 dividers, build a small perfboard:

```
Sensor Analog Out ──── 10kΩ ──┬── To STM32 ADC Pin
                               │
                              10kΩ
                               │
                              GND
```

**Build 6 of these** (pH, turbidity, TDS, DO, EC, ORP) on one perfboard. Label each channel clearly.

> [!TIP]
> Use **1% tolerance resistors** for the dividers. 5% resistors will introduce up to 5% ADC reading error, which is significant for pH calibration.

---

## Quick-Reference: Expected JSON Output (All Sensors Working)

> [!NOTE]
> The BMP280 does **not** produce a `humidity` field. The JSON has 22 fields — no humidity.

```json
{
  "id": "WQM-001",
  "ts": 123456789,
  "pH": 7.12,
  "turb_ntu": 42.5,
  "tds_ppm": 310.0,
  "do_mgl": 6.85,
  "ec_uscm": 680.2,
  "orp_mv": 222.0,
  "temp_w1": 23.50,
  "temp_w2": 0.00,
  "depth_cm": 35.2,
  "temp_air": 28.60,
  "pressure_hpa": 1013.25,
  "lat": 31.1048,
  "lon": 77.1734,
  "alt_m": 2050.0,
  "sog_knots": 0.0,
  "cog_deg": 0.0,
  "gps_fix": 1,
  "sats": 7,
  "heading": 127.5,
  "pitch": -2.3,
  "roll": 0.8
}
```

---

## Troubleshooting Cheat Sheet

| Problem | First Check | Second Check |
|---------|-------------|--------------|
| No serial output at all | COM port + baud rate (115200) | ST-Link driver installed? |
| I2C device not found | SDA/SCL swapped? | Run I2C scanner |
| ADC reads 0 | Voltage divider wired? | Signal board getting 5V? |
| ADC reads 4095 (max) | Input exceeding 3.3V | Divider resistor disconnected? |
| DS18B20 reads 85.0°C | Conversion timing issue | 4.7kΩ pull-up present? |
| DS18B20 reads -127°C | No presence pulse | Check data line wiring |
| GPS no fix | Need sky view | Baud rate 9600 (not 96000!) |
| BNO085 timeout | Boot delay too short | Check SA0 for address |
| Build fails | Merge conflicts in main.c | Search for `<<<<<<<` |
| **DO stuck at 20.0 mg/L** | `DO_CAL1_V` matches measured voltage? | Divisor is `1000.0f` in `Sensor_ReadDO`? |
| **DO reads ~0 mg/L** | Probe membrane wet? | Voltage divider on PA4 present? |
| **BMP280 not found** | Chip is BMP280 (ID=0x58), not BME280 | SDO pin tied correctly for addr 0x76? |
| **ORP reading in 900s** | Old formula bug using 6.6V | Verify latest `Sensor_ReadORP` formula in `main.c` |
| **ORP offset from 222 mV** | Solution temp not 25°C? | Adjust `#define ORP_OFFSET` in `main.c` line 131 |
| **ORP reading unstable** | Electrode needs 1–2 min to stabilize | Check common GND; keep BNC connector dry |

---

## What to Do After All Sensors Pass

Once every sensor passes individually:

1. **Full integration test** — all 13 sensors wired simultaneously, verify JSON packet contains all fields
2. **Long-duration soak test** — run for 1+ hours, check for drift or hangs
3. **Cross-validation** — verify TDS ≈ EC × 0.5, both DS18B20s agree within 0.5°C
4. Move on to the items in your [`CHECKLIST.md`](file:///c:/Users/karti/OneDrive/Desktop/PROJECTS/edge-sensing-node-temp/CHECKLIST.md) under "❌ Not Yet Done"
