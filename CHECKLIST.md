# Edge Sensing Node — Progress Checklist

> **Target:** STM32F722ZE (NUCLEO-F722ZE)  
> **Project:** WQM-Project / edge-sensing-node  
> **Last updated:** 2026-10-09

---

## ✅ Completed

### Hardware / CubeMX Setup
- [x] STM32CubeIDE project created for NUCLEO-F722ZE
- [x] System clock configured (216 MHz HSE PLL)
- [x] MPU configured
- [x] GPIO initialized (LEDs, trigger/echo pins, OneWire data pin)
- [x] ADC1 configured — 6 analog channels (CH3, CH4, CH5, CH6, CH10, CH13)
- [x] I2C1 configured — BMP280 at address `0x76` + BNO085 IMU at `0x4A`
- [x] USART3 configured — debug output and telemetry TX
- [x] TIM6 configured — microsecond-precision delay for OneWire timing
- [x] USB OTG FS PCD initialized

### Sensor Drivers — Analog (ADC-based)
- [x] ADC single-channel reader with 16× oversampling (`ADC_Read_Channel`)
- [x] ADC-to-voltage converter accounting for 2:1 voltage divider (`ADC_Read_Voltage`)
- [x] **pH** — SEN0161 V1 read on PA3 (ADC_CH3) with 3-point buffer calibration (`PH_SCALE = 4.942f`, `PH_OFFSET = -2.713f`), verified in tap vs detergent (`Sensor_ReadPH`)
- [x] **Turbidity** — SEN0189 quadratic polynomial conversion driver implemented (`Sensor_ReadTurbidity`, omitted from active JSON build)
- [x] **TDS** — SEN0244 on PA5 (ADC_CH5) cubic polynomial with temperature compensation + 1413 µS/cm calibration (`TDS_CAL_K = 1.036f`), physical salt disturbance verified (`Sensor_ReadTDS`)
- [x] **Dissolved Oxygen** — SEN0237-A on PA4 (ADC_CH4) with DO saturation lookup table, 7.5% KCl electrolyte (`DO_CAL1_V = 764`, `DO_CAL1_T = 24`, `DO_TEMP_COMP_MV = 17.5f`), BMP280 pressure compensation, thermal deaeration verified in hot water (`Sensor_ReadDO`)
- [x] **Conductivity** — DFR0300 linear conversion driver implemented (`Sensor_ReadConductivity`, omitted from active JSON build)
- [x] **ORP** — SEN0165 on PC3 (ADC_CH13) 2.0V midpoint formula + 20-sample trimmed mean noise filter, detergent disturbance verified (`Sensor_ReadORP`)

### Sensor Drivers — Digital
- [x] **DS18B20** — Full OneWire bit-bang driver on PE6 (reset, read/write bit, read/write byte), active single-probe readout (`temp_w1`)
- [x] DS18B20 ROM search — discovers sensors (READ ROM + SEARCH ROM)
- [x] DS18B20 MATCH ROM temperature read with 12-bit resolution
- [x] DS18B20 SKIP ROM bulk convert for all sensors
- [x] Primary/backup temperature failover logic (falls back to 25°C default if disconnected)
- [x] **JSN-SR04T** — Ultrasonic distance measurement driver (omitted from active JSON build)
- [x] **BMP280** — Full I2C driver: register read/write, burst read (`temp_air`, `pressure_hpa`)
- [x] BMP280 chip ID verification on init (`0x58` at `0x76`/`0x77`)
- [x] BMP280 factory calibration NVM readout (temperature and pressure coefficients)
- [x] BMP280 Bosch compensation algorithms for temperature and barometric pressure (used for DO altitude/pressure compensation)
- [x] BMP280 single-burst read of temperature and pressure
- [x] **BNO085** — 9-DOF Orientation IMU Fusion over I2C (SH-2 / SHTP protocol)
- [x] BNO085 rotation vector to Euler angles (Heading/Pitch/Roll) conversion
- [x] **GPS** — NMEA parser over UART4 interrupt (GGA + RMC sentences for Lat/Lon/Alt/Speed/Heading)

