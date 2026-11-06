/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2026 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "bno085.h"
#include "gps.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* ── Sensor data aggregate ──────────────────────────────────────────────────
 */
typedef struct {
  float ph;
  float tds_ppm;
  float do_mgl;
  float orp_mv;
  float water_temp_1; /* DS18B20 primary   */
  float air_temp;     /* BMP280            */
  float pressure_hpa; /* BMP280            */

  /* ── Navigation / localisation (BNO085 + GPS) ──────────────────────── */
  float latitude;     /* GPS decimal degrees, north +     */
  float longitude;    /* GPS decimal degrees, east +      */
  float altitude_m;   /* GPS altitude above MSL (m)       */
  float speed_knots;  /* GPS speed over ground (knots)    */
  float course_deg;   /* GPS course over ground (degrees) */
  uint8_t gps_fix;    /* 0 = no fix, 1 = GPS, 2 = DGPS   */
  uint8_t satellites; /* Satellites in use                 */
  float heading;      /* BNO085 yaw / heading (degrees)   */
  float pitch;        /* BNO085 pitch (degrees)           */
  float roll;         /* BNO085 roll  (degrees)           */
} WQM_SensorData_t;

/* ── BMP280 calibration data (from chip NVM) ────────────────────────────────
 * This structure holds the factory-set calibration coefficients (trimming
 * parameters) burned into the BMP280 sensor's non-volatile memory (NVM).
 */
typedef struct {
  /* Temperature compensation parameters */
  uint16_t dig_T1;
  int16_t dig_T2;
  int16_t dig_T3;

  /* Pressure compensation parameters */
  uint16_t dig_P1;
  int16_t dig_P2;
  int16_t dig_P3;
  int16_t dig_P4;
  int16_t dig_P5;
  int16_t dig_P6;
  int16_t dig_P7;
  int16_t dig_P8;
  int16_t dig_P9;
} BMP280_CalibData_t;

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* ── ADC & voltage ──────────────────────────────────────────────────────────
 */
#define ADC_RESOLUTION 4095.0f /* 12-bit ADC                     */
#define VREF 3.3f              /* STM32 ADC reference voltage    */
#define VDIV_SCALE 2.0f        /* Voltage divider 2:1 (10k/10k) */
#define ADC_SAMPLES 16         /* Oversampling count             */

/* ── pH sensor (robu.in Analog pH Sensor Kit) ───────────────────────────────
 *
 * Transfer curve from vendor source code (Arduino 5V / 10-bit version):
 *   phValue = avgADC × 5.0 / 1024 / 6          → V at ADC pin (0–5V world)
 *   phValue = 3.5 × phValue                    → pH = 3.5 × V_sensor
 *
 * On STM32 (3.3V / 12-bit) with 2:1 voltage divider on PA3:
 *   V_sensor = (ADC_raw / 4095.0) × 3.3 × 2   (ADC_Read_Voltage returns this)
 *   pH       = PH_SCALE × V_sensor + PH_OFFSET
 *
 * Neutral point check: pH 7.0 → V_sensor ≈ 2.0V → 3.5 × 2.0 = 7.0 ✓
 * Acid point check:    pH 4.0 → V_sensor ≈ 1.14V → 3.5 × 1.14 ≈ 4.0 ✓
 *
 * PH_OFFSET is a one-point trim: short the BNC connector (→ pH 7.0 equivalent),
 * read the displayed value, and set PH_OFFSET = 7.0 - displayed_value.
 */
#define PH_SCALE (3.5f)  /* Vendor-specified linear scale factor      */
#define PH_OFFSET (0.0f) /* One-point calibration trim (adjust in situ)*/

/* NOTE: If PA3 has NO hardware voltage divider (sensor output 0-3V direct),
 * change ADC_Read_Voltage() call for pH to use ADC_Read_Channel() with
 * manual ×(3.3/4095.0) and remove the ×2 — or set VDIV_SCALE to 1.0 here.
 * The current code assumes the 2:1 divider IS present on PA3. */

/* ── TDS sensor (SEN0244) polynomial coefficients ───────────────────────────
 */
#define TDS_COEFF_A (133.42f)
#define TDS_COEFF_B (-255.86f)
#define TDS_COEFF_C (857.39f)
#define TDS_FACTOR (0.5f)
#define TDS_TEMP_COEFF 0.02f /* 2% per °C temperature compensation */

/* ── DO sensor (SEN0237-A) calibration ──────────────────────────────────────
 */
#define DO_CAL1_V                                                              \
  1320 /* Calibration voltage (mV): measured 1.32V in air at 25°C             \
          (sensor output ÷ 2 voltage-divider, reconstructed by VDIV_SCALE) */
#define DO_CAL1_T 25       /* Calibration temperature (°C)         */
#define DO_TEMP_COMP_MV 35 /* ~35 mV per °C compensation           */

/* ── ORP sensor (SEN0165) ───────────────────────────────────────────────────
 */
#define ORP_OFFSET 0.0f /* Calibration offset in mV      */

/* ── DS18B20 OneWire ────────────────────────────────────────────────────────
 */
#define OW_PORT GPIOE
#define OW_PIN GPIO_PIN_6
#define DS18B20_CMD_SKIP_ROM 0xCC
#define DS18B20_CMD_MATCH_ROM 0x55
#define DS18B20_CMD_SEARCH 0xF0
#define DS18B20_CMD_CONVERT 0x44
#define DS18B20_CMD_READ_SCRATCH 0xBE

/* ── BMP280 I2C ─────────────────────────────────────────────────────────────
 */
#define BMP280_ADDR_PRIMARY (0x76 << 1) /* 7-bit addr left-shifted for HAL */
#define BMP280_ADDR_SECONDARY (0x77 << 1)
#define BMP280_REG_CALIB_00 0x88 /* Factory calib data start (24 bytes)     */
#define BMP280_REG_ID 0xD0
#define BMP280_REG_RESET 0xE0
#define BMP280_REG_STATUS 0xF3
#define BMP280_REG_CTRL_MEAS 0xF4
#define BMP280_REG_CONFIG 0xF5
#define BMP280_REG_DATA 0xF7 /* Press MSB..XLSB, Temp MSB..XLSB (6 bytes)*/
#define BMP280_CHIP_ID 0x58
#define BMP280_RESET_CMD 0xB6

/* ── JSON buffer ────────────────────────────────────────────────────────────
 */
#define JSON_BUF_SIZE 768 /* Increased for nav/GPS fields */

/* ── BNO085 IMU ─────────────────────────────────────────────────────────────
 */
#define BNO085_REPORT_INTERVAL_MS 100 /* 10 Hz orientation updates      */

/* ── Sampling interval ──────────────────────────────────────────────────────
 */
