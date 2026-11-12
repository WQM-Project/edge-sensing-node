# WQM Sensor Configuration Variables

This document details the configuration constants (macros) defined in `main.c` that govern the behavior, calibration, and hardware interfaces for the various sensors used in the Water Quality Monitoring (WQM) project.

## pH Sensor (robu.in SEN0161 V1 Kit) — Connected to PA3 (ADC_CH3)

These parameters govern the conversion of post-divider-reconstructed analog voltage (`VDIV_SCALE = 2.0f`) to pH using a linear regression fit: `pH = (V_sensor × PH_SCALE) + PH_OFFSET`.

* **`PH_SCALE (4.942f)`**: Calibrated linear scale factor (pH/V) fitted against pH 4.00 (`1.357 V`), pH 7.00 (`1.968 V`), and pH 10.00 (`2.571 V`) standard buffers (residual error `<= 0.01 pH`).
* **`PH_OFFSET (-2.713f)`**: Calibrated zero-offset trim from the three-point buffer linear regression.

## Turbidity Sensor (SEN0189)

Turbidity (water cloudiness) is calculated from the voltage drop across an optical sensor using a polynomial equation curve-fit to the sensor's characteristics. `turbidity = (TURB_COEFF_A * x^2 + TURB_COEFF_B * x + TURB_COEFF_C) / 1000` where `x` is the measured voltage in mV (millivolts).

* **`TURB_COEFF_A (-1120.4f)`**: The quadratic coefficient in the turbidity calculation polynomial.
* **`TURB_COEFF_B (5742.3f)`**: The linear coefficient.
* **`TURB_COEFF_C (-4352.9f)`**: The constant/intercept coefficient.

## TDS Sensor (SEN0244) — Connected to PA5 (ADC_CH5)

Total Dissolved Solids (TDS) is calculated using a polynomial curve fit against the measured temperature-compensated voltage (post 2:1 divider), factoring in an empirical conversion factor.

* **`TDS_COEFF_A (133.42f)`**, **`TDS_COEFF_B (-255.86f)`**, **`TDS_COEFF_C (857.39f)`**: The polynomial coefficients for translating the temperature-compensated voltage curve into a TDS value.
* **`TDS_FACTOR (0.5f)`**: An approximate empirical conversion factor used to relate electrical conductivity to TDS (specifically for sodium chloride solutions).
* **`TDS_CAL_K (1.036f)`**: Calibrated cell K-factor against a 1413 µS/cm (706.5 ppm target) standard solution (`706.5 / 682.0 = 1.036`).
* **`TDS_TEMP_COEFF (0.02f)`**: Temperature coefficient (2% per °C) normalizing to 25°C baseline.

## DO (Dissolved Oxygen) Sensor (SEN0237-A) — Connected to PA4 (ADC_CH4)

Dissolved oxygen measurements use a Galvanic amperometric probe with 7.5% KCl electrolyte and single-point air saturation calibration + temperature lookup table (`DO_Table[41]`).

* **`DO_CAL1_V (764)`**: The baseline analog voltage (in millivolts) measured at the STM32 ADC pin PA4 (post 2:1 divider, corresponding to 1.54V sensor board output) when the wetted probe is equilibrated in open air at `DO_CAL1_T`.
* **`DO_CAL1_T (24)`**: The ambient temperature (in °C) during air calibration.
* **`DO_TEMP_COMP_MV (17.5f)`**: Voltage drift per degree Celsius post-divider (~35 mV/°C at sensor board output), compensating for membrane permeability changes: $V_{\text{sat}}(T) = \text{DO\_CAL1\_V} + 17.5 \times (T - \text{DO\_CAL1\_T})$.
* **Barometric Pressure Compensation**: Corrected via live BMP280 barometric reading: $\text{DO} \times (P_{\text{actual}} / 1013.25\text{ hPa})$.

## ORP Sensor (SEN0165) — Connected to PC3 (ADC_CH13)

The SEN0165 signal conditioning board outputs an analog voltage referenced to a 2.0V midpoint ($0\text{ mV} \leftrightarrow 2.0\text{ V}$).

* **`ORP_OFFSET (0.0f)`**: Calibration offset applied to the linear calculation: $\text{ORP}_{\text{mV}} = (2.0 - V_{\text{board}}) \times 1000 - \text{ORP\_OFFSET}$.
* **Filtering**: Uses 480 ADC sampling cycles and a 20-sample trimmed mean (rejecting highest and lowest outliers over 100 ms) to suppress high-impedance probe noise.

## DS18B20 Digital Temperature Sensor — Connected to PE6

This sensor communicates over the 1-Wire protocol with an external 4.7kΩ pull-up to 3.3V.

* **`OW_PORT (GPIOE)`** & **`OW_PIN (GPIO_PIN_6)`**: The hardware GPIO pin assigned to the 1-Wire data bus.
* **Command Codes** (`0xCC`, `0x55`, `0xF0`, `0x44`, `0xBE`): Standard 1-Wire hex commands used by the driver to skip ROM addressing, search for devices, trigger temperature conversions, and read the internal scratchpad memory.

## Atmospheric Sensor (BMP280) — Connected to I2C1 (PB8/PB9)

* **`BMP280_ADDR_PRIMARY (0xEC)`** / **`0x76`**: I2C address with SDO grounded.
* Measures ambient air temperature and barometric pressure for dissolved oxygen altitude compensation.

## IP68 Underwater Ultrasonic Obstacle Avoidance Sensor (6 m, UART)

Used for measuring water level via ultrasonic ranging over UART frames.

* **`DEPTH_SENSOR_UART (&huart3)`**: UART instance used for depth sensor receive path.
* **`DEPTH_UART_FRAME_HEADER (0xFFu)`** and **`DEPTH_UART_FRAME_LEN (4u)`**: Conservative framed payload parser assumptions used by the current firmware abstraction.
* **`DEPTH_UART_TIMEOUT_MS (120u)`**: UART receive timeout for one depth frame.
* **`DEPTH_SENSOR_MAX_CM (600.0f)`**: Maximum accepted measurement range (6 meters) before marking the sample invalid.
