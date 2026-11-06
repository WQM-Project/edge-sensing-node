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
#include <stdio.h>
#include <string.h>

/* Private defines -----------------------------------------------------------*/

/* ── SHTP channel numbers ───────────────────────────────────────────────── */
#define SHTP_CHAN_COMMAND       0  /**< Device → host: errors / advertisements  */
#define SHTP_CHAN_EXECUTABLE    1  /**< Host → device: reset / DFU              */
#define SHTP_CHAN_CONTROL       2  /**< Host ↔ device: SH-2 control             */
#define SHTP_CHAN_INPUT_REPORT  3  /**< Device → host: sensor reports            */

/* ── SH-2 report IDs ───────────────────────────────────────────────────── */
#define SH2_ROTATION_VECTOR        0x05
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

/* ── Maximum SHTP cargo we expect (advertisement packet is ~276 bytes) ─── */
#define SHTP_MAX_CARGO             300
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

/** Active I2C address determined during Init (0x4A or 0x4B shifted) */
uint8_t bno085_i2c_addr = BNO085_I2C_ADDR;

/** Stored report interval for re-enabling after reset */
static uint16_t configured_report_ms = 100;

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

  return HAL_I2C_Master_Transmit(hi2c, bno085_i2c_addr, shtp_buf, total,
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
 *
 * CRITICAL: SHTP over I2C requires reading the ENTIRE packet (header + cargo)
 * in a SINGLE I2C transaction.  If you issue two separate Master_Receive calls
 * the BNO085 resets its internal byte pointer at each START condition, so the
 * second read starts a brand-new (usually empty) transaction — not a
 * continuation of the first.  The correct sequence is:
 *   1. Peek: read exactly 4 bytes to discover the total packet length.
 *   2. Full read: read the complete packet (up to SHTP_MAX_PACKET bytes) in
 *      one transaction, which re-sends the header then the cargo.
 */
static int16_t shtp_receive(I2C_HandleTypeDef *hi2c, uint8_t *channel_out) {
  /* Step 1 — peek at the 4-byte SHTP header to learn packet length. */
  uint8_t hdr[SHTP_HEADER_SIZE];
  HAL_StatusTypeDef st = HAL_I2C_Master_Receive(hi2c, bno085_i2c_addr, hdr,
                                                SHTP_HEADER_SIZE,
                                                BNO085_I2C_TIMEOUT);
  if (st != HAL_OK) return -1;

  /* Parse length (little-endian, bit 15 = continuation flag — ignore it) */
  uint16_t pkt_len = (uint16_t)(hdr[0] | ((hdr[1] & 0x7F) << 8));

  if (pkt_len <= SHTP_HEADER_SIZE || pkt_len == 0x7FFF || pkt_len == 0xFFFF) {
    /* Bus idle or empty packet — nothing to do */
    return 0;
  }

  uint16_t read_len = pkt_len;
  if (read_len > SHTP_MAX_PACKET) {
    read_len = SHTP_MAX_PACKET;
  }

  /*
   * Step 2 — full-packet read in a single I2C transaction.
   * The BNO085 re-clocks the 4-byte header first, then the cargo, all in one
   * transfer.  We receive into shtp_buf directly so header + cargo end up
   * contiguous: shtp_buf[0..3] = header, shtp_buf[4..] = cargo.
   */
  st = HAL_I2C_Master_Receive(hi2c, bno085_i2c_addr, shtp_buf, read_len,
                              BNO085_I2C_TIMEOUT);
  if (st != HAL_OK) return -1;

  /* Drain any bytes beyond our buffer limit to keep the bus clean */
  if (pkt_len > read_len) {
    uint16_t excess = pkt_len - read_len;
    uint8_t discard[16];
    while (excess > 0) {
      uint16_t chunk = (excess > (uint16_t)sizeof(discard))
                           ? (uint16_t)sizeof(discard)
                           : excess;
      HAL_I2C_Master_Receive(hi2c, bno085_i2c_addr, discard, chunk, 20);
      excess -= chunk;
    }
  }

  *channel_out = shtp_buf[2];

  /* Mirror the device's sequence number so our next TX seq is in sync */
  if (*channel_out < (uint8_t)sizeof(shtp_seq)) {
    shtp_seq[*channel_out] = shtp_buf[3];
  }

  return (int16_t)(read_len - SHTP_HEADER_SIZE);
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
  char dbg[96];

  /* Probe both 0x4A and 0x4B addresses with retries */
  uint8_t found = 0;
  const uint8_t candidate_addrs[2] = {BNO085_I2C_ADDR, BNO085_I2C_ADDR_ALT};

  for (int retry = 0; retry < 5 && !found; retry++) {
    for (int i = 0; i < 2; i++) {
      if (HAL_I2C_IsDeviceReady(hi2c, candidate_addrs[i], 2, 50) == HAL_OK) {
        bno085_i2c_addr = candidate_addrs[i];
        found = 1;
        break;
      }
    }
    if (!found) {
      HAL_Delay(20);
    }
  }

  if (!found) {
    Debug_Print("[BNO085] Not responding at address 0x4A or 0x4B\r\n");
    return 1; /* Chip not responding at either 0x4A or 0x4B */
  }

  snprintf(dbg, sizeof(dbg), "[BNO085] Device detected at 0x%02X\r\n",
           bno085_i2c_addr >> 1);
  Debug_Print(dbg);

  /* Clear sequence counters */
  memset(shtp_seq, 0, sizeof(shtp_seq));

  /* Soft-reset */
  BNO085_Reset(hi2c);
  HAL_Delay(400);  /* BNO085 needs ~300-500 ms after reset to boot */

  /*
   * Drain startup packets (advertisements, reset notification).
   * Allow a few consecutive empty reads before giving up so we don't
   * abort too early when the BNO085 is still booting and briefly quiet.
   */
  uint8_t chan;
  int drained = 0;
  int empty_streak = 0;
  for (int drain = 0; drain < 20; drain++) {
    int16_t len = shtp_receive(hi2c, &chan);
    if (len > 0) {
      drained++;
      empty_streak = 0;
      HAL_Delay(10);
    } else {
      empty_streak++;
      if (empty_streak >= 3) break; /* Three consecutive empty reads — bus idle */
      HAL_Delay(20);
    }
  }

  snprintf(dbg, sizeof(dbg), "[BNO085] Drained %d boot packet(s)\r\n", drained);
  Debug_Print(dbg);

  /* Verify communication: send Product ID request */
  uint8_t pid_req[2] = {SH2_PRODUCT_ID_REQ, 0};
  shtp_send(hi2c, SHTP_CHAN_CONTROL, pid_req, sizeof(pid_req));
  HAL_Delay(40);

  /* Look for Product ID response (give it up to 500 ms) */
  uint8_t got_response = 0;
  for (int tries = 0; tries < 25; tries++) {
    int16_t len = shtp_receive(hi2c, &chan);
    if (len > 0 && chan == SHTP_CHAN_CONTROL &&
        shtp_buf[SHTP_HEADER_SIZE] == SH2_PRODUCT_ID_RESP) {
      got_response = 1;
      uint8_t sw_maj = shtp_buf[SHTP_HEADER_SIZE + 2];
      uint8_t sw_min = shtp_buf[SHTP_HEADER_SIZE + 3];
      snprintf(dbg, sizeof(dbg), "[BNO085] SH-2 Product ID OK (FW v%d.%d)\r\n",
               sw_maj, sw_min);
      Debug_Print(dbg);
      break;
    }
    HAL_Delay(20);
  }

  /* Retry once if no response on first attempt */
  if (!got_response) {
    Debug_Print("[BNO085] Retrying Product ID request...\r\n");
    shtp_send(hi2c, SHTP_CHAN_CONTROL, pid_req, sizeof(pid_req));
    HAL_Delay(40);
    for (int tries = 0; tries < 15; tries++) {
      int16_t len = shtp_receive(hi2c, &chan);
      if (len > 0 && chan == SHTP_CHAN_CONTROL &&
          shtp_buf[SHTP_HEADER_SIZE] == SH2_PRODUCT_ID_RESP) {
        got_response = 1;
        uint8_t sw_maj = shtp_buf[SHTP_HEADER_SIZE + 2];
        uint8_t sw_min = shtp_buf[SHTP_HEADER_SIZE + 3];
        snprintf(dbg, sizeof(dbg), "[BNO085] SH-2 Product ID OK on retry (FW v%d.%d)\r\n",
                 sw_maj, sw_min);
        Debug_Print(dbg);
        break;
      }
      HAL_Delay(20);
    }
  }

  if (!got_response) {
    Debug_Print("[BNO085] No Product ID response after reset\r\n");
    return 1;  /* Sensor not responding */
  }

  /* Save configured report interval */
  configured_report_ms = report_ms;

  /* Enable 9-DOF Rotation Vector and 6-DOF Game Rotation Vector reports */
  uint32_t interval_us = (uint32_t)report_ms * 1000U;
  shtp_set_feature(hi2c, SH2_ROTATION_VECTOR, interval_us);
  HAL_Delay(40);
  shtp_set_feature(hi2c, SH2_GAME_ROTATION_VECTOR, interval_us);
  HAL_Delay(40);

  /* Drain the feature response */
  for (int drain = 0; drain < 5; drain++) {
    int16_t len = shtp_receive(hi2c, &chan);
    if (len <= 0) break;
    HAL_Delay(10);
  }

  return 0;
}

/**
 * @brief  Poll the BNO085 for new orientation reports (Rotation Vector /
 *         Game Rotation Vector) and convert quaternion to Euler angles.
 *
 * @return 0 = new data written to *orient.
 *         1 = no new data (call again later).
 *         2 = I2C communication error.
 */
uint8_t BNO085_Read(I2C_HandleTypeDef *hi2c, BNO085_Orientation_t *orient) {
  uint8_t found_report = 0;

  /* Drain pending packets from BNO085 FIFO buffer so we get the freshest rotation report */
  for (int iter = 0; iter < 20; iter++) {
    uint8_t chan = 0;
    int16_t cargo_len = shtp_receive(hi2c, &chan);

    if (cargo_len <= 0) break;

    /* If device reset or sent an advertisement, re-enable feature reports */
    if (chan == SHTP_CHAN_COMMAND || chan == SHTP_CHAN_EXECUTABLE) {
      uint32_t interval_us = (uint32_t)configured_report_ms * 1000U;
      shtp_set_feature(hi2c, SH2_ROTATION_VECTOR, interval_us);
      shtp_set_feature(hi2c, SH2_GAME_ROTATION_VECTOR, interval_us);
      continue;
    }

    /* We only care about Input Report channel (3) */
    if (chan != SHTP_CHAN_INPUT_REPORT) continue;

    uint8_t *cargo = &shtp_buf[SHTP_HEADER_SIZE];
    uint16_t offset = 0;

    while (offset < cargo_len) {
      uint8_t report_id = cargo[offset];

      /* Timestamps: Base Timestamp (0xFB, 0xF1), Timestamp Rebase (0xFA), Timestamp Reference (0xF2) = 5 bytes */
      if (report_id == 0xFB || report_id == 0xFA || report_id == 0xF1 || report_id == 0xF2) {
        offset += 5;
        continue;
      }

      /* Determine expected report length:
       *   SH2_ROTATION_VECTOR (0x05) & SH2_ARVR_ROTATION_VECTOR (0x28): 14 bytes (with accuracy)
       *   SH2_GAME_ROTATION_VECTOR (0x08) & SH2_ARVR_GAME_ROTATION_VECTOR (0x29): 12 bytes (no accuracy)
       */
      uint8_t rpt_size = 0;
      if (report_id == SH2_ROTATION_VECTOR || report_id == SH2_ARVR_ROTATION_VECTOR) {
        rpt_size = 14;
      } else if (report_id == SH2_GAME_ROTATION_VECTOR || report_id == 0x29) {
        rpt_size = 12;
      }

      if (rpt_size > 0) {
        if (offset + rpt_size > cargo_len) break; /* Truncated in packet */

        /* Parse Q14 fixed-point quaternion components */
        int16_t qi_raw = (int16_t)(cargo[offset + 4]  | (cargo[offset + 5]  << 8));
        int16_t qj_raw = (int16_t)(cargo[offset + 6]  | (cargo[offset + 7]  << 8));
        int16_t qk_raw = (int16_t)(cargo[offset + 8]  | (cargo[offset + 9]  << 8));
        int16_t qr_raw = (int16_t)(cargo[offset + 10] | (cargo[offset + 11] << 8));

        float qi = (float)qi_raw * Q14_SCALE;
        float qj = (float)qj_raw * Q14_SCALE;
        float qk = (float)qk_raw * Q14_SCALE;
        float qr = (float)qr_raw * Q14_SCALE;

        /* Convert quaternion to Euler angles (yaw / pitch / roll) */
        quat_to_euler(qi, qj, qk, qr,
                      &orient->yaw, &orient->pitch, &orient->roll);

        /* Parse accuracy (Q12) if 14-byte format */
        if (rpt_size == 14) {
          int16_t acc_raw = (int16_t)(cargo[offset + 12] | (cargo[offset + 13] << 8));
          orient->accuracy = (float)acc_raw * Q12_SCALE;
        } else {
          orient->accuracy = 0.0f;
        }

        found_report = 1;
        orient->valid = 1;
        offset += rpt_size;
      }
      /* Skip standard 10-byte motion reports if present (accel, gyro, mag, lin_accel, gravity) */
      else if (report_id >= 0x01 && report_id <= 0x06) {
        offset += 10;
      }
      else {
        break; /* Unhandled report type, end packet parse */
      }
    }
  }

  return found_report ? 0 : 1;
}