#define SAMPLE_INTERVAL_MS 1000 /* 1 second between readings     */

/* ── Device ID ──────────────────────────────────────────────────────────────
 */
#define DEVICE_ID                                                              \
  "WQM-001" // incase multiple of these units are deployed later, so each must
            // carry an id

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;

I2C_HandleTypeDef hi2c1;

TIM_HandleTypeDef htim6;

UART_HandleTypeDef huart4;
UART_HandleTypeDef huart3;

PCD_HandleTypeDef hpcd_USB_OTG_FS;

/* USER CODE BEGIN PV */

static BMP280_CalibData_t bmp280_calib;
static int32_t bmp280_t_fine; /* Shared between T & P comp  */
static uint8_t bmp280_i2c_addr = BMP280_ADDR_PRIMARY;
static uint8_t bmp280_ok = 0;
static uint8_t bno085_ok = 0; /* 1 if BNO085 initialized */
static WQM_SensorData_t sensor_data;
static char json_buf[JSON_BUF_SIZE];
static uint32_t sample_counter = 0;

/* BNO085 orientation data */
static BNO085_Orientation_t imu_orient;

/* GPS parsed data */
static GPS_Data_t gps_data;

/* DS18B20 ROM addresses — filled at boot by search */
static uint8_t ds18b20_rom[2][8]; /* ROM codes for up to 2 sensors       */
static uint8_t ds18b20_count = 0; /* Number of sensors discovered        */

/* DO saturation lookup table (mg/L × 1000) from 0°C to 40°C
 * e.g. DO_Table[25] = 8250 → 8.250 mg/L (standard O₂ saturation at 25°C) */
static const uint16_t DO_Table[41] = {
    14460, 14220, 13820, 13440, 13090, 12740, 12420, 12110, 11810, 11530, 11260,
    11010, 10770, 10530, 10300, 10080, 9860,  9660,  9460,  9270,  9080,  8900,
    8730,  8570,  8410,  8250,  8110,  7960,  7820,  7690,  7560,  7430,  7300,
    7180,  7070,  6950,  6840,  6730,  6630,  6530,  6410};

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART3_UART_Init(void);
static void MX_USB_OTG_FS_PCD_Init(void);
static void MX_ADC1_Init(void);
static void MX_I2C1_Init(void);
static void MX_TIM6_Init(void);
static void MX_UART4_Init(void);
/* USER CODE BEGIN PFP */

/* ── Microsecond delay ──────────────────────────────────────────────────────
 */
static void DWT_Init(void);
static void delay_us(uint32_t us);

/* ── ADC helpers ────────────────────────────────────────────────────────────
 */
static uint32_t ADC_Read_Channel(uint32_t channel);
static float ADC_Read_Voltage(uint32_t channel);

/* ── DS18B20 OneWire ────────────────────────────────────────────────────────
 */
static void OW_Init(void);
static inline void OW_Write(uint8_t state);
static inline uint8_t OW_Read(void);
static uint8_t OW_Reset(void);
static void OW_WriteBit(uint8_t bit);
static uint8_t OW_ReadBit(void);
static void OW_WriteByte(uint8_t data);
static uint8_t OW_ReadByte(void);
static void DS18B20_SearchROM(void);
static void DS18B20_StartConvertAll(void);
static float DS18B20_ReadTemp(uint8_t index);

/* ── BMP280 ─────────────────────────────────────────────────────────────────
 */
static uint8_t BMP280_Init(void);
static void BMP280_ReadCalibration(void);
static void BMP280_ReadAll(float *temp, float *press);
static int32_t BMP280_CompensateTemp(int32_t adc_T);
static uint32_t BMP280_CompensatePress(int32_t adc_P);

/* ── Analog sensor conversions ──────────────────────────────────────────────
 */
static float Sensor_ReadPH(void);
static float Sensor_ReadTDS(float water_temp);
static float Sensor_ReadDO(float water_temp, float pressure_hpa);
static float Sensor_ReadORP(void);

/* ── JSON builder ───────────────────────────────────────────────────────────
 */
static uint16_t WQM_BuildJSON(const WQM_SensorData_t *data, char *buf,
                              uint16_t buf_size);

/* ── Debug UART output ──────────────────────────────────────────────────────
 */
void Debug_Print(const char *str);

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* ═══════════════════════════════════════════════════════════════════════════
 */
/*                   MICROSECOND DELAY (Cortex-M7 DWT)                       */
/* ═══════════════════════════════════════════════════════════════════════════
 */

