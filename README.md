# edge-sensing-node

**STM32-based water quality edge sensor node** — part of the [WQM-Project](https://github.com/WQM-Project) ecosystem.

This repo contains the firmware for the **edge sensing node**: a NUCLEO-F722ZE board wired to a multi-parameter water quality sensor suite. Its single job is to:

1. **Acquire** readings from active onboard sensors (pH, TDS, dissolved oxygen, ORP, water temperature, barometric pressure, air temperature, and 9-DOF IMU orientation)
2. **Serialize** them into a compact JSON packet
3. **Transmit** directly over 3.3V UART RX–TX to the onboard ESP32 + SIM7600 [gateway](https://github.com/WQM-Project/gateway), which attaches SIM7600 GNSS coordinates + UTC time and pushes data over 4G LTE to the server (no LoRa radio or separate shore gateway; see [STM32_INTEGRATION.md](STM32_INTEGRATION.md))

---

## System Position

```
┌──────────────────────┐    3.3V UART RX–TX    ┌──────────────────────┐      4G LTE       ┌──────────────────┐
│  edge-sensing-node   │──────────────────────▶│  onboard gateway     │─────────────────▶│  Supabase Cloud  │
│  (this repo)         │   JSON @ 115200 8N1   │  (ESP32 + SIM7600    │   HTTPS + JSON   │  + ML Pipeline   │
│  STM32F722ZE         │◀──────────────────────│   4G LTE + GNSS HAT) │                  │  + Dashboards    │
└──────────────────────┘       ACK / NAK       └──────────────────────┘                  └──────────────────┘
```

---

## Sensor Suite

| Sensor | Model | Interface | MCU Pin | Parameter | Unit |
|---|---|---|---|---|---|
| pH | DFRobot SEN0161 | Analog | PA3 (ADC\_CH3) | Water pH | 0–14 |
| Turbidity | DFRobot SEN0189 | Analog | PC0 (ADC\_CH10) | Turbidity | NTU |
| TDS | Gravity SEN0244 | Analog | PA5 (ADC\_CH5) | Total Dissolved Solids | ppm |
| DO | DFRobot SEN0237-A | Analog | PA4 (ADC\_CH4) | Dissolved Oxygen | mg/L |
| ORP | DFRobot SEN0165 | Analog | PC3 (ADC\_CH13) | Oxidation-Reduction Potential | mV |
| Temperature (×2) | DS18B20 | OneWire | PE6 | Water Temperature | °C |
| Depth | IP68 Underwater Ultrasonic Obstacle Avoidance Sensor (6 m) | UART | USART3 RX (PD9) | Water Level | cm |
| Atmospheric | BMP280 | I2C1 | PB8/PB9 | Air Temp, Pressure | °C, hPa |
| Navigation / IMU | BNO085 | I2C1 | PB8/PB9 | Orientation (heading, pitch, roll) | ° |
| Location | Onboard SIM7600 GNSS *(via ESP32)* / Optional NEO-M8N | UART | ESP32+SIM7600 *(optional `UART4` `PC10/PC11`)* | GPS Navigation & Coordinates | — |

---

## JSON Telemetry Packet

Every `SAMPLE_INTERVAL_MS` (default **5 s**), the node serializes the active 19-field telemetry line (see [STM32_INTEGRATION.md](STM32_INTEGRATION.md)) and transmits it over UART to the onboard ESP32 gateway:

```json
{
  "id": "WQM-001",
  "ts": 166519,
  "pH": 7.12,
  "tds_ppm": 310.0,
  "do_mgl": 8.84,
  "orp_mv": 91.6,
  "temp_w1": 23.62,
  "temp_air": 28.60,
  "pressure_hpa": 1013.25,
  "lat": 0.000000,
  "lon": 0.000000,
  "alt_m": 0.0,
  "sog_knots": 0.0,
  "cog_deg": 0.0,
  "gps_fix": 0,
  "sats": 0,
  "heading": 0.0,
  "pitch": 0.0,
  "roll": 0.0
}
```

| Field | Description |
|---|---|
| `id` | Device identifier (supports multi-node deployment) |
| `ts` | Timestamp — `HAL_GetTick()` ms since boot |
| `pH` | pH value (3-point linear regression: `PH_SCALE = 4.942f`, `PH_OFFSET = -2.713f`) |
| `tds_ppm` | Total Dissolved Solids in ppm (`TDS_CAL_K = 1.036f`, DS18B20 temperature-compensated) |
| `do_mgl` | Dissolved Oxygen in mg/L (7.5% KCl, BMP280 pressure + DS18B20 temp compensated) |
| `orp_mv` | Oxidation-Reduction Potential in mV (2.0V midpoint formula + 20-sample trimmed mean) |
| `temp_w1` | Water temperature in °C (DS18B20 on `PE6`) |
| `temp_air` / `pressure_hpa` | Atmospheric air temperature and barometric pressure (`BMP280` on `PB8/PB9`, no RH sensor) |
| `lat` / `lon` / `alt_m` / `sog_knots` / `cog_deg` / `gps_fix` / `sats` | Optional STM32 GNSS pass-through (`UART4`); the onboard ESP32 + SIM7600 module provides primary GPS coordinates & strips unfixed `(0, 0)` placeholders |
| `heading` / `pitch` / `roll` | BNO085 9-DOF IMU Euler orientation (`I2C1`) |

*(Note: Optional drivers for `turb_ntu` / `ec_uscm` / `temp_w2` / `depth_cm` exist in firmware, and `humidity` is omitted with BMP280.)*

---

## Firmware Architecture

The firmware follows a **stop-and-sample** sequential acquisition pattern, optimized for sensor settling times:

```
main()
  ├── SystemClock_Config()       216 MHz HSE PLL
  ├── MX_*_Init()                ADC1, I2C1, USART3, UART4, TIM6, GPIO
  ├── BMP280_Init()              Read calibration NVM (addr 0x76)
  ├── BNO085_Init()              Initialize 9-DOF IMU (addr 0x4A)
  ├── DS18B20_SearchROM()        Discover 1-Wire devices
  │
  └── while(1)
        ├── BMP280_ReadAll()          Step 1: Atmospheric pressure & air temp
        ├── BNO085_Update()           Step 2: IMU orientation
        ├── DS18B20 Convert + Read    Step 3: Water temp (750ms conversion)
        ├── Sensor_ReadTDS()          Step 4: TDS (PA5)
        ├── Sensor_ReadORP()          Step 5: ORP (PC3, 20-sample trimmed mean)
        ├── Sensor_ReadPH()           Step 6: pH (PA3)
        ├── Sensor_ReadDO()           Step 7: DO (PA4, longest settling)
        │
        ├── WQM_BuildJSON()           Serialize 19-field active JSON
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
| `PH_SCALE` / `PH_OFFSET` | `4.942f` / `-2.713f` | Linear regression (`pH = PH_SCALE × V_sensor + PH_OFFSET`) across pH 4.00, 7.00, 10.00 buffers (2:1 divider on `PA3`) |
| `TDS_CAL_K` | `1.036f` | K-value calibration factor against 1413 µS/cm (706.5 ppm) standard on `PA5` |
| `TURB_COEFF_A/B/C` | -1120.4 / 5742.3 / -4352.9 | Polynomial curve fit for your turbidity sensor unit (currently omitted from active JSON build) |
| `DO_CAL1_V` / `DO_CAL1_T` | 764 mV / 24°C | Single-point air saturation voltage (post 2:1 divider, ~1.54V board output) |
| `DO_TEMP_COMP_MV` | 17.5 mV/°C | Temperature coefficient post-divider (~35 mV/°C at sensor output) |
| `ORP_OFFSET` | 0.0 mV | Offset from ORP standard solution (reconstructed 2.0V midpoint formula) |

---

## Peripheral Summary

| Peripheral | Usage |
|---|---|
| **ADC1** | Analog sensors (PA3, PA4, PA5, PC3, PC0), 16× oversampling, 480 cycles sampling time |
| **I2C1** | BMP280 barometer (`0x76`) and BNO085 9-DOF IMU (`0x4A`) |
| **USART3** | JSON telemetry TX + USB debug ST-LINK VCP (dedicated 3.3V UART RX–TX to onboard ESP32 per [STM32_INTEGRATION.md](STM32_INTEGRATION.md)) |
| **UART4** | Optional NEO-M8N GPS UART RX (`PC11`; primary GNSS comes from SIM7600 on the onboard ESP32 gateway) |
| **TIM6** | Microsecond-precision delay for OneWire timing |
| **GPIO PE6** | DS18B20 OneWire data bus (external 4.7kΩ pull-up) |
| **GPIO PB0** | LD1 heartbeat LED |

---

## Related Repositories

| Repo | Role |
|---|---|
| [WQM-Project/WQM](https://github.com/WQM-Project/WQM) | Top-level project documentation, BOM, and planning |
| [WQM-Project/water-quality-ml](https://github.com/WQM-Project/water-quality-ml) | ML pipeline — LSTM prediction and trend analysis |
| [WQM-Project/gateway](https://github.com/WQM-Project/gateway) | Onboard ESP32 + SIM7600 4G LTE & GNSS gateway (UART RX–TX from STM32) |

---

## License

*License TBD*
