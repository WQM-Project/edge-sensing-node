/**
 ******************************************************************************
 * @file    bno085.h
 * @brief   BNO085 9-DOF IMU driver (I2C mode) for STM32 HAL.
 *
 *          SmartElex 9-DOF Orientation IMU Fusion Breakout — BNO085
 *          Uses Hillcrest SH-2 protocol over I2C to retrieve fused
 *          orientation data (rotation vector → Euler angles + heading).
 *
 * @note    Shares I2C1 bus with BME280 (0x76). BNO085 at 0x4A.
 ******************************************************************************
 */

#ifndef BNO085_H
#define BNO085_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include <stdint.h>

/* Exported types ------------------------------------------------------------*/

/**
 * @brief  Fused orientation data from BNO085 ARVR-stabilised rotation vector.
 */
typedef struct {
  float yaw;       /**< Heading / yaw   in degrees (0–360, north = 0)   */
  float pitch;     /**< Pitch           in degrees (-90 to +90)         */
  float roll;      /**< Roll            in degrees (-180 to +180)       */
  float accuracy;  /**< Heading accuracy estimate in radians            */
  uint8_t valid;   /**< 1 = data is fresh / sensor responding, 0 = stale */
} BNO085_Orientation_t;

/* Exported constants --------------------------------------------------------*/

/** I2C 7-bit address left-shifted for HAL (SA0 = LOW → 0x4A) */
#define BNO085_I2C_ADDR         (0x4A << 1)

/** Alternative address when SA0 = HIGH */
#define BNO085_I2C_ADDR_ALT     (0x4B << 1)

/* Exported functions --------------------------------------------------------*/

/**
 * @brief  Initialise the BNO085 sensor over I2C.
 *         Resets the chip, waits for advertisement, and enables the
 *         Game Rotation Vector report at the requested interval.
 *
 * @param  hi2c          Pointer to the I2C handle (must already be init'd).
 * @param  report_ms     Desired report interval in milliseconds (e.g. 50).
 * @retval 0 = success, non-zero = error code.
 */
uint8_t BNO085_Init(I2C_HandleTypeDef *hi2c, uint16_t report_ms);

/**
 * @brief  Poll the BNO085 for new orientation data.
 *         Should be called at least as often as the report interval.
 *         Parses the SH-2 SHTP cargo and fills the orientation struct.
 *
 * @param  hi2c   Pointer to the I2C handle.
 * @param  orient Pointer to output orientation struct.
 * @retval 0 = new data available, 1 = no new data, 2 = comm error.
 */
uint8_t BNO085_Read(I2C_HandleTypeDef *hi2c, BNO085_Orientation_t *orient);

/**
 * @brief  Soft-reset the BNO085.
 * @param  hi2c  Pointer to the I2C handle.
 */
void BNO085_Reset(I2C_HandleTypeDef *hi2c);

#ifdef __cplusplus
}
#endif

#endif /* BNO085_H */