static void DWT_Init(void) {
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->LAR = 0xC5ACCE55; /* Unlock DWT registers on Cortex-M7 */
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static void delay_us(uint32_t us) {
  uint32_t start = DWT->CYCCNT;
  uint32_t ticks = us * (SystemCoreClock / 1000000U);
  while ((DWT->CYCCNT - start) < ticks)
    ;
}

/* ═══════════════════════════════════════════════════════════════════════════
 */
/*                         ADC SINGLE-CHANNEL READER                         */
/* ═══════════════════════════════════════════════════════════════════════════
 */

/**
 * @brief  Read a single ADC channel with oversampling (16 samples averaged).
 * @param  channel  ADC channel number (e.g. ADC_CHANNEL_3)
 * @return Raw 12-bit ADC value (averaged)
 */
static uint32_t ADC_Read_Channel(uint32_t channel) {
  ADC_ChannelConfTypeDef sConfig = {0};
  sConfig.Channel = channel;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_56CYCLES;
  HAL_ADC_ConfigChannel(&hadc1, &sConfig);

  uint32_t sum = 0;
  for (int i = 0; i < ADC_SAMPLES; i++) {
    HAL_ADC_Start(&hadc1);
    HAL_ADC_PollForConversion(&hadc1, 10);
    sum += HAL_ADC_GetValue(&hadc1);
    HAL_ADC_Stop(&hadc1);
  }
  return sum / ADC_SAMPLES;
}

/**
 * @brief  Read ADC channel and convert to real-world voltage (accounting
 *         for voltage divider).
 * @param  channel  ADC channel number
 * @return Voltage at the sensor output (before divider), in volts
 */
static float ADC_Read_Voltage(uint32_t channel) {
  uint32_t raw = ADC_Read_Channel(channel);
  float v_adc = ((float)raw / ADC_RESOLUTION) * VREF;
  return v_adc * VDIV_SCALE; /* Scale back through voltage divider */
}

/* ═══════════════════════════════════════════════════════════════════════════
 */
/*                        DS18B20 — ONEWIRE DRIVER                           */
/* ═══════════════════════════════════════════════════════════════════════════
 */

static void OW_Init(void) {
  GPIO_InitTypeDef g = {0};
  g.Pin = OW_PIN;
  g.Mode = GPIO_MODE_OUTPUT_OD;
  g.Pull = GPIO_PULLUP;
  g.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(OW_PORT, &g);
  OW_PORT->BSRR = OW_PIN; /* Release to float HIGH */
}

static inline void OW_Write(uint8_t state) {
  if (state) {
    OW_PORT->BSRR = OW_PIN; /* Release (open-drain floating HIGH via pull-up) */
  } else {
    OW_PORT->BSRR = (uint32_t)OW_PIN << 16U; /* Drive LOW to GND */
  }
}

static inline uint8_t OW_Read(void) { return (OW_PORT->IDR & OW_PIN) ? 1 : 0; }

/**
 * @brief  OneWire reset pulse. Returns 1 if a device is present, 0 otherwise.
 */
static uint8_t OW_Reset(void) {
  uint8_t presence = 0;
  OW_Write(0);
  delay_us(480);
  OW_Write(1); /* Release bus */

  /* Search for presence pulse (sensor pulls line LOW 15–60µs after release) */
  for (int t = 0; t < 240; t += 10) {
    delay_us(10);
    if (OW_Read() == 0) {
      presence = 1;
    }
  }
  delay_us(240);
  return presence;
}

static void OW_WriteBit(uint8_t bit) {
  if (bit) {
    OW_Write(0);
    delay_us(6);
    OW_Write(1);
    delay_us(54);
  } else {
    OW_Write(0);
    delay_us(60);
    OW_Write(1);
    delay_us(10);
  }
}

static uint8_t OW_ReadBit(void) {
  uint8_t bit;
  OW_Write(0);
  delay_us(3);
  OW_Write(1);
  delay_us(10);
  bit = OW_Read();
  delay_us(50);
  return bit;
}

static void OW_WriteByte(uint8_t data) {
  for (int i = 0; i < 8; i++) {
    OW_WriteBit(data & 0x01);
    data >>= 1;
  }
}

static uint8_t OW_ReadByte(void) {
  uint8_t data = 0;
  for (int i = 0; i < 8; i++) {
    data |= (OW_ReadBit() << i);
  }
  return data;
}

/**
 * @brief  Simple ROM search — discovers up to 2 DS18B20 devices on the bus.
 *         For exactly 2 sensors, this uses the SEARCH ROM algorithm.
 *         Fallback: if only 1 device, READ ROM is used.
 */
static void DS18B20_SearchROM(void) {
  ds18b20_count = 0;
  OW_Init();

  /* Check initial bus idle voltage level */
  uint8_t idle = OW_Read();
  if (idle == 0) {
    Debug_Print("[DS18B20] Pin PE6 reads LOW at idle (bus shorted to GND or no "
                "pull-up resistor).\r\n");
  } else {
    Debug_Print("[DS18B20] Pin PE6 reads HIGH at idle (pull-up OK).\r\n");
  }

  /* Try READ ROM first (works only with a single device) */
  if (!OW_Reset()) {
    Debug_Print(
        "[DS18B20] No presence pulse on PE6 (sensor not responding).\r\n");
    return;
  }
  Debug_Print("[DS18B20] Presence pulse detected!\r\n");

  OW_WriteByte(0x33); /* READ ROM command */
  for (int i = 0; i < 8; i++) {
    ds18b20_rom[0][i] = OW_ReadByte();
  }

  char rom_diag[80];
  snprintf(rom_diag, sizeof(rom_diag),
           "[DS18B20] ROM: %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
           ds18b20_rom[0][0], ds18b20_rom[0][1], ds18b20_rom[0][2],
           ds18b20_rom[0][3], ds18b20_rom[0][4], ds18b20_rom[0][5],
           ds18b20_rom[0][6], ds18b20_rom[0][7]);
  Debug_Print(rom_diag);

  /* Validate family code (0x28 = DS18B20) */
  if (ds18b20_rom[0][0] == 0x28) {
    ds18b20_count = 1;
  } else {
    Debug_Print("[DS18B20] Warning: family code is not 0x28.\r\n");
    return; /* No valid device found */
  }

  /* Attempt to find a second device via SEARCH ROM */
  /* Simplified: try SKIP ROM convert then match each ROM to read.
     For 2 sensors sharing a bus, we do a basic iterative search. */
  uint8_t last_discrepancy = 0;
  uint8_t rom_byte, rom_bit, id_bit, cmp_bit;
  uint8_t search_dir;
  uint8_t temp_rom[8];
  (void)last_discrepancy; /* Used in full search; suppress warning */

  if (!OW_Reset())
    return;
  OW_WriteByte(DS18B20_CMD_SEARCH);

  for (int bit_num = 1; bit_num <= 64; bit_num++) {
    id_bit = OW_ReadBit();
    cmp_bit = OW_ReadBit();

    if (id_bit && cmp_bit) {
      /* No devices on bus */
      return;
    }

    if (id_bit != cmp_bit) {
      search_dir = id_bit;
    } else {
      /* Discrepancy — take the opposite path from ROM[0] */
      rom_byte = (bit_num - 1) / 8;
      rom_bit = (bit_num - 1) % 8;
      search_dir = !(ds18b20_rom[0][rom_byte] & (1 << rom_bit)) ? 1 : 0;
      if (search_dir)
        last_discrepancy = bit_num;
    }

    rom_byte = (bit_num - 1) / 8;
    rom_bit = (bit_num - 1) % 8;
    if (search_dir) {
      temp_rom[rom_byte] |= (1 << rom_bit);
    } else {
      temp_rom[rom_byte] &= ~(1 << rom_bit);
    }

    OW_WriteBit(search_dir);
  }

  /* Validate the second ROM */
  if (temp_rom[0] == 0x28 && memcmp(temp_rom, ds18b20_rom[0], 8) != 0) {
    memcpy(ds18b20_rom[1], temp_rom, 8);
    ds18b20_count = 2;
  }
}

/**
 * @brief  Start temperature conversion on ALL DS18B20 sensors (SKIP ROM).
 */
static void DS18B20_StartConvertAll(void) {
  if (!OW_Reset())
    return;
  OW_WriteByte(DS18B20_CMD_SKIP_ROM);
  OW_WriteByte(DS18B20_CMD_CONVERT);
}

/**
 * @brief  Read temperature from a specific DS18B20 by ROM index.
 * @param  index  0 = primary, 1 = backup
 * @return Temperature in °C, or -127.0 on error
 */
static float DS18B20_ReadTemp(uint8_t index) {
  if (index >= ds18b20_count)
    return -127.0f;

  if (!OW_Reset())
    return -127.0f;

  OW_WriteByte(DS18B20_CMD_MATCH_ROM);
  for (int i = 0; i < 8; i++) {
    OW_WriteByte(ds18b20_rom[index][i]);
  }
  OW_WriteByte(DS18B20_CMD_READ_SCRATCH);

  uint8_t lsb = OW_ReadByte();
  uint8_t msb = OW_ReadByte();

  int16_t raw = (int16_t)((msb << 8) | lsb);
  return (float)raw / 16.0f; /* Default 12-bit resolution: 0.0625°C per LSB */
}

/* ═══════════════════════════════════════════════════════════════════════════
 */
/*                         BMP280 — I2C DRIVER                               */
/* ═══════════════════════════════════════════════════════════════════════════
 */

static uint8_t BMP280_ReadReg(uint8_t reg) {
  uint8_t val = 0;
  HAL_I2C_Mem_Read(&hi2c1, bmp280_i2c_addr, reg, I2C_MEMADD_SIZE_8BIT, &val, 1,
                   100);
  return val;
}

static void BMP280_WriteReg(uint8_t reg, uint8_t val) {
  HAL_I2C_Mem_Write(&hi2c1, bmp280_i2c_addr, reg, I2C_MEMADD_SIZE_8BIT, &val, 1,
                    100);
}

static HAL_StatusTypeDef BMP280_ReadBurst(uint8_t reg, uint8_t *buf,
                                          uint16_t len) {
  return HAL_I2C_Mem_Read(&hi2c1, bmp280_i2c_addr, reg, I2C_MEMADD_SIZE_8BIT,
                          buf, len, 100);
}

static uint8_t BMP280_Init(void) {
  const uint8_t candidate_addrs[2] = {BMP280_ADDR_PRIMARY,
                                      BMP280_ADDR_SECONDARY};
  uint8_t id = 0;
  uint8_t found = 0;
  char diag[128];

  /* Allow GY-BMP280 module LDO regulator and bus pull-ups to stabilize */
  HAL_Delay(100);

  /* Probe both 0x76 and 0x77 addresses with retries */
  for (int retry = 0; retry < 5 && !found; retry++) {
    for (int i = 0; i < 2; i++) {
      bmp280_i2c_addr = candidate_addrs[i];
      if (HAL_I2C_IsDeviceReady(&hi2c1, bmp280_i2c_addr, 2, 50) == HAL_OK) {
        id = BMP280_ReadReg(BMP280_REG_ID);
        snprintf(diag, sizeof(diag),
                 "[BMP280] ACK at 0x%02X, Reg 0xD0 ID = 0x%02X\r\n",
                 bmp280_i2c_addr >> 1, id);
        Debug_Print(diag);

        if (id == BMP280_CHIP_ID || id == 0x56 || id == 0x57) {
          found = 1;
          break;
        }
      }
    }
    if (!found) {
      HAL_Delay(20);
    }
  }

  if (!found) {
    bmp280_ok = 0;
    return 0;
  }

  bmp280_ok = 1;

  /* Soft reset sensor and wait for reboot */
  BMP280_WriteReg(BMP280_REG_RESET, BMP280_RESET_CMD);
  HAL_Delay(100);

  /* Wait until NVM trimming parameters finish copying to image registers */
  uint8_t poll_count = 50;
  while ((BMP280_ReadReg(BMP280_REG_STATUS) & 0x01) && --poll_count) {
    HAL_Delay(10);
  }

  /* Read factory calibration data (24 bytes) */
  BMP280_ReadCalibration();

  /* Print diagnostic calibration coefficients */
  {
    char diag[128];
    snprintf(diag, sizeof(diag),
             "[BMP280] Calib: T1=%u T2=%d T3=%d | P1=%u P2=%d P3=%d\r\n",
             bmp280_calib.dig_T1, bmp280_calib.dig_T2, bmp280_calib.dig_T3,
             bmp280_calib.dig_P1, bmp280_calib.dig_P2, bmp280_calib.dig_P3);
    Debug_Print(diag);
  }

  /* Config: standby 62.5ms, filter off */
  BMP280_WriteReg(BMP280_REG_CONFIG, 0x20);

  /* ctrl_meas: temp ×1, pressure ×1, normal mode (0x27) */
  BMP280_WriteReg(BMP280_REG_CTRL_MEAS, 0x27);
  HAL_Delay(50);

  /* Verify ctrl_meas */
  uint8_t ctrl_check = BMP280_ReadReg(BMP280_REG_CTRL_MEAS);
  if ((ctrl_check & 0x03) == 0) {
    /* If still in sleep, force normal mode */
    BMP280_WriteReg(BMP280_REG_CTRL_MEAS, 0x27);
  }

  return 1;
}

static void BMP280_ReadCalibration(void) {
  uint8_t buf[24];
  BMP280_ReadBurst(BMP280_REG_CALIB_00, buf, 24);

  bmp280_calib.dig_T1 = (uint16_t)(buf[1] << 8 | buf[0]);
  bmp280_calib.dig_T2 = (int16_t)(buf[3] << 8 | buf[2]);
  bmp280_calib.dig_T3 = (int16_t)(buf[5] << 8 | buf[4]);
  bmp280_calib.dig_P1 = (uint16_t)(buf[7] << 8 | buf[6]);
  bmp280_calib.dig_P2 = (int16_t)(buf[9] << 8 | buf[8]);
  bmp280_calib.dig_P3 = (int16_t)(buf[11] << 8 | buf[10]);
  bmp280_calib.dig_P4 = (int16_t)(buf[13] << 8 | buf[12]);
  bmp280_calib.dig_P5 = (int16_t)(buf[15] << 8 | buf[14]);
  bmp280_calib.dig_P6 = (int16_t)(buf[17] << 8 | buf[16]);
  bmp280_calib.dig_P7 = (int16_t)(buf[19] << 8 | buf[18]);
  bmp280_calib.dig_P8 = (int16_t)(buf[21] << 8 | buf[20]);
  bmp280_calib.dig_P9 = (int16_t)(buf[23] << 8 | buf[22]);
}

/**
 * @brief  Bosch BMP280 temperature compensation (returns °C × 100).
 *         Sets bmp280_t_fine for pressure compensation.
 */
static int32_t BMP280_CompensateTemp(int32_t adc_T) {
  int32_t var1, var2, T;
  var1 = ((((adc_T >> 3) - ((int32_t)bmp280_calib.dig_T1 << 1))) *
          ((int32_t)bmp280_calib.dig_T2)) >>
         11;
  var2 = (((((adc_T >> 4) - ((int32_t)bmp280_calib.dig_T1)) *
            ((adc_T >> 4) - ((int32_t)bmp280_calib.dig_T1))) >>
           12) *
          ((int32_t)bmp280_calib.dig_T3)) >>
         14;
  bmp280_t_fine = var1 + var2;
  T = (bmp280_t_fine * 5 + 128) >> 8;
  return T; /* °C × 100 */
}

static uint32_t BMP280_CompensatePress(int32_t adc_P) {
  int64_t var1, var2, p;
  var1 = ((int64_t)bmp280_t_fine) - 128000;
  var2 = var1 * var1 * (int64_t)bmp280_calib.dig_P6;
  var2 = var2 + ((var1 * (int64_t)bmp280_calib.dig_P5) << 17);
  var2 = var2 + (((int64_t)bmp280_calib.dig_P4) << 35);
  var1 = ((var1 * var1 * (int64_t)bmp280_calib.dig_P3) >> 8) +
         ((var1 * (int64_t)bmp280_calib.dig_P2) << 12);
  var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)bmp280_calib.dig_P1) >> 33;
  if (var1 == 0)
    return 0;
  p = 1048576 - adc_P;
  p = (((p << 31) - var2) * 3125) / var1;
  var1 = (((int64_t)bmp280_calib.dig_P9) * (p >> 13) * (p >> 13)) >> 25;
  var2 = (((int64_t)bmp280_calib.dig_P8) * p) >> 19;
  p = ((p + var1 + var2) >> 8) + (((int64_t)bmp280_calib.dig_P7) << 4);
  return (uint32_t)p; /* Pa × 256 */
}

