/**
 ******************************************************************************
 * @file    gps.c
 * @brief   GPS NMEA-0183 parser — extracts GGA and RMC sentence fields.
 *
 *          Receives bytes one-at-a-time from UART RX interrupt, assembles
 *          complete NMEA sentences, and parses $GPGGA / $GNGGA and
 *          $GPRMC / $GNRMC for position, altitude, speed, course, time, and
 *          satellite/fix quality information.
 *
 *          Supports both GP (GPS-only) and GN (multi-constellation) talker
 *          IDs so it works with u-blox NEO-6M/7M/8M, BN-220, Quectel, etc.
 *
 * @note    Pin assignment: UART4 — PC10 (TX) / PC11 (RX), 9600 baud.
 ******************************************************************************
 */

/* Includes ------------------------------------------------------------------*/
#include "gps.h"
#include <stdlib.h>
#include <string.h>

/* Private defines -----------------------------------------------------------*/

/** Internal ring-buffer is not needed — we parse byte-by-byte into a
 *  line buffer terminated by '\n'. */
#define NMEA_START_CHAR  '$'
#define NMEA_END_CHAR    '\n'

/* Private variables ---------------------------------------------------------*/

/** Line assembly buffer */
static char     nmea_line[GPS_NMEA_MAX_LEN];
static uint8_t  nmea_idx = 0;
static uint8_t  nmea_receiving = 0;

/** Latest parsed GPS data (double-buffered with a "ready" copy) */
static volatile GPS_Data_t gps_parsed;
static volatile uint8_t    gps_data_lock = 0;

/** UART handle cached from GPS_Init */
static UART_HandleTypeDef *gps_huart = NULL;

/** Single-byte RX buffer for interrupt reception */
static uint8_t gps_rx_byte;

/* Private function prototypes -----------------------------------------------*/

static void     parse_nmea_sentence(const char *sentence);
static void     parse_gga(const char *sentence);
static void     parse_rmc(const char *sentence);
static float    nmea_coord_to_decimal(const char *raw, char hemisphere);
static uint8_t  nmea_checksum_valid(const char *sentence);
static const char *field_at(const char *sentence, uint8_t index);
static void     field_copy(const char *start, char *dst, uint8_t max_len);

/* ═══════════════════════════════════════════════════════════════════════════ */
/*                              PUBLIC API                                    */
/* ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Initialise GPS reception.  Starts interrupt-based byte reception.
 */
uint8_t GPS_Init(UART_HandleTypeDef *huart) {
  gps_huart = huart;
  nmea_idx = 0;
  nmea_receiving = 0;

  /* Zero the parsed struct */
  memset((void *)&gps_parsed, 0, sizeof(gps_parsed));

  /* Start receiving the first byte via interrupt */
  if (HAL_UART_Receive_IT(gps_huart, &gps_rx_byte, 1) != HAL_OK) {
    return 1;
  }
  return 0;
}

/**
 * @brief  Feed a single byte into the NMEA state machine.
 *
 * Typical usage: call this from HAL_UART_RxCpltCallback() when the
 * interrupt fires for UART4.
 */
void GPS_FeedByte(uint8_t byte) {
  if (byte == NMEA_START_CHAR) {
    /* Start of a new sentence */
    nmea_idx = 0;
    nmea_receiving = 1;
    nmea_line[nmea_idx++] = (char)byte;
    return;
  }

  if (!nmea_receiving) return;

  if (nmea_idx < GPS_NMEA_MAX_LEN - 1) {
    nmea_line[nmea_idx++] = (char)byte;
  }

  if (byte == NMEA_END_CHAR || nmea_idx >= GPS_NMEA_MAX_LEN - 1) {
    /* Null-terminate and parse */
    nmea_line[nmea_idx] = '\0';
    parse_nmea_sentence(nmea_line);
    nmea_receiving = 0;
    nmea_idx = 0;
  }
}

/**
 * @brief  Copy the latest parsed GPS data to the caller's struct.
 */
void GPS_GetData(GPS_Data_t *data) {
  /* Simple lock-free copy — interrupt may update during copy but
     individual fields are 32-bit aligned, so partial writes are
     unlikely.  For production, use a double-buffer or disable IRQ. */
  __disable_irq();
  memcpy(data, (const void *)&gps_parsed, sizeof(GPS_Data_t));
  __enable_irq();
}

/**
 * @brief  Returns 1 if the latest RMC status indicated a valid fix.
 */
