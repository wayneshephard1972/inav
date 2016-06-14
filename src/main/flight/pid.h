/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "rx/rx.h"

#include "io/rc_controls.h"

#include "config/runtime_config.h"

#define GYRO_SATURATION_LIMIT   1800        // 1800dps
#define PID_MAX_OUTPUT          1000
#define YAW_P_LIMIT_MIN 100                 // Maximum value for yaw P limiter
#define YAW_P_LIMIT_MAX 500                 // Maximum value for yaw P limiter
#define YAW_P_LIMIT_DEFAULT 300             // Default value for yaw P limiter

#define MAG_HOLD_RATE_LIMIT_MIN 10
#define MAG_HOLD_RATE_LIMIT_MAX 250
#define MAG_HOLD_RATE_LIMIT_DEFAULT 90

typedef enum {
    PIDROLL,
    PIDPITCH,
    PIDYAW,
    PIDALT,
    PIDPOS,
    PIDPOSR,
    PIDNAVR,
    PIDLEVEL,
    PIDMAG,
    PIDVEL,
    PID_ITEM_COUNT
} pidIndex_e;

typedef struct pidProfile_s {
    uint8_t P8[PID_ITEM_COUNT];
    uint8_t I8[PID_ITEM_COUNT];
    uint8_t D8[PID_ITEM_COUNT];

    uint8_t dterm_lpf_hz;                   // (default 17Hz, Range 1-50Hz) Used for PT1 element in PID1, PID2 and PID5
    uint8_t yaw_pterm_lpf_hz;               // Used for filering Pterm noise on noisy frames
    uint8_t gyro_soft_lpf_hz;               // Gyro FIR filtering
    uint8_t acc_soft_lpf_hz;                // Set the Low Pass Filter factor for ACC. Reducing this value would reduce ACC noise (visible in GUI), but would increase ACC lag time. Zero = no filter

    uint16_t yaw_p_limit;
    uint8_t yaw_lpf_hz;

    int16_t max_angle_inclination[ANGLE_INDEX_COUNT];       // Max possible inclination (roll and pitch axis separately

    uint8_t mag_hold_rate_limit;    //Maximum rotation rate MAG_HOLD mode cas feed to yaw rate PID controller

} pidProfile_t;

extern int16_t axisPID[];
extern int32_t axisPID_P[], axisPID_I[], axisPID_D[], axisPID_Setpoint[];

void pidResetErrorAccumulators(void);
void updatePIDCoefficients(const pidProfile_t *pidProfile, const controlRateConfig_t *controlRateConfig, const rxConfig_t *rxConfig);

int16_t pidAngleToRcCommand(float angleDeciDegrees);
float pidRateToRcCommand(float rateDPS, uint8_t rate);

void pidController(const pidProfile_t *pidProfile, const controlRateConfig_t *controlRateConfig, const rxConfig_t *rxConfig);

enum {
    MAG_HOLD_DISABLED = 0,
    MAG_HOLD_UPDATE_HEADING,
    MAG_HOLD_ENABLED
};

void updateMagHoldHeading(int16_t heading);
int16_t getMagHoldHeading();