/**
 * @brief  Read BMP280 data (temperature & barometric pressure).
 */
static void BMP280_ReadAll(float *temp, float *press) {
  if (!bmp280_ok) {
    *temp = 0.0f;
    *press = 0.0f;
    return;
  }

  uint8_t buf[6] = {0};
  if (BMP280_ReadBurst(BMP280_REG_DATA, buf, 6) != HAL_OK) {
    HAL_I2C_DeInit(&hi2c1);
    MX_I2C1_Init();
    return;
  }

  int32_t adc_P = (int32_t)((buf[0] << 12) | (buf[1] << 4) | (buf[2] >> 4));
  int32_t adc_T = (int32_t)((buf[3] << 12) | (buf[4] << 4) | (buf[5] >> 4));

  /* 0x80000 or 0 indicates measurement disabled or unready */
  if (adc_T == 0x80000 || adc_T == 0) {
    return;
  }

  /* Temperature MUST be calculated first (sets bmp280_t_fine) */
  int32_t t100 = BMP280_CompensateTemp(adc_T);
  uint32_t p256 = BMP280_CompensatePress(adc_P);

  *temp = (float)t100 / 100.0f;
  *press = (float)p256 / 256.0f / 100.0f; /* Pa → hPa */
}

/* ═══════════════════════════════════════════════════════════════════════════
 */