uint8_t GPS_HasFix(void) {
  return gps_parsed.valid;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*                        UART RX CALLBACK HOOK                               */
/* ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Call this from HAL_UART_RxCpltCallback when huart == UART4.
 *         It feeds the received byte and re-arms the interrupt.
 *
 * Example (in main.c or stm32f7xx_it.c):
 *   void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
 *       if (huart->Instance == UART4) {
 *           extern void GPS_UART_IRQCallback(void);
 *           GPS_UART_IRQCallback();
 *       }
 *   }
 */
void GPS_UART_IRQCallback(void) {
  GPS_FeedByte(gps_rx_byte);

  /* Re-arm for the next byte */
  if (gps_huart != NULL) {
    HAL_UART_Receive_IT(gps_huart, &gps_rx_byte, 1);
  }
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*                         NMEA SENTENCE ROUTER                               */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void parse_nmea_sentence(const char *sentence) {
  if (sentence[0] != '$') return;

  /* Validate checksum (optional but recommended) */
  if (!nmea_checksum_valid(sentence)) return;

  /* Route by sentence type (skip $XX prefix, compare from index 3) */
  const char *type = &sentence[3];

  if (strncmp(type, "GGA", 3) == 0) {
    parse_gga(sentence);
  } else if (strncmp(type, "RMC", 3) == 0) {
    parse_rmc(sentence);
  }
  /* Other sentences (GSA, GSV, VTG, GLL) ignored for now */
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*                       GGA PARSER (position + quality)                      */
/* ═══════════════════════════════════════════════════════════════════════════ */

/**
 * GGA fields:
 *   0: $xxGGA
 *   1: UTC time        hhmmss.sss
 *   2: Latitude        ddmm.mmmmm
 *   3: N/S
 *   4: Longitude       dddmm.mmmmm
 *   5: E/W
 *   6: Fix quality     0/1/2
 *   7: Satellites in use
 *   8: HDOP
 *   9: Altitude (MSL)
 *  10: Altitude unit (M)
 *  11: Geoid separation
 *  12: Geoid unit
 *  13: DGPS age
 *  14: DGPS station ID
 */
static void parse_gga(const char *sentence) {
  char field[20];

  /* Field 1: UTC time */
  const char *p = field_at(sentence, 1);
  if (p) {
    field_copy(p, field, sizeof(field));
    if (strlen(field) >= 6) {
      gps_parsed.hour   = (uint8_t)((field[0] - '0') * 10 + (field[1] - '0'));
      gps_parsed.minute = (uint8_t)((field[2] - '0') * 10 + (field[3] - '0'));
      gps_parsed.second = (uint8_t)((field[4] - '0') * 10 + (field[5] - '0'));
    }
  }

  /* Field 2-3: Latitude + hemisphere */
  p = field_at(sentence, 2);
  if (p) {
    field_copy(p, field, sizeof(field));
    const char *h = field_at(sentence, 3);
    char hem = 'N';
    if (h && (*h == 'N' || *h == 'S')) hem = *h;
    if (strlen(field) > 0) {
      gps_parsed.latitude = nmea_coord_to_decimal(field, hem);
    }
  }

  /* Field 4-5: Longitude + hemisphere */
  p = field_at(sentence, 4);
  if (p) {
    field_copy(p, field, sizeof(field));
    const char *h = field_at(sentence, 5);
    char hem = 'E';
    if (h && (*h == 'E' || *h == 'W')) hem = *h;
    if (strlen(field) > 0) {
      gps_parsed.longitude = nmea_coord_to_decimal(field, hem);
    }
  }

  /* Field 6: Fix quality */
  p = field_at(sentence, 6);
  if (p) {
    gps_parsed.fix_quality = (uint8_t)(*p - '0');
  }

  /* Field 7: Satellites in use */
  p = field_at(sentence, 7);
  if (p) {
    field_copy(p, field, sizeof(field));
    gps_parsed.satellites = (uint8_t)atoi(field);
  }

  /* Field 8: HDOP */
  p = field_at(sentence, 8);
  if (p) {
    field_copy(p, field, sizeof(field));
    gps_parsed.hdop = (float)atof(field);
  }

  /* Field 9: Altitude */
  p = field_at(sentence, 9);
  if (p) {
    field_copy(p, field, sizeof(field));
    gps_parsed.altitude_m = (float)atof(field);
  }
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*                  RMC PARSER (position + speed + course + date)              */
/* ═══════════════════════════════════════════════════════════════════════════ */

/**
 * RMC fields:
 *   0: $xxRMC
 *   1: UTC time
 *   2: Status          A=active, V=void
 *   3: Latitude
 *   4: N/S
 *   5: Longitude
 *   6: E/W
 *   7: Speed (knots)
 *   8: Course (degrees)
 *   9: Date            ddmmyy
 *  10: Magnetic variation
 *  11: E/W
 */
static void parse_rmc(const char *sentence) {
  char field[20];

  /* Field 2: Status */
  const char *p = field_at(sentence, 2);
  if (p) {
    gps_parsed.valid = (*p == 'A') ? 1 : 0;
  }

  /* If no valid fix, still parse what we can but mark invalid */

  /* Field 3-4: Latitude */
  p = field_at(sentence, 3);
  if (p) {
    field_copy(p, field, sizeof(field));
    const char *h = field_at(sentence, 4);
    char hem = 'N';
    if (h && (*h == 'N' || *h == 'S')) hem = *h;
    if (strlen(field) > 0) {
      gps_parsed.latitude = nmea_coord_to_decimal(field, hem);
    }
  }

  /* Field 5-6: Longitude */
  p = field_at(sentence, 5);
  if (p) {
    field_copy(p, field, sizeof(field));
    const char *h = field_at(sentence, 6);
    char hem = 'E';
    if (h && (*h == 'E' || *h == 'W')) hem = *h;
    if (strlen(field) > 0) {
      gps_parsed.longitude = nmea_coord_to_decimal(field, hem);
    }
  }

  /* Field 7: Speed over ground (knots) */
  p = field_at(sentence, 7);
  if (p) {
    field_copy(p, field, sizeof(field));
    if (strlen(field) > 0) {
      gps_parsed.speed_knots = (float)atof(field);
    }
  }

  /* Field 8: Course over ground (degrees) */
  p = field_at(sentence, 8);
  if (p) {
    field_copy(p, field, sizeof(field));
    if (strlen(field) > 0) {
      gps_parsed.course_deg = (float)atof(field);
    }
  }

  /* Field 9: Date (ddmmyy) */
  p = field_at(sentence, 9);
  if (p) {
    field_copy(p, field, sizeof(field));
    if (strlen(field) >= 6) {
      gps_parsed.day   = (uint8_t)((field[0] - '0') * 10 + (field[1] - '0'));
      gps_parsed.month = (uint8_t)((field[2] - '0') * 10 + (field[3] - '0'));
      gps_parsed.year  = (uint8_t)((field[4] - '0') * 10 + (field[5] - '0'));
    }
  }
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*                         NMEA HELPER FUNCTIONS                              */
/* ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Convert NMEA coordinate (ddmm.mmmmm or dddmm.mmmmm) to
 *         decimal degrees.
 *
 *         NMEA latitude:  ddmm.mmmmm  (d=2 digits)
 *         NMEA longitude: dddmm.mmmmm (d=3 digits)
 *
 * @param  raw        Raw NMEA coordinate string.
 * @param  hemisphere 'N', 'S', 'E', or 'W'.
 * @return Decimal degrees (negative for S and W).
 */
static float nmea_coord_to_decimal(const char *raw, char hemisphere) {
  if (raw == NULL || raw[0] == '\0') return 0.0f;

  float raw_val = (float)atof(raw);

  /* Degrees: integer part of (raw / 100) */
  int degrees = (int)(raw_val / 100.0f);

  /* Minutes: remainder */
  float minutes = raw_val - (float)(degrees * 100);

  float decimal = (float)degrees + minutes / 60.0f;

  if (hemisphere == 'S' || hemisphere == 'W') {
    decimal = -decimal;
  }

  return decimal;
}

/**
 * @brief  Validate NMEA checksum.
 *
 *         Checksum is XOR of all characters between '$' and '*'.
 *         Expected checksum follows '*' as two hex digits.
 *
 * @return 1 if valid (or no checksum present), 0 if mismatch.
 */
static uint8_t nmea_checksum_valid(const char *sentence) {
  if (sentence[0] != '$') return 0;

  uint8_t calc_cs = 0;
  const char *p = &sentence[1];

  while (*p && *p != '*' && *p != '\r' && *p != '\n') {
    calc_cs ^= (uint8_t)*p;
    p++;
  }

  if (*p != '*') return 1;  /* No checksum field — accept */

  /* Parse the two-digit hex checksum after '*' */
  p++;
  char hex[3] = {p[0], p[1], '\0'};
  uint8_t expected_cs = (uint8_t)strtol(hex, NULL, 16);

  return (calc_cs == expected_cs) ? 1 : 0;
}

/**
 * @brief  Return a pointer to the start of field N (0-indexed) in an
 *         NMEA sentence.  Fields are comma-separated.
 *
 * @return Pointer to first char of the field, or NULL if not found.
 */
static const char *field_at(const char *sentence, uint8_t index) {
  const char *p = sentence;
  uint8_t field = 0;

  /* Field 0 starts at the beginning of the sentence (e.g. "$GPGGA") */
  if (index == 0) return p;

  while (*p) {
    if (*p == ',') {
      field++;
      if (field == index) return p + 1;
    }
    p++;
  }
  return NULL;
}

/**
 * @brief  Copy a comma-terminated or CR/LF-terminated field into dst.
 */
static void field_copy(const char *start, char *dst, uint8_t max_len) {
  uint8_t i = 0;
  while (start[i] && start[i] != ',' && start[i] != '*' &&
         start[i] != '\r' && start[i] != '\n' && i < max_len - 1) {
    dst[i] = start[i];
    i++;
  }
  dst[i] = '\0';
}
