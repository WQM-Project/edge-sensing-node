/**
 ******************************************************************************
 * @file    bno085.c
 * @brief   BNO085 9-DOF IMU driver — SH-2 / SHTP over I2C.
 *
 *          Implements the Hillcrest Sensor Hub Transport Protocol (SHTP)
 *          over I2C to communicate with the BNO085.  Only the Game Rotation
 *          Vector report (0x08) is requested; quaternion output is converted
 *          to Euler angles (yaw / pitch / roll) for the telemetry packet.
 *
 *          Reference: BNO080/085 datasheet §5 (SHTP), §6 (SH-2 protocol).
 *
 * @note    The BNO085 sits on the same I2C1 bus as the BME280 but at
 *          address 0x4A (vs 0x76).  No bus contention.
 ******************************************************************************
 */

/* Includes ------------------------------------------------------------------*/
#include "bno085.h"
#include <math.h>
#include <string.h>

/* Private defines -----------------------------------------------------------*/

/* ── SHTP channel numbers ───────────────────────────────────────────────── */
#define SHTP_CHAN_COMMAND       0  /**< Device → host: errors / advertisements  */
#define SHTP_CHAN_EXECUTABLE    1  /**< Host → device: reset / DFU              */
#define SHTP_CHAN_CONTROL       2  /**< Host ↔ device: SH-2 control             */
#define SHTP_CHAN_INPUT_REPORT  3  /**< Device → host: sensor reports            */

/* ── SH-2 report IDs ───────────────────────────────────────────────────── */
#define SH2_GAME_ROTATION_VECTOR   0x08
#define SH2_ARVR_ROTATION_VECTOR   0x28
#define SH2_SET_FEATURE_CMD        0xFD
#define SH2_GET_FEATURE_RESP       0xFC
#define SH2_PRODUCT_ID_REQ         0xF9
#define SH2_PRODUCT_ID_RESP        0xF8

/* ── Command / reset ────────────────────────────────────────────────────── */
#define SH2_CMD_RESET              0x01

/* ── SHTP header size ───────────────────────────────────────────────────── */
#define SHTP_HEADER_SIZE           4

/* ── Maximum SHTP cargo we expect (rotation vector report is tiny) ────── */
#define SHTP_MAX_CARGO             128
#define SHTP_MAX_PACKET            (SHTP_HEADER_SIZE + SHTP_MAX_CARGO)

/* ── I2C timeout ────────────────────────────────────────────────────────── */
#define BNO085_I2C_TIMEOUT         100  /* ms */

/* ── Q-point scale factors for BNO085 fixed-point fields ────────────────── */
#define Q14_SCALE  (1.0f / 16384.0f)   /* Rotation vector quaternion Q14  */
#define Q12_SCALE  (1.0f / 4096.0f)    /* Accuracy field Q12              */

/* ── Maths ──────────────────────────────────────────────────────────────── */
#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif
#define RAD_TO_DEG  (180.0f / M_PI)

/* Private variables ---------------------------------------------------------*/

/** Sequence numbers per SHTP channel (required by protocol) */
static uint8_t shtp_seq[6] = {0};

/** Shared I/O buffer for SHTP packets */
static uint8_t shtp_buf[SHTP_MAX_PACKET];

/* Private function prototypes -----------------------------------------------*/

static HAL_StatusTypeDef shtp_send(I2C_HandleTypeDef *hi2c,
                                   uint8_t channel,
                                   const uint8_t *payload,
                                   uint16_t payload_len);

static int16_t shtp_receive(I2C_HandleTypeDef *hi2c,
                            uint8_t *channel_out);

static void shtp_set_feature(I2C_HandleTypeDef *hi2c,
                             uint8_t report_id,
                             uint32_t interval_us);

static void quat_to_euler(float qi, float qj, float qk, float qr,
                           float *yaw, float *pitch, float *roll);

/* ═══════════════════════════════════════════════════════════════════════════ */
/*                          SHTP TRANSPORT LAYER                              */
/* ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Send an SHTP packet (header + payload) to the BNO085.
 */
static HAL_StatusTypeDef shtp_send(I2C_HandleTypeDef *hi2c,
                                   uint8_t channel,
                                   const uint8_t *payload,
                                   uint16_t payload_len) {
  uint16_t total = SHTP_HEADER_SIZE + payload_len;
  if (total > SHTP_MAX_PACKET) return HAL_ERROR;

  /* SHTP header: 2-byte little-endian length, channel, sequence number */
  shtp_buf[0] = (uint8_t)(total & 0xFF);
  shtp_buf[1] = (uint8_t)((total >> 8) & 0x7F);  /* bit 15 = 0 (no continuation) */
  shtp_buf[2] = channel;
  shtp_buf[3] = shtp_seq[channel]++;

  if (payload_len > 0) {
    memcpy(&shtp_buf[SHTP_HEADER_SIZE], payload, payload_len);
  }

  return HAL_I2C_Master_Transmit(hi2c, BNO085_I2C_ADDR, shtp_buf, total,
                                  BNO085_I2C_TIMEOUT);
}

