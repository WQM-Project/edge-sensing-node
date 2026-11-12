# edge-sensing-node

**STM32-based water quality edge sensor node** — part of the [WQM-Project](https://github.com/WQM-Project) ecosystem.

This repo contains the firmware for the **edge sensing node**: a NUCLEO-F722ZE board wired to a multi-parameter water quality sensor suite. Its single job is to:

1. **Acquire** readings from 10+ sensors (pH, turbidity, TDS, dissolved oxygen, conductivity, ORP, temperature, depth, atmospheric conditions)
2. **Serialize** them into a compact JSON packet
3. **Transmit** via UART (→ LoRa SX1278 in deployment) to the [gateway](https://github.com/WQM-Project) for cloud upload

---

## System Position

```
┌─────────────────────┐       LoRa 868 MHz        ┌───────────────┐       4G LTE      ┌──────────────┐
│  edge-sensing-node  │────────────────────────▶ │    gateway     │ ─────────────────▶│  Firebase /  │
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
| TDS | Gravity SEN0244 | Analog | PA5 (ADC\_CH5) | Total Dissolved Solids | ppm |
| DO | DFRobot SEN0237-A | Analog | PA4 (ADC\_CH4) | Dissolved Oxygen | mg/L |
| ORP | DFRobot SEN0165 | Analog | PC3 (ADC\_CH13) | Oxidation-Reduction Potential | mV |
| Temperature (×2) | DS18B20 | OneWire | PE6 | Water Temperature | °C |
| Depth | IP68 Underwater Ultrasonic Obstacle Avoidance Sensor (6 m) | UART | USART3 RX (PD9) | Water Level | cm |
| Atmospheric | BMP280 | I2C1 | PB8/PB9 | Air Temp, Pressure | °C, hPa |
| Navigation / IMU | BNO085 | I2C1 | PB8/PB9 | Orientation (heading, pitch, roll) | ° |
| Location | NEO-M8N / NEO-6M | UART | UART4 (PC10/PC11) | GPS Navigation & Coordinates | — |

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
| `depth_cm` | Ultrasonic water-level distance from UART depth frame |
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
        ├── DepthSensor_ReadDistance() Step 7: UART depth
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
| **USART3** | JSON telemetry TX + depth sensor UART RX |
| **UART4** | NEO-M8N GPS UART RX (`PC11`) |
| **TIM6** | Microsecond-precision delay for OneWire timing |
| **GPIO PE6** | DS18B20 OneWire data bus (external 4.7kΩ pull-up) |
| **GPIO PB0** | LD1 heartbeat LED |

---

## Related Repositories

| Repo | Role |
|---|---|
| [WQM-Project/WQM](https://github.com/WQM-Project/WQM) | Top-level project documentation, BOM, and planning |
| [WQM-Project/water-quality-ml](https://github.com/WQM-Project/water-quality-ml) | ML pipeline — LSTM prediction and trend analysis |
| [WQM-Project/gateway](https://github.com/WQM-Project/gateway) | ESP8266 + SIM7600E gateway & cellular uploader |

---

## License

*License TBD*
