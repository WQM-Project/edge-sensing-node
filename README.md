# 🌊 edge-sensing-node

**STM32-based water quality edge sensor node** — part of the [WQM-Project](https://github.com/WQM-Project) ecosystem.

This repo contains the firmware for the **edge sensing node**: a NUCLEO-F722ZE board wired to a multi-parameter water quality sensor suite. Its single job is to:

1. **Acquire** readings from 10+ sensors (pH, turbidity, TDS, dissolved oxygen, conductivity, ORP, temperature, depth, atmospheric conditions)
2. **Serialize** them into a compact JSON packet
3. **Transmit** via UART (→ LoRa SX1278 in deployment) to the [gateway](https://github.com/WQM-Project) for cloud upload

---

## System Position

```
┌─────────────────────┐       LoRa 868 MHz       ┌───────────────┐       4G LTE       ┌──────────────┐
│  edge-sensing-node  │ ─────────────────────────▶│    gateway    │ ──────────────────▶│  Firebase /  │
│  (this repo)        │   JSON over UART→SX1278   │  (ESP8266 +   │   HTTPS + JSON    │  Cloud + ML  │
│  STM32F722ZE        │                           │   SIM7600E)   │                   │  Dashboard   │
└─────────────────────┘                           └───────────────┘                   └──────────────┘
```

---

## Sensor Suite

| Sensor | Model | Interface | MCU Pin | Parameter | Unit |
|---|---|---|---|---|---|
| pH | DFRobot SEN0161 | Analog | PA3 (ADC\_CH3) | Water pH | 0–14 |
| Turbidity | DFRobot SEN0189 | Analog | PC0 (ADC\_CH10) | Turbidity | NTU |
| TDS | Gravity SEN0244 | Analog | PC3 (ADC\_CH13) | Total Dissolved Solids | ppm |
| DO | DFRobot SEN0237-A | Analog | PA4 (ADC\_CH4) | Dissolved Oxygen | mg/L |
| Conductivity | DFRobot DFR0300 | Analog | PA5 (ADC\_CH5) | Electrical Conductivity | µS/cm |
| ORP | DFRobot SEN0165 | Analog | PA6 (ADC\_CH6) | Oxidation-Reduction Potential | mV |
| Temperature (×2) | DS18B20 | OneWire | PE6 | Water Temperature | °C |
| Depth | JSN-SR04T | GPIO (Trig/Echo) | PE4 / PE5 | Water Level | cm |
| Atmospheric | BME280 | I2C1 | PB8/PB9 | Air Temp, Humidity, Pressure | °C, %, hPa |

---

## JSON Telemetry Packet

Every `SAMPLE_INTERVAL_MS` (default **5 s**), the node serializes all sensor data and transmits it over USART3:

```json
{
  "id": "WQM-001",
  "ts": 123456789,
  "pH": 7.12,
  "turb_ntu": 42.5,
  "tds_ppm": 310.0,
  "do_mgl": 6.85,
  "ec_uscm": 680.2,
  "orp_mv": 215.0,
  "temp_w1": 23.50,
  "temp_w2": 23.48,
  "depth_cm": 35.2,
  "temp_air": 28.60,
  "humidity": 65.3,
  "pressure_hpa": 1013.25
}
```

| Field | Description |
|---|---|
| `id` | Device identifier (supports multi-node deployment) |
| `ts` | Timestamp — `HAL_GetTick()` ms since boot |
| `pH` | pH value (two-point calibrated) |
| `turb_ntu` | Turbidity in NTU (quadratic polynomial fit) |
| `tds_ppm` | Total Dissolved Solids, temperature-compensated |
| `do_mgl` | Dissolved Oxygen in mg/L (pressure + temp compensated) |
| `ec_uscm` | Electrical Conductivity in µS/cm |
| `orp_mv` | Oxidation-Reduction Potential in mV |
| `temp_w1` / `temp_w2` | Primary and backup water temperature (DS18B20) |
| `depth_cm` | Ultrasonic water-level distance |
| `temp_air` / `humidity` / `pressure_hpa` | Atmospheric conditions (BME280) |

---

## Firmware Architecture

The firmware follows a **stop-and-sample** sequential acquisition pattern, optimized for sensor settling times:

```
main()
  ├── SystemClock_Config()       216 MHz HSE PLL
  ├── MX_*_Init()                ADC1, I2C1, USART3, TIM6, GPIO
  ├── BME280_Init()              Read calibration NVM
  ├── DS18B20_SearchROM()        Discover 1-Wire devices
  │
  └── while(1)
        ├── BME280_ReadAll()          Step 1: Atmospheric
        ├── DS18B20 Convert + Read    Step 2: Water temp (750ms conversion)
        ├── Sensor_ReadConductivity() Step 3: Fast analog sensors
        ├── Sensor_ReadTDS()
        ├── Sensor_ReadTurbidity()
        ├── Sensor_ReadORP()          Step 4: ORP
        ├── Sensor_ReadPH()           Step 5: pH
        ├── Sensor_ReadDO()           Step 6: DO (longest settling)
        ├── JSNSR04T_ReadDistance()    Step 7: Ultrasonic depth
        │
        ├── WQM_BuildJSON()           Serialize → JSON
        ├── Debug_Print()             TX via USART3
        ├── Toggle LD1 (PB0)          Heartbeat LED
        └── HAL_Delay(5000)           Wait for next sample
```

---

## Building

### Prerequisites

- [STM32CubeIDE](https://www.st.com/en/development-tools/stm32cubeide.html) (v1.12+)
- NUCLEO-F722ZE board (or compatible STM32F722ZE target)

### Steps

1. **Clone** this repo
   ```bash
   git clone https://github.com/WQM-Project/edge-sensing-node.git
   ```
2. **Open** in STM32CubeIDE: `File → Import → Existing Projects into Workspace`
3. **Build**: `Ctrl+B` (or `Project → Build Project`)
4. **Flash**: Connect the NUCLEO board via USB ST-Link and click `Run → Debug`

The project also includes an **IAR EWARM** workspace under `EWARM/` if you prefer that toolchain.

---

## Sensor Calibration

All calibration constants are defined as `#define` macros at the top of [`Core/Src/main.c`](Core/Src/main.c). See the detailed reference in [`Sensor_Readme/Sensor_Variables_Readme.md`](Sensor_Readme/Sensor_Variables_Readme.md).

Key calibration values to adjust for your hardware:

| Constant | Default | What to calibrate |
|---|---|---|
| `PH_CAL_SLOPE` / `PH_CAL_OFFSET` | -5.70 / 21.34 | Two-point calibration with pH 4.0 and 7.0 buffers |
| `TURB_COEFF_A/B/C` | -1120.4 / 5742.3 / -4352.9 | Polynomial curve fit for your turbidity sensor unit |
| `DO_CAL1_V` / `DO_CAL1_T` | 1600 mV / 25°C | Single-point in air-saturated water |
| `EC_K_CAL` | 1.0 | Test against known EC standard solution |
| `ORP_OFFSET` | 0.0 mV | Offset from ORP standard solution |

---

## Peripheral Summary

| Peripheral | Usage |
|---|---|
| **ADC1** | 6 analog sensors (channels 3, 4, 5, 6, 10, 13), 16× oversampling |
| **I2C1** | BME280 environmental sensor (addr `0x76`) |
| **USART3** | Debug output + JSON telemetry TX (→ LoRa module in deployment) |
| **TIM6** | Microsecond-precision delay for OneWire timing |
| **GPIO PE6** | DS18B20 OneWire data bus |
| **GPIO PE4/PE5** | JSN-SR04T ultrasonic trigger/echo |
| **GPIO PB0** | LD1 heartbeat LED |

---

## Related Repositories

| Repo | Role |
|---|---|
| [WQM-Project/WQM](https://github.com/WQM-Project/WQM) | Top-level project documentation, BOM, and planning |
| [WQM-Project/water-quality-ml](https://github.com/WQM-Project/water-quality-ml) | ML pipeline — LSTM prediction and trend analysis |
| Gateway repo | ESP8266 + SIM7600E gateway (coming soon) |

---

## License

*License TBD*
