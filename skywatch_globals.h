#ifndef SKYWATCH_GLOBALS_H
#define SKYWATCH_GLOBALS_H

/***************************************************************************************
  skywatch_globals.h
  ------------------
  All pin defines, EEPROM addresses, and extern declarations that must be visible
  to BOTH the main .ino AND ws_tunnel.h.

  Include order in SkyWatch_Pro_v17.ino:
    1. skywatch_globals.h   ← pins, EEPROM, externs
    2. dashboard.h          ← PROGMEM HTML
    3. ws_tunnel.h          ← WebSocket tunnel (uses globals defined here)
    4. .ino body            ← actual definitions of all globals
***************************************************************************************/

// ── Pin definitions ───────────────────────────────────────────────
#define RGB_PIN       48
#define NUM_PIXELS    1
#define DHT_PIN       21
#define DHT_TYPE      DHT11
#define MQ135_PIN     4
#define HALL_SPEED    14
#define MIC_PIN       3
#define DUST_ANA_PIN  8
#define DUST_LED_PIN  9
#define HALL_N        5
#define HALL_E        6
#define HALL_S        7
#define HALL_W        15
#define RAIN_PIN      16
#define RAIN_PWR_PIN  17
#define SD_CS_PIN     10
#define I2C_SDA       42
#define I2C_SCL       2
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64

// ── EEPROM layout ─────────────────────────────────────────────────
#define EEPROM_SIZE   1024
#define ADDR_SSID       0   // 32 bytes → ends  31
#define ADDR_PASS      32   // 32 bytes → ends  63
#define ADDR_R0        70   //  4 bytes → ends  73
#define ADDR_T_OFF     75   //  4 bytes → ends  78
#define ADDR_H_OFF     80   //  4 bytes → ends  83
#define ADDR_P_OFF     85   //  4 bytes → ends  88
#define ADDR_D_OFF     90   //  4 bytes → ends  93
#define ADDR_W_CAL     95   //  4 bytes → ends  98
#define ADDR_RL_CAL   100   //  4 bytes → ends 103
#define ADDR_API_KEY  120   // 20 bytes → ends 139

#include <math.h>  // sqrtf, fabsf, powf — needed by AnomalyFilter inline methods

// ── AnomalyFilter — complete definition ──────────────────────────
// Defined here so both the .ino AND ws_tunnel.h see the full class.
// The .ino must NOT redefine the class or its methods — only
// declare instances:  AnomalyFilter tempAI(3.0f), presAI(3.0f);
class AnomalyFilter {
private:
  float buffer[10];
  int   idx      = 0;
  bool  full     = false;
  float threshold;
  int   streak   = 0;

public:
  float currentMean   = 0;
  float currentStdDev = 0.1f;
  float lastZScore    = 0;

  AnomalyFilter(float thresh = 3.0f) : threshold(thresh) {
    for (int i = 0; i < 10; i++) buffer[i] = 0;
  }

  bool isGlitch(float newValue) {
    if (!full) {
      buffer[idx] = newValue;
      idx = (idx + 1) % 10;
      if (idx == 0) full = true;
      currentMean = newValue;
      return false;
    }
    float sum = 0;
    for (int i = 0; i < 10; i++) sum += buffer[i];
    currentMean = sum / 10.0f;

    float varSum = 0;
    for (int i = 0; i < 10; i++) varSum += (buffer[i] - currentMean) * (buffer[i] - currentMean);
    currentStdDev = sqrtf(varSum / 10.0f);
    if (currentStdDev < 0.1f) currentStdDev = 0.1f;

    lastZScore = fabsf((newValue - currentMean) / currentStdDev);
    if (lastZScore > threshold) {
      streak++;
      if (streak >= 3) {
        streak = 0;
        buffer[idx] = newValue;
        idx = (idx + 1) % 10;
        return false;
      }
      return true;
    }
    streak = 0;
    buffer[idx] = newValue;
    idx = (idx + 1) % 10;
    return false;
  }
};

// ── Extern declarations ───────────────────────────────────────────
// All variables are *defined* in the .ino; these just make them
// visible inside ws_tunnel.h which is compiled before the .ino body.

extern float temp, hum, pres, mslp;
extern float wind_speed, wind_gust;
extern float dew_point, cloud_base_m;
extern float ppm_co2, dust_ug, db_level;
extern float pressure_trend;
extern float temp_hi, temp_lo;
extern float R0, t_off, h_off, p_off, d_off, w_cal, rl_val;
extern float v_mq;
extern int   aqi, r_pm, r_mq;
extern int   ltg_distance_km;
extern bool  is_raining, is_daytime;
extern unsigned long i2c_latency;
extern int   ai_glitches_blocked;
extern String beaufort, wind_txt, forecast, active_sensor;
extern String ssid_name, ssid_pass, ts_key;
extern AnomalyFilter tempAI, presAI;

#endif // SKYWATCH_GLOBALS_H
