# Bill of Materials — WQM Sensor Suite (Recommended Build)

> **Document:** BOM-WQM-SENSOR-2026-R1  
> **Project:** River Water Quality Monitor with Pollution Prediction  
> **Revision:** 1.0  
> **Date:** 2026-08-19  
> **Budget Ceiling:** ₹30,000 (sensor subsystem only)

---

## 1. Sensor Modules

| # | Qty | Part Number | Description | Interface | Unit Price (₹) | Subtotal (₹) | Source |
| --- | ----- | ------------- | ------------- | ----------- | --------------- | -------------- | -------- |
| 1 | 1 | DFRobot SEN0161 | Gravity: Analog pH Meter Kit (glass bulb electrode, BNC) | Analog | 800–1,200 | 800–1,200 | Robu / Robocraze |
| 2 | 1 | DFRobot SEN0189 | Gravity: Analog Turbidity Sensor (IR optical) | Analog | 700–950 | 700–950 | Robu / Robocraze |
| 3 | 1 | DFRobot SEN0244 | Gravity: Analog TDS Sensor Kit | Analog | 300–450 | 300–450 | Robu / Robocraze |
| 4 | 2 | — (generic) | DS18B20 Waterproof Temperature Probe (stainless steel, 1m cable) | Digital (OneWire) | 150–200 | 300–400 | Robu / Amazon IN |
| 5 | 1 | DFRobot SEN0237-A | Gravity: Analog Dissolved Oxygen Sensor Kit (galvanic) | Analog | 3,500–4,200 | 3,500–4,200 | Robu / MakerBazar |
| 6 | 1 | DFRobot DFR0300 | Gravity: Analog Conductivity Sensor (K=1) | Analog | 1,200–1,800 | 1,200–1,800 | Robu / Robocraze |
| 7 | 1 | DFRobot SEN0165 | Gravity: Analog ORP Sensor Meter (platinum electrode, BNC) | Analog | 9,400–10,800 | 9,400–10,800 | Robu / Element14 |
| 8 | 1 | IP68 Underwater Ultrasonic Obstacle Avoidance Sensor | Waterproof Ultrasonic Distance Sensor (0–600 cm) | UART | 250–450 | 250–450 | Robu / Amazon IN |
| 9 | 1 | GY-BMP280 | BMP280 Barometric Pressure + Air Temperature Breakout (no RH on current build) | I2C | 250–400 | 250–400 | Robu / Robocraze |

| | | | | | **Sensor Total** | **₹16,700–20,600** | |

---

## 2. Calibration & Consumables (Initial Kit)

| # | Qty | Description | Est. Price (₹) | Notes |
| --- | ----- | ------------- | ---------------- | ------- |
| C1 | 2 | pH Buffer Solution — pH 4.0 (250 mL) | 150–250 | For two-point calibration |
| C2 | 2 | pH Buffer Solution — pH 7.0 (250 mL) | 150–250 | For two-point calibration |
| C3 | 1 | DO Membrane Cap + Electrolyte Fill (7.5% KCl — 2 g KCl in 26 g water) — spare | 400–600 | First replacement set |
| C4 | 1 | ORP Calibration / Cleaning Solution | 200–400 | For periodic platinum tip maintenance |
| C5 | 1 | Conductivity Calibration Standard (1413 μS/cm, 250 mL) | 200–350 | For K=1 probe calibration |

| | | | **Consumables Total** | **₹1,100–1,850** |

---

## 3. 3D-Printed Parts & Filament

| # | Qty | Part | Material | Est. Print Weight (g) | Notes |
| --- | ----- | ------ | ---------- | ----------------------- | ------- |
| P1 | 1 | Probe mast body (sensor rack, ~30 cm) | PETG | 80–120 | Rigid, UV-resistant, waterproof. Carries all submerged sensors in fixed positions. |
| P2 | 1 | Turbidity sensor shroud/baffle | PETG | 15–25 | Side-slotted cover over SEN0189 optical window. Blocks bubble ingress. |
| P3 | 1 | DO membrane guard cage | PETG | 10–15 | Wire-mesh style cage around galvanic membrane tip. Non-negotiable protection. |
| P4 | 1 | pH bulb guard tube | PETG / PVC | 10–15 | Perforated tube recessing the glass bulb from lateral impact. |
| P5 | 1 | Cable gland mounting plate | PETG | 15–25 | Sealed hull-entry plate with holes for 6–8 cable glands. |
| P6 | 1 | BMP280 + electronics mounting bracket | PLA | 5–10 | Internal hull bracket. No water contact — PLA acceptable. |
| P7 | 1 | IP68 ultrasonic hull-bottom mount | PETG | 10–15 | Angled bracket, faces sensor downward, clear of prop wash. |

| | | | **Est. Total Print Weight** | **145–225 g** | |

**Filament Required:**

| # | Qty | Description | Est. Price (₹) | Notes |
| --- | ----- | ------------- | ---------------- | ------- |
| F1 | 1 | PETG filament — 1 kg spool (white or black) | 800–1,200 | UV-resistant, waterproof, suitable for all submerged parts. ~145–225 g used; rest is spare. |
| F2 | 1 | PLA filament — 250 g or share from existing spool | 0–300 | Only for internal bracket (P6). Skip if PETG is used for everything. |

| | | | **Filament Total** | **₹800–1,500** |

> [!TIP]
> **Why PETG over PLA for submerged parts:** PLA absorbs water over days/weeks and becomes brittle. PETG is hydrophobic, UV-stable, and maintains structural integrity indefinitely underwater. All parts touching water **must** be PETG (or ASA/ABS if available).