/*                    ANALOG SENSOR CONVERSION FUNCTIONS                     */
/* ═══════════════════════════════════════════════════════════════════════════
 */

/**
 * @brief  Read pH from the robu.in Analog pH Sensor Kit.
 *
 *         Formula: pH = PH_SCALE × V_sensor + PH_OFFSET
 *         where V_sensor is the reconstructed sensor output in volts
 *         (ADC_Read_Voltage accounts for the 2:1 divider on PA3).
 *
 *         Calibration:
 *          • Short the BNC connector → sensor should read ~pH 7.0
 *            If reading ≠ 7.0, set PH_OFFSET = 7.0 − displayed_value.
 *          • Dip in pH 4.00 buffer → verify reading ≈ 4.0 (confirms scale).
 */
static float Sensor_ReadPH(void) {
  /* ADC_Read_Voltage reconstructs V_sensor = ADC × (3.3/4095) × 2 (divider) */
  float v_sensor = ADC_Read_Voltage(ADC_CHANNEL_3); /* PA3 */

  float ph = PH_SCALE * v_sensor + PH_OFFSET;

  /* Clamp to physically valid pH range */
  if (ph < 0.0f)
    ph = 0.0f;
  if (ph > 14.0f)
    ph = 14.0f;

  char ph_diag[96];
  snprintf(ph_diag, sizeof(ph_diag),
           "[pH] V_sensor=%.3fV  pH=%.2f  (scale=%.1f offset=%.2f)\r\n",
           v_sensor, ph, PH_SCALE, PH_OFFSET);
  Debug_Print(ph_diag);

  return ph;
}

/**
 * @brief  Read TDS from SEN0244 with temperature compensation.
 *         DFRobot polynomial: TDS = (133.42×Vc³ - 255.86×Vc² + 857.39×Vc) × 0.5
 */
static float Sensor_ReadTDS(float water_temp) {
  float voltage = ADC_Read_Voltage(ADC_CHANNEL_13); /* PC3 */

  /* Temperature compensation */
  float comp_coeff = 1.0f + TDS_TEMP_COEFF * (water_temp - 25.0f);
  float comp_v = voltage / comp_coeff;

  /* Polynomial conversion */
  float tds = (TDS_COEFF_A * comp_v * comp_v * comp_v +
               TDS_COEFF_B * comp_v * comp_v + TDS_COEFF_C * comp_v) *
              TDS_FACTOR;
  if (tds < 0.0f)
    tds = 0.0f;
  if (tds > 1000.0f)
    tds = 1000.0f; /* SEN0244 max range */
  return tds;
}

/**
 * @brief  Read dissolved oxygen (Graylogix galvanic amperometric probe).
 *
 *         Method: single-point air calibration + temperature lookup table.
 *
 *         V_pin_mv  = raw ADC voltage at PA4 pin in millivolts
 *                     (post-divider, same scale as DO_CAL1_V)
 *         V_sat_mv  = DO_CAL1_V × DO_Table[T] / DO_Table[CAL_T]
 *                     (saturation voltage scaled to current temperature)
 *         DO (mg/L) = V_pin_mv × DO_Table[T] / V_sat_mv / 1000
 *
 *         Barometric correction: DO × (P_actual / 1013.25)
 *
 * @param  water_temp   Water temperature from DS18B20 (°C)
 * @param  pressure_hpa Barometric pressure from BMP280 (hPa)
 */