/**
 * @brief  Receive an SHTP packet from the BNO085.
 * @param  hi2c         I2C handle.
 * @param  channel_out  Receives the SHTP channel number of the packet.
 * @return Cargo length (>0), 0 = empty/no data, -1 = I2C error.
 *
 * The SHTP header occupies the first 4 bytes of shtp_buf.
 * Cargo starts at shtp_buf[SHTP_HEADER_SIZE].
 */
static int16_t shtp_receive(I2C_HandleTypeDef *hi2c, uint8_t *channel_out) {
  /* Read the 4-byte SHTP header first to learn the total length */
  HAL_StatusTypeDef st = HAL_I2C_Master_Receive(hi2c, BNO085_I2C_ADDR,
                                                 shtp_buf, SHTP_HEADER_SIZE,
                                                 BNO085_I2C_TIMEOUT);
  if (st != HAL_OK) return -1;

  /* Parse length (little-endian, bit 15 = continuation flag) */
  uint16_t pkt_len = (uint16_t)(shtp_buf[0] | ((shtp_buf[1] & 0x7F) << 8));

  if (pkt_len <= SHTP_HEADER_SIZE || pkt_len == 0xFFFF) {
    /* No data waiting (BNO085 returns 0xFFFF when nothing is available) */
    return 0;
  }

  /* Clamp to buffer size */
  if (pkt_len > SHTP_MAX_PACKET) pkt_len = SHTP_MAX_PACKET;

  /* Now read the full packet (header + cargo) */
  st = HAL_I2C_Master_Receive(hi2c, BNO085_I2C_ADDR, shtp_buf, pkt_len,
                               BNO085_I2C_TIMEOUT);
  if (st != HAL_OK) return -1;

  *channel_out = shtp_buf[2];

  /* Update our copy of the device's sequence number */
  shtp_seq[*channel_out] = shtp_buf[3];

  return (int16_t)(pkt_len - SHTP_HEADER_SIZE);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*                          SH-2 FEATURE CONTROL                              */
/* ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Send a Set Feature Command to enable a sensor report.
 *
 * @param  hi2c         I2C handle.
 * @param  report_id    SH-2 sensor report ID to enable.
 * @param  interval_us  Report interval in microseconds.
 */
static void shtp_set_feature(I2C_HandleTypeDef *hi2c,
                             uint8_t report_id,
                             uint32_t interval_us) {
  uint8_t cmd[17];
  memset(cmd, 0, sizeof(cmd));

  cmd[0] = SH2_SET_FEATURE_CMD;
  cmd[1] = report_id;
  /* bytes 2-4: sensor-specific config (0 = defaults) */
  /* bytes 5-8: report interval in µs (little-endian) */
  cmd[5] = (uint8_t)(interval_us & 0xFF);
  cmd[6] = (uint8_t)((interval_us >> 8) & 0xFF);
  cmd[7] = (uint8_t)((interval_us >> 16) & 0xFF);
  cmd[8] = (uint8_t)((interval_us >> 24) & 0xFF);
  /* bytes 9-12: batch interval (0 = no batching) */
  /* bytes 13-16: sensor-specific config word (0) */

  shtp_send(hi2c, SHTP_CHAN_CONTROL, cmd, sizeof(cmd));
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*                     QUATERNION → EULER CONVERSION                          */
/* ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Convert unit quaternion (i, j, k, real) to aerospace Euler angles.
 *
 * Convention: ZYX (yaw → pitch → roll), right-hand, NED-ish.
 *   - yaw   = heading in [0, 360) degrees
 *   - pitch  = [-90, +90]
 *   - roll   = [-180, +180]
 */
static void quat_to_euler(float qi, float qj, float qk, float qr,
                           float *yaw, float *pitch, float *roll) {
  /* Roll (x-axis rotation) */
  float sinr_cosp = 2.0f * (qr * qi + qj * qk);
  float cosr_cosp = 1.0f - 2.0f * (qi * qi + qj * qj);
  *roll = atan2f(sinr_cosp, cosr_cosp) * RAD_TO_DEG;

  /* Pitch (y-axis rotation) — clamp to avoid NaN in asin */
  float sinp = 2.0f * (qr * qj - qk * qi);
  if (sinp >= 1.0f)
    *pitch = 90.0f;
  else if (sinp <= -1.0f)
    *pitch = -90.0f;
  else
    *pitch = asinf(sinp) * RAD_TO_DEG;

  /* Yaw (z-axis rotation) → heading */
  float siny_cosp = 2.0f * (qr * qk + qi * qj);
  float cosy_cosp = 1.0f - 2.0f * (qj * qj + qk * qk);
  *yaw = atan2f(siny_cosp, cosy_cosp) * RAD_TO_DEG;

  /* Normalise yaw to [0, 360) */
  if (*yaw < 0.0f) *yaw += 360.0f;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*                           PUBLIC API                                       */
/* ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Soft-reset the BNO085 using the executable channel.
 */
void BNO085_Reset(I2C_HandleTypeDef *hi2c) {
  uint8_t reset_cmd = SH2_CMD_RESET;
  shtp_send(hi2c, SHTP_CHAN_EXECUTABLE, &reset_cmd, 1);
}

/**
 * @brief  Initialise BNO085: reset, drain advertisements, enable Game
 *         Rotation Vector at the given report interval.
 *
 * @return 0 on success, 1 if chip did not respond.
 */
uint8_t BNO085_Init(I2C_HandleTypeDef *hi2c, uint16_t report_ms) {
  /* Clear sequence counters */
  memset(shtp_seq, 0, sizeof(shtp_seq));

  /* Soft-reset */
  BNO085_Reset(hi2c);
  HAL_Delay(300);  /* BNO085 needs ~300 ms after reset to boot */

  /* Drain any pending packets (advertisements, unsolicited reports) */
  uint8_t chan;
  for (int drain = 0; drain < 10; drain++) {
    int16_t len = shtp_receive(hi2c, &chan);
    if (len <= 0) break;
    HAL_Delay(10);
  }

  /* Verify communication: try to read something from the sensor */
  /* Send Product ID request */
  uint8_t pid_req[2] = {SH2_PRODUCT_ID_REQ, 0};
  shtp_send(hi2c, SHTP_CHAN_CONTROL, pid_req, sizeof(pid_req));
  HAL_Delay(50);

  /* Look for Product ID response */
  uint8_t got_response = 0;
  for (int tries = 0; tries < 5; tries++) {
    int16_t len = shtp_receive(hi2c, &chan);
    if (len > 0 && chan == SHTP_CHAN_CONTROL &&
        shtp_buf[SHTP_HEADER_SIZE] == SH2_PRODUCT_ID_RESP) {
      got_response = 1;
      break;
    }
    HAL_Delay(20);
  }

  if (!got_response) return 1;  /* Sensor not responding */

  /* Enable Game Rotation Vector report */
  uint32_t interval_us = (uint32_t)report_ms * 1000U;
  shtp_set_feature(hi2c, SH2_GAME_ROTATION_VECTOR, interval_us);
  HAL_Delay(50);

  /* Drain the feature response */
  for (int drain = 0; drain < 5; drain++) {
    int16_t len = shtp_receive(hi2c, &chan);
    if (len <= 0) break;
    HAL_Delay(10);
  }

  return 0;
}

/**
 * @brief  Poll the BNO085 for a new Game Rotation Vector report and
 *         convert the quaternion to Euler angles.
 *
 * @return 0 = new data written to *orient.
 *         1 = no new data (call again later).
 *         2 = I2C communication error.
 */
uint8_t BNO085_Read(I2C_HandleTypeDef *hi2c, BNO085_Orientation_t *orient) {
  uint8_t chan;
  int16_t cargo_len = shtp_receive(hi2c, &chan);

  if (cargo_len < 0) return 2;  /* I2C error */
  if (cargo_len == 0) return 1; /* No data   */

  /* We only care about Input Report channel (3) */
  if (chan != SHTP_CHAN_INPUT_REPORT) return 1;

  uint8_t *cargo = &shtp_buf[SHTP_HEADER_SIZE];

  /*
   * Input Report header:
   *   [0]    = Report ID
   *   [1]    = Sequence number
   *   [2]    = Status & delay
   *   [3]    = Delay (LSB)
   *   [4-5]  = i (Q14 signed)
   *   [6-7]  = j (Q14 signed)
   *   [8-9]  = k (Q14 signed)
   *   [10-11]= real (Q14 signed)
   *   [12-13]= accuracy (Q12 signed, radians) — for ARVR; game RV has 0
   */
  uint8_t report_id = cargo[0];

  if (report_id != SH2_GAME_ROTATION_VECTOR &&
      report_id != SH2_ARVR_ROTATION_VECTOR) {
    return 1;  /* Not the report we want */
  }

  if (cargo_len < 14) return 1;  /* Truncated */

  /* Parse Q14 fixed-point quaternion components */
  int16_t qi_raw = (int16_t)(cargo[4]  | (cargo[5]  << 8));
  int16_t qj_raw = (int16_t)(cargo[6]  | (cargo[7]  << 8));
  int16_t qk_raw = (int16_t)(cargo[8]  | (cargo[9]  << 8));
  int16_t qr_raw = (int16_t)(cargo[10] | (cargo[11] << 8));

  float qi = (float)qi_raw * Q14_SCALE;
  float qj = (float)qj_raw * Q14_SCALE;
  float qk = (float)qk_raw * Q14_SCALE;
  float qr = (float)qr_raw * Q14_SCALE;

  /* Convert quaternion to Euler angles */
  quat_to_euler(qi, qj, qk, qr,
                &orient->yaw, &orient->pitch, &orient->roll);

  /* Parse accuracy (Q12, only meaningful for ARVR rotation vector) */
  if (cargo_len >= 14) {
    int16_t acc_raw = (int16_t)(cargo[12] | (cargo[13] << 8));
    orient->accuracy = (float)acc_raw * Q12_SCALE;
  } else {
    orient->accuracy = 0.0f;
  }

  orient->valid = 1;
  return 0;
}