---

## 4. Interface Summary (NUCLEO-F722ZE)

| Pin | ADC Channel / Bus | Sensor Connected | Notes |
| --------------- | ------------------- | ----------------- | ----- |
| **PA3** | ADC1 Channel 3 | pH (SEN0161) | 0–5V output via 2:1 divider |
| **PA4** | ADC1 Channel 4 | DO (SEN0237-A) | 0–3V output via 2:1 divider |
| **PA5** | ADC1 Channel 5 | TDS (SEN0244) | 0–2.3V output via 2:1 divider |
| **PC3** | ADC1 Channel 13 | ORP (SEN0165) | Centered at 2.0V via 2:1 divider |
| **PC0** | ADC1 Channel 10 | Turbidity (SEN0189) | 0–4.5V output via 2:1 divider |
| **PE6** | OneWire Bus | DS18B20 (Water Temp) | External 4.7kΩ pull-up to 3.3V |
| **PB8 / PB9** | I2C1 | BMP280 Barometer (`0x76`) + BNO085 IMU (`0x4A`) | Shared I2C bus with pull-ups |
| **PC10 / PC11**| UART4 | Onboard ESP32 + SIM7600 gateway | 115200 8N1 JSON + ACK/NAK (`PC10` TX → `GPIO25`, `PC11` RX ← `GPIO26`) |
| **PD9** | USART3 RX | Depth sensor UART | IP68 ultrasonic distance |

---

## 5. Power Budget (Sensor Subsystem Only)

| Sensor | Active Current | Duty Cycle (Stop-and-Sample) | Avg. Current |
| -------- | --------------- | ------------------------------ | ------------- |
| pH signal board | ~5 mA | 100% (always on during mission) | 5 mA |
| Turbidity (IR LED) | ~30 mA | ~5% (reading only) | 1.5 mA |
| TDS signal board | ~5 mA | 100% | 5 mA |
| DS18B20 × 2 | ~1.5 mA each | ~5% | 0.15 mA |
| DO signal board | ~6 mA | 100% | 6 mA |
| Conductivity board | ~5 mA | 100% | 5 mA |
| ORP signal board | ~5 mA | 100% | 5 mA |
| IP68 ultrasonic (UART) | ~30 mA | ~2% | 0.6 mA |
| BMP280 | ~0.3 mA | ~1% | ~0 mA |
| **Total sensor draw** | | | **~28.25 mA** |

> [!TIP]
> Sensor subsystem draws negligible current compared to propulsion motors (10–30 A). No special power management needed for sensors — they can remain powered throughout a mission without impacting battery life.

---

## 6. Mechanical Mounting Summary

```
HULL (top)
├── BMP280 ..................... internal, away from motor heat
├── IP68 ultrasonic ........... hull bottom, facing down
│
PROBE MAST (submerged, rigid PVC/3D-printed)
├── Conductivity (K=1) ........ upper mast (fast, least sensitive)
├── TDS ....................... upper mast, few cm from conductivity
├── DS18B20 × 2 .............. mid mast (zip-tied / bracket)
├── Turbidity (SEN0189) ....... mid mast, shroud w/ side slots
├── ORP (SEN0165) ............. mid mast, BNC cable gland
├── pH (SEN0161) .............. lower mast, PVC guard tube
└── DO (SEN0237-A) ............ deepest, wire cage over membrane
```

> [!IMPORTANT]
> All analog sensor signal boards must share a **separate regulated power rail** from ESC/drive motors with a **star-ground topology**. This is a design decision to make before the hull is sealed — analog sensors will pick up ESC switching noise if grounding is daisy-chained.

---

## 7. Cost Summary

| Category | Min (₹) | Max (₹) |
| ---------- | --------- | --------- |
| Sensor modules (×10) | 16,700 | 20,650 |
| Calibration consumables (initial) | 1,100 | 1,850 |
| 3D printing filament | 800 | 1,500 |
| **Grand Total** | **₹18,600** | **₹24,000** |
| **Remaining from ₹30K budget** | **₹6,000** | **₹11,400** |

> [!NOTE]
> Remaining budget covers spare probes (DO membrane caps, pH replacement bulbs), additional calibration solutions for extended field campaigns, cable glands, waterproof connectors, and mounting hardware.

---

## 8. Procurement Checklist

- [ ] pH sensor kit (SEN0161) + pH 4.0 & 7.0 buffers
- [ ] Turbidity sensor kit (SEN0189)
- [ ] TDS sensor kit (SEN0244)
- [ ] DS18B20 waterproof probes × 2
- [ ] DO sensor kit (SEN0237-A) — verify 7.5% KCl electrolyte solution (2g KCl in 26g pure water) is included or source locally
- [ ] Conductivity sensor kit (DFR0300) + 1413 μS/cm calibration standard
- [ ] ORP sensor kit (SEN0165) + cleaning solution
- [ ] IP68 underwater ultrasonic obstacle avoidance sensor (6 m, UART)
- [ ] BMP280 breakout board (barometric pressure + air temperature)
- [ ] Spare DO membrane cap + electrolyte
- [ ] PETG filament — 1 kg spool (for probe mast, shrouds, guard cages, mounts)
- [ ] 3D print: probe mast, turbidity shroud, DO cage, pH guard, cable gland plate, mounts

---

*BOM-WQM-SENSOR-2026-R1 | Prepared for WQM-Project/WQM*