static float Sensor_ReadDO(float water_temp, float pressure_hpa) {
  /*
   * Read the raw ADC count and convert to mV at the ADC pin.
   * We deliberately do NOT use ADC_Read_Voltage() (which applies ×2 for the
   * divider) because DO_CAL1_V is also measured at the ADC pin — both sides
   * of the ratio are on the same post-divider scale, so they cancel.
   */
  uint32_t raw = ADC_Read_Channel(ADC_CHANNEL_4); /* PA4 */
  uint32_t v_pin_mv = (raw * 3300UL) / 4095UL;    /* mV at ADC pin */

  /* Clamp temperature index to lookup table bounds [0, 40] */
  uint8_t t = (uint8_t)water_temp;
  if (t > 40)
    t = 40;

  /*
   * Temperature-scaled saturation voltage.
   * Formula: V_sat = CAL1_V × DO_Table[T] / DO_Table[CAL1_T]
   * This accounts for the fact that the probe current (and thus output
   * voltage) scales proportionally with oxygen solubility.
   */
  uint32_t v_sat_mv = ((uint32_t)DO_CAL1_V * (uint32_t)DO_Table[t]) /
                      (uint32_t)DO_Table[DO_CAL1_T];

  float do_val = 0.0f;
  if (v_sat_mv > 0U) {
    do_val =
        ((float)v_pin_mv * (float)DO_Table[t]) / ((float)v_sat_mv * 1000.0f);
  }

  /* Barometric pressure compensation: DO × (P / P_std) */
  if (pressure_hpa > 0.0f) {
    do_val *= (pressure_hpa / 1013.25f);
  }

  /* Diagnostic output */
  char do_diag[96];
  snprintf(do_diag, sizeof(do_diag),
           "[DO] V_pin=%lumV V_sat=%lumV T=%d°C -> %.2f mg/L\r\n",
           (unsigned long)v_pin_mv, (unsigned long)v_sat_mv, t, do_val);
  Debug_Print(do_diag);

  if (do_val < 0.0f)
    do_val = 0.0f;
  if (do_val > 20.0f)
    do_val = 20.0f;
  return do_val;
}

/**
 * @brief  Read ORP from SEN0165.
 *         The SEN0165 signal board outputs a voltage centred around ~2.0V
 *         (at 0 mV ORP).  The relationship is linear:
 *           ORP_mV = (2.0 − V_board) × 1000
 *         where V_board is the 0–5V output of the signal board, reconstructed
 *         from the ADC reading through the 2:1 voltage divider.
 *
 *         This is the simplified form of DFRobot's original formula:
 *           ((30 × 5 × 1000) − (75 × ADC × 5 × 1000 / 1024)) / 75
 *         adapted for our 12-bit ADC + 3.3V VREF + 2:1 divider setup.
 */
static float Sensor_ReadORP(void) {
  /* Reconstruct the actual sensor-board output voltage (0–5V range) */
  float v_board =
      ADC_Read_Voltage(ADC_CHANNEL_6); /* PA6, includes ×2 divider */

  /* DFRobot SEN0165: ORP_mV = (2.0V midpoint − board voltage) × 1000 */
  float orp_mv = (2.0f - v_board) * 1000.0f - ORP_OFFSET;

  return orp_mv;
}

/* ═══════════════════════════════════════════════════════════════════════════
 */
/*                           JSON PACKET BUILDER                             */
/* ═══════════════════════════════════════════════════════════════════════════
 */

/**
 * @brief  Build a JSON payload from the sensor data struct.
 * @param  data     Pointer to sensor data
 * @param  buf      Output buffer
 * @param  buf_size Buffer size
 * @return Number of bytes written (excluding null terminator)
 */