### Data Collection & Sampling
- [x] `WQM_SensorData_t` struct defined — holds all 12 sensor readings
- [x] Stop-and-sample sequential acquisition loop implemented (7-step sequence)
- [x] Sensor reading order optimized for settling times (BMP280 → DS18B20 → fast analog → ORP → pH → DO)
- [x] 5-second sampling interval (`SAMPLE_INTERVAL_MS = 5000`)
- [x] Sample counter tracking

### Data Packet — JSON Serialization
- [x] `WQM_BuildJSON()` function builds compact JSON from sensor data struct
- [x] Active JSON packet includes environmental fields: `id`, `ts`, `pH`, `tds_ppm`, `do_mgl`, `orp_mv`, `temp_w1`, `temp_air`, `pressure_hpa` (unfitted `turb_ntu`, `ec_uscm`, `temp_w2`, `depth_cm`, `humidity` omitted)
- [x] JSON packet includes navigation fields: `lat`, `lon`, `alt_m`, `sog_knots`, `cog_deg`, `gps_fix`, `sats`, `heading`, `pitch`, `roll`
- [x] Device ID embedded in packet (`"WQM-001"`) — supports multi-node deployment
- [x] Timestamp from `HAL_GetTick()` (ms since boot)
- [x] JSON buffer size increased to 768 bytes
- [x] JSON packet transmitted over USART3 via `Debug_Print()`

### Debug & Diagnostics
- [x] Boot banner printed over UART
- [x] BMP280 init status reported (OK / ERR)
- [x] DS18B20 sensor count reported at boot
- [x] Heartbeat LED toggle (LD1 / PB0) each sample cycle
- [x] Debug UART helper function (`Debug_Print`)

### Documentation
- [x] README.md with system position diagram, sensor table, JSON format, firmware architecture, build instructions, calibration guide, and peripheral summary

---

## ❌ Not Yet Done

### Communication — Onboard ESP32 UART RX–TX Link
- [x] `WQM_BuildJSON` active 19-field newline-delimited JSON serializer (`STM32_INTEGRATION.md` contract)
- [ ] Dedicated 3.3V UART TX/RX port (`115200 8N1` to onboard ESP32 `GPIO25`/`GPIO26`) separate from USB debug UART
- [ ] `ACK` / `NAK` reply handling and retry logic (`send_reading` per `STM32_INTEGRATION.md`)
- [ ] Onboard end-to-end soak test with ESP32 + SIM7600 4G LTE + GNSS gateway

### Power Management
- [ ] Low-power sleep between samples (STOP/STANDBY modes)
- [ ] RTC-based wakeup for timed sampling
- [ ] Battery voltage monitoring
- [ ] Power-on/off control for sensor rails

### Reliability & Error Handling
- [ ] Sensor health status flags per reading
- [ ] Watchdog timer (IWDG) for hang recovery
- [ ] CRC or checksum on JSON packet
- [ ] Sensor timeout / failure counters
- [ ] Non-volatile logging of error events (Flash or EEPROM)

### Calibration
- [ ] Runtime calibration via UART commands (instead of recompiling `#define` values)
- [ ] Calibration coefficients stored in Flash/EEPROM
- [ ] Multi-point calibration support for pH, DO, EC

### OTA / Configuration
- [ ] Remote configuration update mechanism
- [ ] Firmware version field in JSON packet
- [ ] Device ID settable at runtime (not just compile-time `#define`)

### Testing
- [ ] Unit-level validation of each sensor driver against known inputs
- [ ] End-to-end JSON packet validation
- [ ] Long-duration soak test (stability over hours/days)
- [ ] Power consumption profiling

---

## Summary

| Category | Status |
|---|---|
| CubeMX / Peripheral Init | ✅ Done |
| Analog Sensor Drivers (6) | ✅ Done |
| Digital Sensor Drivers (3) | ✅ Done |
| Data Collection Loop | ✅ Done |
| **JSON Data Packet** | **✅ Done** |
| Debug UART Output | ✅ Done |
| Onboard ESP32 UART Link | 🟡 In Progress (`WQM_BuildJSON` ready; dedicated UART + ACK/NAK pending) |
| Power Management | ❌ Not started |
| Error Handling / Watchdog | ❌ Not started |
| Runtime Calibration | ❌ Not started |
