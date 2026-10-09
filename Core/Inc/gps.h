/**
 ******************************************************************************
 * @file    gps.h
 * @brief   GPS NMEA parser driver for STM32 HAL (UART RX).
 *
 *          Designed for standard GPS modules (e.g. NEO-6M, NEO-7M, BN-220)
 *          that output NMEA-0183 sentences at 9600 baud over UART TX/RX.
 *
 * @note    Uses UART4 on PC10 (TX) / PC11 (RX).
 ******************************************************************************
 */

#ifndef GPS_H
#define GPS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include <stdint.h>

/* Exported types ------------------------------------------------------------*/

/**
 * @brief  Parsed GPS fix data from GGA + RMC sentences.
 */
typedef struct {
  /* Position */
  float latitude;       /**< Decimal degrees, north positive             */
  float longitude;      /**< Decimal degrees, east positive              */
  float altitude_m;     /**< Altitude above MSL in metres  (from GGA)   */

  /* Motion */
  float speed_knots;    /**< Speed over ground in knots    (from RMC)   */
  float course_deg;     /**< Course over ground in degrees (from RMC)   */

  /* Quality */
  uint8_t fix_quality;  /**< 0=invalid, 1=GPS, 2=DGPS     (from GGA)   */
  uint8_t satellites;   /**< Number of satellites in use   (from GGA)   */
  float hdop;           /**< Horizontal dilution of precision (from GGA)*/

  /* Time (UTC) */
  uint8_t hour;
  uint8_t minute;
  uint8_t second;

  /* Date (UTC) */
  uint8_t day;
  uint8_t month;
  uint8_t year;         /**< Last two digits (e.g. 26 for 2026)         */

  /* Status flags */
  uint8_t valid;        /**< 1 = valid fix (RMC status 'A'), 0 = no fix */
} GPS_Data_t;

/* Exported constants --------------------------------------------------------*/

/** UART baud rate for GPS module */
#define GPS_BAUD_RATE       9600

/** Maximum NMEA sentence length (spec says 82, leave margin) */
#define GPS_NMEA_MAX_LEN    100

/* Exported functions --------------------------------------------------------*/

/**
 * @brief  Initialise the GPS UART peripheral.
 *         Configures UART4 at 9600 baud and starts interrupt-based
 *         byte-by-byte reception.
 *
 * @param  huart  Pointer to the UART handle (must point to UART4).
 * @retval 0 = success, non-zero = error.
 */
uint8_t GPS_Init(UART_HandleTypeDef *huart);

/**
 * @brief  Feed a raw byte into the NMEA parser.
 *         Call this from the UART RX complete callback for each byte
 *         received from the GPS module.
 *
 * @param  byte  The received byte.
 */
void GPS_FeedByte(uint8_t byte);

/**
 * @brief  Get the latest parsed GPS data.
 *         Thread-safe: copies the internal data struct atomically.
 *
 * @param  data  Pointer to the output GPS data struct.
 */
void GPS_GetData(GPS_Data_t *data);

/**
 * @brief  Check if the GPS has a valid fix.
 * @retval 1 = valid fix, 0 = no fix.
 */
uint8_t GPS_HasFix(void);

#ifdef __cplusplus
}
#endif

#endif /* GPS_H */