static uint16_t WQM_BuildJSON(const WQM_SensorData_t *data, char *buf,
                              uint16_t buf_size) {
  int len = snprintf(
      buf, buf_size,
      "{"
      "\"id\":\"%s\","
      "\"ts\":%lu,"
      "\"pH\":%.2f,"
      "\"tds_ppm\":%.1f,"
      "\"do_mgl\":%.2f,"
      "\"orp_mv\":%.1f,"
      "\"temp_w1\":%.2f,"
      "\"temp_air\":%.2f,"
      "\"pressure_hpa\":%.2f,"
      "\"lat\":%.6f,"
      "\"lon\":%.6f,"
      "\"alt_m\":%.1f,"
      "\"sog_knots\":%.1f,"
      "\"cog_deg\":%.1f,"
      "\"gps_fix\":%u,"
      "\"sats\":%u,"
      "\"heading\":%.1f,"
      "\"pitch\":%.1f,"
      "\"roll\":%.1f"
      "}\r\n",
      DEVICE_ID, (unsigned long)HAL_GetTick(), data->ph,
      data->tds_ppm, data->do_mgl, data->orp_mv,
      data->water_temp_1, data->air_temp,
      data->pressure_hpa, data->latitude, data->longitude, data->altitude_m,
      data->speed_knots, data->course_deg, data->gps_fix, data->satellites,
      data->heading, data->pitch, data->roll);

  return (uint16_t)((len > 0) ? len : 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 */
/*                           DEBUG UART OUTPUT                               */
/* ═══════════════════════════════════════════════════════════════════════════
 */

void Debug_Print(const char *str) {
  HAL_UART_Transmit(&huart3, (uint8_t *)str, strlen(str), 500);
}

/* Manual peripheral init functions removed — CubeMX now generates them.
 * TIM6 prescaler and start are configured in USER CODE TIM6_Init 2 below.
 */

/* USER CODE END 0 */

/**
 * @brief  The application entry point.
 * @retval int
 */
int main(void) {

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick.
   */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART3_UART_Init();
  MX_USB_OTG_FS_PCD_Init();
  MX_ADC1_Init();
  MX_I2C1_Init();
  MX_TIM6_Init();
  MX_UART4_Init();
  /* USER CODE BEGIN 2 */
  DWT_Init(); /* Enable ARM Cortex-M7 cycle counter for microsecond delays */

  /* ── Boot message ─────────────────────────────────────────────────────────
   */
  Debug_Print("\r\n========================================\r\n");
  Debug_Print("  WQM Sensor Suite — NUCLEO-F722ZE\r\n");
  Debug_Print("========================================\r\n");

  /* ── I2C Bus Scanner ──────────────────────────────────────────────────────
   */
  Debug_Print("[I2C1] Scanning bus for devices...\r\n");
  uint8_t i2c_devs = 0;
  for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
    if (HAL_I2C_IsDeviceReady(&hi2c1, (uint16_t)(addr << 1), 2, 20) == HAL_OK) {
      char scan_msg[48];
      snprintf(scan_msg, sizeof(scan_msg), "[I2C1] ACK at 0x%02X\r\n", addr);
      Debug_Print(scan_msg);
      i2c_devs++;
    }
  }
  if (i2c_devs == 0) {
    Debug_Print("[I2C1] No devices found on bus!\r\n");
  }

  /* ── Initialize BMP280 ────────────────────────────────────────────────────
   */
  if (BMP280_Init()) {
    char msg[64];
    snprintf(msg, sizeof(msg),
             "[OK]  BMP280 initialized (ID=0x%02X at 0x%02X)\r\n",
             BMP280_CHIP_ID, bmp280_i2c_addr >> 1);
    Debug_Print(msg);
  } else {
    Debug_Print("[ERR] BMP280 not found! Check I2C wiring.\r\n");
  }

  /* ── Discover DS18B20 sensors ─────────────────────────────────────────────
   */
  DS18B20_SearchROM();
  {
    char msg[64];
    snprintf(msg, sizeof(msg), "[OK]  DS18B20 sensors found: %u\r\n",
             ds18b20_count);
    Debug_Print(msg);
  }

  Debug_Print("[OK]  All peripherals initialized.\r\n");

  /* ── Initialize GPS ───────────────────────────────────────────────────────
   */
  if (GPS_Init(&huart4) == 0) {
    Debug_Print("[OK]  GPS UART initialized.\r\n");
  } else {
    Debug_Print("[ERR] GPS UART failed to initialize!\r\n");
  }

  /* ── Initialize BNO085 ────────────────────────────────────────────────────
   */
  if (BNO085_Init(&hi2c1, BNO085_REPORT_INTERVAL_MS) == 0) {
    bno085_ok = 1;
    char msg[64];
    snprintf(msg, sizeof(msg), "[OK]  BNO085 initialized (at 0x%02X).\r\n",
             bno085_i2c_addr >> 1);
    Debug_Print(msg);
  } else {
    bno085_ok = 0;
    Debug_Print("[ERR] BNO085 failed to initialize! Check I2C wiring.\r\n");
    /* Reset I2C1 bus so NACK from absent BNO085 doesn't foul BMP280 reads */
    HAL_I2C_DeInit(&hi2c1);
    MX_I2C1_Init();
  }

  Debug_Print("========================================\r\n\r\n");

  HAL_Delay(1000); /* Let sensors stabilize */

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1) {
    sample_counter++;
    uint32_t loop_start = HAL_GetTick();

    /* ═══════════════════════════════════════════════════════════════════════
     */
    /*  STOP-AND-SAMPLE SEQUENCE                                              */
    /*  1. BMP280  2. DS18B20  3. TDS  4. ORP  5. pH  6. DO                   */
    /* ═══════════════════════════════════════════════════════════════════════
     */

    /* ── Step 1: Read BMP280 (air temp, barometric pressure) ─────────────── */
    BMP280_ReadAll(&sensor_data.air_temp, &sensor_data.pressure_hpa);

    /* ── Step 2: Start DS18B20 conversion and read temperatures ────────── */
    DS18B20_StartConvertAll();
    /* Keep BNO085 FIFO drained during 750ms DS18B20 conversion */
    uint32_t ds_start = HAL_GetTick();
    while (HAL_GetTick() - ds_start < 750) {
      if (bno085_ok) {
        BNO085_Read(&hi2c1, &imu_orient);
      }
      HAL_Delay(50);
    }
    sensor_data.water_temp_1 =
        DS18B20_ReadTemp(0);         /* Single sensor — index 0 */

    /* Use primary temp for compensation; fall back to 25°C if probe fails */
    float comp_temp = sensor_data.water_temp_1;
    if (comp_temp < -50.0f || comp_temp > 125.0f) {
      comp_temp = 25.0f; /* Default if probe disconnected */
    }

    /* ── Step 3: Read fast analog sensor (TDS) ───────────────────────────── */
    sensor_data.tds_ppm = Sensor_ReadTDS(comp_temp);

    /* ── Step 4: Read ORP (~20s settling in real deployment, instant here)  */
    sensor_data.orp_mv = Sensor_ReadORP();

    /* ── Step 5: Read pH ────────────────────────────────────────────────────
     */
    sensor_data.ph = Sensor_ReadPH();

    /* ── Step 6: Read DO last (slowest — maximum settling time by now) ──── */
    sensor_data.do_mgl = Sensor_ReadDO(comp_temp, sensor_data.pressure_hpa);

    /* ── Step 7: Read IMU (BNO085) ───────────────────────────────────────── */
    if (bno085_ok) {
      BNO085_Read(&hi2c1, &imu_orient);
      if (imu_orient.valid) {
        sensor_data.heading = imu_orient.yaw;
        sensor_data.pitch = imu_orient.pitch;
        sensor_data.roll = imu_orient.roll;
        char imu_diag[80];
        snprintf(
            imu_diag, sizeof(imu_diag),
            "[BNO085] Yaw: %.1f deg | Pitch: %.1f deg | Roll: %.1f deg\r\n",
            imu_orient.yaw, imu_orient.pitch, imu_orient.roll);
        Debug_Print(imu_diag);
      }
    }

    /* ── Step 8: Copy latest GPS data ───────────────────────────────────────
     */
    GPS_GetData(&gps_data);
    sensor_data.latitude = gps_data.latitude;
    sensor_data.longitude = gps_data.longitude;
    sensor_data.altitude_m = gps_data.altitude_m;
    sensor_data.speed_knots = gps_data.speed_knots;
    sensor_data.course_deg = gps_data.course_deg;
    sensor_data.gps_fix = gps_data.fix_quality;
    sensor_data.satellites = gps_data.satellites;

    /* ═══════════════════════════════════════════════════════════════════════
     */
    /*  BUILD JSON PACKET & TRANSMIT VIA USART3 (debug; LoRa later)         */
    /* ═══════════════════════════════════════════════════════════════════════
     */

    uint16_t json_len = WQM_BuildJSON(&sensor_data, json_buf, JSON_BUF_SIZE);
    if (json_len > 0) {
      Debug_Print(json_buf);
    }

    /* Status LED blink — green LD1 on PB0 */
    HAL_GPIO_TogglePin(GPIOB, LD1_Pin);

    /* Wait for remainder of sample interval while keeping IMU serviced and FIFO
     * drained */
    while ((HAL_GetTick() - loop_start) < SAMPLE_INTERVAL_MS) {
      if (bno085_ok) {
        BNO085_Read(&hi2c1, &imu_orient);
      }
      HAL_Delay(20);
    }

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
 * @brief System Clock Configuration
 * @retval None
 */
void SystemClock_Config(void) {
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure LSE Drive Capability
   */
  HAL_PWR_EnableBkUpAccess();

  /** Configure the main internal regulator output voltage
   */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
   * in the RCC_OscInitTypeDef structure.
   */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_BYPASS;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 216;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
    Error_Handler();
  }

  /** Activate the Over-Drive mode
   */
  if (HAL_PWREx_EnableOverDrive() != HAL_OK) {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
   */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                                RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_7) != HAL_OK) {
    Error_Handler();
  }
}

/**
 * @brief ADC1 Initialization Function
 * @param None
 * @retval None
 */
static void MX_ADC1_Init(void) {

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Configure the global features of the ADC (Clock, Resolution, Data
   * Alignment and number of conversion)
   */
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  if (HAL_ADC_Init(&hadc1) != HAL_OK) {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in
   * the sequencer and its sample time.
   */
  sConfig.Channel = ADC_CHANNEL_3;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_3CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK) {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */
}

/**
 * @brief I2C1 Initialization Function
 * @param None
 * @retval None
 */
static void MX_I2C1_Init(void) {

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.Timing = 0x20404768;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK) {
    Error_Handler();
  }

  /** Configure Analogue filter
   */
  if (HAL_I2CEx_ConfigAnalogFilter(&hi2c1, I2C_ANALOGFILTER_ENABLE) != HAL_OK) {
    Error_Handler();
  }

  /** Configure Digital filter
   */
  if (HAL_I2CEx_ConfigDigitalFilter(&hi2c1, 0) != HAL_OK) {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */

  /* USER CODE END I2C1_Init 2 */
}

/**
 * @brief TIM6 Initialization Function
 * @param None
 * @retval None
 */
static void MX_TIM6_Init(void) {

  /* USER CODE BEGIN TIM6_Init 0 */

  /* USER CODE END TIM6_Init 0 */

  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM6_Init 1 */

  /* USER CODE END TIM6_Init 1 */
  htim6.Instance = TIM6;
  htim6.Init.Prescaler = 0;
  htim6.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim6.Init.Period = 65535;
  htim6.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim6) != HAL_OK) {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim6, &sMasterConfig) != HAL_OK) {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM6_Init 2 */
  __HAL_TIM_SET_PRESCALER(&htim6, 107);
  HAL_TIM_Base_Start(&htim6); /* Start free-running counter */
  /* USER CODE END TIM6_Init 2 */
}

/**
 * @brief UART4 Initialization Function
 * @param None
 * @retval None
 */
static void MX_UART4_Init(void) {

  /* USER CODE BEGIN UART4_Init 0 */

  /* USER CODE END UART4_Init 0 */

  /* USER CODE BEGIN UART4_Init 1 */

  /* USER CODE END UART4_Init 1 */
  huart4.Instance = UART4;
  huart4.Init.BaudRate = 115200;
  huart4.Init.WordLength = UART_WORDLENGTH_8B;
  huart4.Init.StopBits = UART_STOPBITS_1;
  huart4.Init.Parity = UART_PARITY_NONE;
  huart4.Init.Mode = UART_MODE_TX_RX;
  huart4.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart4.Init.OverSampling = UART_OVERSAMPLING_16;
  huart4.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart4.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart4) != HAL_OK) {
    Error_Handler();
  }
  /* USER CODE BEGIN UART4_Init 2 */

  /* USER CODE END UART4_Init 2 */
}

/**
 * @brief USART3 Initialization Function
 * @param None
 * @retval None
 */
static void MX_USART3_UART_Init(void) {

  /* USER CODE BEGIN USART3_Init 0 */

  /* USER CODE END USART3_Init 0 */

  /* USER CODE BEGIN USART3_Init 1 */

  /* USER CODE END USART3_Init 1 */
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 115200;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  huart3.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart3.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart3) != HAL_OK) {
    Error_Handler();
  }
  /* USER CODE BEGIN USART3_Init 2 */

  /* USER CODE END USART3_Init 2 */
}

/**
 * @brief USB_OTG_FS Initialization Function
 * @param None
 * @retval None
 */
static void MX_USB_OTG_FS_PCD_Init(void) {

  /* USER CODE BEGIN USB_OTG_FS_Init 0 */

  /* USER CODE END USB_OTG_FS_Init 0 */

  /* USER CODE BEGIN USB_OTG_FS_Init 1 */

  /* USER CODE END USB_OTG_FS_Init 1 */
  hpcd_USB_OTG_FS.Instance = USB_OTG_FS;
  hpcd_USB_OTG_FS.Init.dev_endpoints = 6;
  hpcd_USB_OTG_FS.Init.speed = PCD_SPEED_FULL;
  hpcd_USB_OTG_FS.Init.dma_enable = DISABLE;
  hpcd_USB_OTG_FS.Init.phy_itface = PCD_PHY_EMBEDDED;
  hpcd_USB_OTG_FS.Init.Sof_enable = ENABLE;
  hpcd_USB_OTG_FS.Init.low_power_enable = DISABLE;
  hpcd_USB_OTG_FS.Init.lpm_enable = DISABLE;
  hpcd_USB_OTG_FS.Init.battery_charging_enable = ENABLE;
  hpcd_USB_OTG_FS.Init.vbus_sensing_enable = ENABLE;
  hpcd_USB_OTG_FS.Init.use_dedicated_ep1 = DISABLE;
  if (HAL_PCD_Init(&hpcd_USB_OTG_FS) != HAL_OK) {
    Error_Handler();
  }
  /* USER CODE BEGIN USB_OTG_FS_Init 2 */

  /* USER CODE END USB_OTG_FS_Init 2 */
}

/**
 * @brief GPIO Initialization Function
 * @param None
 * @retval None
 */
static void MX_GPIO_Init(void) {
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOE, DS18B20_DQ_Pin | DS18B20_DQE6_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, LD1_Pin | LD3_Pin | LD2_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(USB_PowerSwitchOn_GPIO_Port, USB_PowerSwitchOn_Pin,
                    GPIO_PIN_RESET);

  /*Configure GPIO pins : DS18B20_DQ_Pin DS18B20_DQE6_Pin */
  GPIO_InitStruct.Pin = DS18B20_DQ_Pin | DS18B20_DQE6_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

  /*Configure GPIO pin : PE5 */
  GPIO_InitStruct.Pin = GPIO_PIN_5;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

  /*Configure GPIO pin : USER_Btn_Pin */
  GPIO_InitStruct.Pin = USER_Btn_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(USER_Btn_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : LD1_Pin LD3_Pin LD2_Pin */
  GPIO_InitStruct.Pin = LD1_Pin | LD3_Pin | LD2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : USB_PowerSwitchOn_Pin */
  GPIO_InitStruct.Pin = USB_PowerSwitchOn_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(USB_PowerSwitchOn_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : USB_OverCurrent_Pin */
  GPIO_InitStruct.Pin = USB_OverCurrent_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(USB_OverCurrent_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : PC6 PC7 */
  GPIO_InitStruct.Pin = GPIO_PIN_6 | GPIO_PIN_7;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF8_USART6;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/**
 * @brief UART RX Complete Callback for GPS
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
  if (huart->Instance == UART4) {
    extern void GPS_UART_IRQCallback(void);
    GPS_UART_IRQCallback();
  }
}

/* USER CODE END 4 */

/* MPU Configuration */

void MPU_Config(void) {
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region and the memory to be protected
   */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}

/**
 * @brief  This function is executed in case of error occurrence.
 * @retval None
 */
void Error_Handler(void) {
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1) {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
 * @brief  Reports the name of the source file and the source line number
 *         where the assert_param error has occurred.
 * @param  file: pointer to the source file name
 * @param  line: assert_param error line source number
 * @retval None
 */
void assert_failed(uint8_t *file, uint32_t line) {
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line
     number, ex: printf("Wrong parameters value: file %s on line %d\r\n", file,
     line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
