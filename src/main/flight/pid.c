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

#include <stdbool.h>
#include <stdint.h>
#include <math.h>

#include <platform.h>

#include "build_config.h"
#include "debug.h"

#include "config/runtime_config.h"

#include "common/axis.h"
#include "common/maths.h"
#include "common/filter.h"

#include "drivers/sensor.h"
#include "drivers/accgyro.h"
#include "drivers/gyro_sync.h"

#include "sensors/sensors.h"
#include "sensors/gyro.h"
#include "sensors/acceleration.h"

#include "rx/rx.h"

#include "io/rc_controls.h"
#include "io/gps.h"

#include "flight/pid.h"
#include "flight/imu.h"
#include "flight/navigation_rewrite.h"

#define MAG_HOLD_ERROR_LPF_FREQ 2

typedef struct {
    float kP;
    float kI;
    float kD;
    float kT;

    float gyroRate;
    float rateTarget;

    // Buffer for derivative calculation
#define DTERM_BUF_COUNT 5
    float dTermBuf[DTERM_BUF_COUNT];

    // Rate integrator
    float errorGyroIf;
    float errorGyroIfLimit;

    // Axis lock accumulator
    float axisLockAccum;

    // Used for ANGLE filtering
    filterStatePt1_t angleFilterState;

    // Rate filtering
    filterStatePt1_t ptermLpfState;
    filterStatePt1_t deltaLpfState;
} pidState_t;

extern uint8_t motorCount;
extern bool motorLimitReached;
extern float dT;

int16_t magHoldTargetHeading;

// Thrust PID Attenuation factor. 0.0f means fully attenuated, 1.0f no attenuation is applied
static float tpaFactor;
int16_t axisPID[FLIGHT_DYNAMICS_INDEX_COUNT];

#ifdef BLACKBOX
int32_t axisPID_P[FLIGHT_DYNAMICS_INDEX_COUNT], axisPID_I[FLIGHT_DYNAMICS_INDEX_COUNT], axisPID_D[FLIGHT_DYNAMICS_INDEX_COUNT], axisPID_Setpoint[FLIGHT_DYNAMICS_INDEX_COUNT];
#endif

static pidState_t pidState[FLIGHT_DYNAMICS_INDEX_COUNT];

void pidResetErrorAccumulators(void)
{
    // Reset R/P/Y integrator
    for (int axis = 0; axis < 3; axis++) {
        pidState[axis].errorGyroIf = 0.0f;
        pidState[axis].errorGyroIfLimit = 0.0f;
    }

    // Reset yaw heading lock accumulator
    pidState[FD_YAW].axisLockAccum = 0;
}

static float pidRcCommandToAngle(int16_t stick)
{
    return stick * 2.0f;
}

int16_t pidAngleToRcCommand(float angleDeciDegrees)
{
    return angleDeciDegrees / 2.0f;
}

float pidRateToRcCommand(float rateDPS, uint8_t rate)
{
    return (rateDPS * 50.0f) / (rate + 20.0f);
}

float pidRcCommandToRate(int16_t stick, uint8_t rate)
{
    // Map stick position from 200dps to 1200dps
    return (float)((rate + 20) * stick) / 50.0f;
}

#define FP_PID_RATE_P_MULTIPLIER    40.0f       // betaflight - 40.0
#define FP_PID_RATE_I_MULTIPLIER    10.0f       // betaflight - 10.0
#define FP_PID_RATE_D_MULTIPLIER    4000.0f     // betaflight - 1000.0
#define FP_PID_LEVEL_P_MULTIPLIER   40.0f       // betaflight - 10.0
#define FP_PID_YAWHOLD_P_MULTIPLIER 80.0f

#define KD_ATTENUATION_BREAK        0.25f

void updatePIDCoefficients(const pidProfile_t *pidProfile, const controlRateConfig_t *controlRateConfig, const rxConfig_t *rxConfig)
{
    // TPA should be updated only when TPA is actually set
    if (controlRateConfig->dynThrPID == 0 || rcData[THROTTLE] < controlRateConfig->tpa_breakpoint) {
        tpaFactor = 1.0f;
    } else if (rcData[THROTTLE] < 2000) {
        tpaFactor = (100 - (uint16_t)controlRateConfig->dynThrPID * (rcData[THROTTLE] - controlRateConfig->tpa_breakpoint) / (2000 - controlRateConfig->tpa_breakpoint)) / 100.0f;
    } else {
        tpaFactor = (100 - controlRateConfig->dynThrPID) / 100.0f;
    }

    // Additional throttle-based KD attenuation (kudos to RS2K & Raceflight)
    float relThrottle = constrainf( ((float)rcData[THROTTLE] - (float)rxConfig->mincheck) / ((float)rxConfig->maxcheck - (float)rxConfig->mincheck), 0.0f, 1.0f);
    float kdAttenuationFactor;

    if (relThrottle < KD_ATTENUATION_BREAK) {
        kdAttenuationFactor = constrainf((relThrottle / KD_ATTENUATION_BREAK) + 0.50f, 0.0f, 1.0f);
    } else {
        kdAttenuationFactor = 1.0f;
    }

    // PID coefficients can be update only with THROTTLE and TPA or inflight PID adjustments
    //TODO: Next step would be to update those only at THROTTLE or inflight adjustments change
    for (int axis = 0; axis < 3; axis++) {
        pidState[axis].kP = pidProfile->P8[axis] / FP_PID_RATE_P_MULTIPLIER;
        pidState[axis].kI = pidProfile->I8[axis] / FP_PID_RATE_I_MULTIPLIER;
        pidState[axis].kD = pidProfile->D8[axis] / FP_PID_RATE_D_MULTIPLIER;

        // Apply TPA to ROLL and PITCH axes
        if (axis != FD_YAW) {
            pidState[axis].kP *= tpaFactor;
            pidState[axis].kD *= tpaFactor * kdAttenuationFactor;
        }

        if ((pidProfile->P8[axis] != 0) && (pidProfile->I8[axis] != 0)) {
            pidState[axis].kT = 2.0f / ((pidState[axis].kP / pidState[axis].kI) + (pidState[axis].kD / pidState[axis].kP));
        } else {
            pidState[axis].kT = 0;
        }
    }
}

static void pidApplyHeadingLock(const pidProfile_t *pidProfile, pidState_t *pidState)
{
    // Heading lock mode is different from Heading hold using compass.
    // Heading lock attempts to keep heading at current value even if there is an external disturbance.
    // If there is some external force that rotates the aircraft and Rate PIDs are unable to compensate,
    // heading lock will bring heading back if disturbance is not too big
    // Heading error is not integrated when stick input is significant or machine is disarmed.
    if (ABS(pidState->rateTarget) > 2 || !ARMING_FLAG(ARMED)) {
        pidState->axisLockAccum = 0;
    } else {
        pidState->axisLockAccum += (pidState->rateTarget - pidState->gyroRate) * dT;
        pidState->axisLockAccum = constrainf(pidState->axisLockAccum, -45, 45);
        pidState->rateTarget = pidState->axisLockAccum * (pidProfile->P8[PIDMAG] / FP_PID_YAWHOLD_P_MULTIPLIER);
    }
}

static float calcHorizonLevelStrength(const pidProfile_t *pidProfile, const rxConfig_t *rxConfig)
{
    float horizonLevelStrength = 1;

    // Figure out the raw stick positions
    const int32_t stickPosAil = ABS(getRcStickDeflection(FD_ROLL, rxConfig->midrc));
    const int32_t stickPosEle = ABS(getRcStickDeflection(FD_PITCH, rxConfig->midrc));
    const int32_t mostDeflectedPos = MAX(stickPosAil, stickPosEle);

    // Progressively turn off the horizon self level strength as the stick is banged over
    horizonLevelStrength = (float)(500 - mostDeflectedPos) / 500;  // 1 at centre stick, 0 = max stick deflection
    if (pidProfile->D8[PIDLEVEL] == 0){
        horizonLevelStrength = 0;
    } else {
        horizonLevelStrength = constrainf(((horizonLevelStrength - 1) * (100.0f / pidProfile->D8[PIDLEVEL])) + 1, 0, 1);
    }
    return horizonLevelStrength;
}

static void pidLevel(const pidProfile_t *pidProfile, pidState_t *pidState, flight_dynamics_index_t axis, float horizonLevelStrength)
{
    // This is ROLL/PITCH, run ANGLE/HORIZON controllers
    const float angleTarget = pidRcCommandToAngle(rcCommand[axis]);
    const float angleError = (constrain(angleTarget, -pidProfile->max_angle_inclination[axis], +pidProfile->max_angle_inclination[axis]) - attitude.raw[axis]) / 10.0f;

    // P[LEVEL] defines self-leveling strength (both for ANGLE and HORIZON modes)
    if (FLIGHT_MODE(HORIZON_MODE)) {
        pidState->rateTarget += angleError * (pidProfile->P8[PIDLEVEL] / FP_PID_LEVEL_P_MULTIPLIER) * horizonLevelStrength;
    } else {
        pidState->rateTarget = angleError * (pidProfile->P8[PIDLEVEL] / FP_PID_LEVEL_P_MULTIPLIER);
    }

    // Apply simple LPF to rateTarget to make response less jerky
    // Ideas behind this:
    //  1) Attitude is updated at gyro rate, rateTarget for ANGLE mode is calculated from attitude
    //  2) If this rateTarget is passed directly into gyro-base PID controller this effectively doubles the rateError.
    //     D-term that is calculated from error tend to amplify this even more. Moreover, this tend to respond to every
    //     slightest change in attitude making self-leveling jittery
    //  3) Lowering LEVEL P can make the effects of (2) less visible, but this also slows down self-leveling.
    //  4) Human pilot response to attitude change in RATE mode is fairly slow and smooth, human pilot doesn't
    //     compensate for each slightest change
    //  5) (2) and (4) lead to a simple idea of adding a low-pass filter on rateTarget for ANGLE mode damping
    //     response to rapid attitude changes and smoothing out self-leveling reaction
    if (pidProfile->I8[PIDLEVEL]) {
        // I8[PIDLEVEL] is filter cutoff frequency (Hz). Practical values of filtering frequency is 5-10 Hz
        pidState->rateTarget = filterApplyPt1(pidState->rateTarget, &pidState->angleFilterState, pidProfile->I8[PIDLEVEL], dT);
    }
}

static void pidApplyRateController(const pidProfile_t *pidProfile, pidState_t *pidState, flight_dynamics_index_t axis)
{
    const float rateError = pidState->rateTarget - pidState->gyroRate;

    // Calculate new P-term
    float newPTerm = rateError * pidState->kP;
    // Constrain YAW by yaw_p_limit value if not servo driven (in that case servo limits apply)
    if (axis == FD_YAW && (motorCount >= 4 && pidProfile->yaw_p_limit)) {
        newPTerm = constrain(newPTerm, -pidProfile->yaw_p_limit, pidProfile->yaw_p_limit);
    }

    // Additional P-term LPF on YAW axis
    if (axis == FD_YAW && pidProfile->yaw_lpf_hz) {
        newPTerm = filterApplyPt1(newPTerm, &pidState->ptermLpfState, pidProfile->yaw_lpf_hz, dT);
    }

    // Calculate new D-term
    float newDTerm;
    if (pidProfile->D8[axis] == 0) {
        // optimisation for when D8 is zero, often used by YAW axis
        newDTerm = 0;
    } else {
        // Calculate derivative using 5-point noise-robust differentiators without time delay (one-sided or forward filters)
        // by Pavel Holoborodko, see http://www.holoborodko.com/pavel/numerical-methods/numerical-derivative/smooth-low-noise-differentiators/
        // h[0] = 5/8, h[-1] = 1/4, h[-2] = -1, h[-3] = -1/4, h[-4] = 3/8
        static const float dtermCoeffs[DTERM_BUF_COUNT] = {5.0f, 2.0f, -8.0f, -2.0f, 3.0f};
        filterUpdateFIR(DTERM_BUF_COUNT, pidState->dTermBuf, pidState->gyroRate);
        newDTerm = filterApplyFIR(DTERM_BUF_COUNT, pidState->dTermBuf, dtermCoeffs, -pidState->kD / (8 * dT));

        // Apply additional lowpass
        if (pidProfile->dterm_lpf_hz) {
            newDTerm = filterApplyPt1(newDTerm, &pidState->deltaLpfState, pidProfile->dterm_lpf_hz, dT);
        }
    }

    // TODO: Get feedback from mixer on available correction range for each axis
    const float pidAttenuationFactor = STATE(PID_ATTENUATE) ? 0.33f : 1.0f;
    const float newOutput = (newPTerm + newDTerm) * pidAttenuationFactor + pidState->errorGyroIf;
    const float newOutputLimited = constrainf(newOutput, -PID_MAX_OUTPUT, +PID_MAX_OUTPUT);

    // Integrate only if we can do backtracking
    pidState->errorGyroIf += (rateError * pidState->kI * dT) + ((newOutputLimited - newOutput) * pidState->kT * dT);

    // Don't grow I-term if motors are at their limit
    if (STATE(ANTI_WINDUP) || motorLimitReached) {
        pidState->errorGyroIf = constrainf(pidState->errorGyroIf, -pidState->errorGyroIfLimit, pidState->errorGyroIfLimit);
    } else {
        pidState->errorGyroIfLimit = ABS(pidState->errorGyroIf);
    }

    axisPID[axis] = newOutputLimited;

#ifdef BLACKBOX
    axisPID_P[axis] = newPTerm;
    axisPID_I[axis] = pidState->errorGyroIf;
    axisPID_D[axis] = newDTerm;
    axisPID_Setpoint[axis] = pidState->rateTarget;
#endif
}

void updateMagHoldHeading(int16_t heading)
{
    magHoldTargetHeading = heading;
}

int16_t getMagHoldHeading() {
    return magHoldTargetHeading;
}

uint8_t getMagHoldState()
{

    #ifndef MAG
        return MAG_HOLD_DISABLED;
    #endif

    if (!sensors(SENSOR_MAG) || !STATE(SMALL_ANGLE)) {
        return MAG_HOLD_DISABLED;
    }

#if defined(NAV)
    int navHeadingState = naivationGetHeadingControlState();
    // NAV will prevent MAG_MODE from activating, but require heading control
    if (navHeadingState != NAV_HEADING_CONTROL_NONE) {
        // Apply maghold only if heading control is in auto mode
        if (navHeadingState == NAV_HEADING_CONTROL_AUTO) {
            return MAG_HOLD_ENABLED;
        }
    }
    else
#endif
    if (ABS(rcCommand[YAW]) < 15 && FLIGHT_MODE(MAG_MODE)) {
        return MAG_HOLD_ENABLED;
    } else {
        return MAG_HOLD_UPDATE_HEADING;
    }

    return MAG_HOLD_UPDATE_HEADING;
}

/*
 * MAG_HOLD P Controller returns desired rotation rate in dps to be fed to Rate controller
 */
float pidMagHold(const pidProfile_t *pidProfile)
{

    static filterStatePt1_t magHoldRateFilter;
    float magHoldRate;

    int16_t error = DECIDEGREES_TO_DEGREES(attitude.values.yaw) - magHoldTargetHeading;

    /*
     * Convert absolute error into relative to current heading
     */
    if (error <= -180) {
        error += 360;
    }

    if (error >= +180) {
        error -= 360;
    }

    /*
        New MAG_HOLD controller work slightly different that previous one.
        Old one mapped error to rotation speed in following way:
            - on rate 0 it gave about 0.5dps for each degree of error
            - error 0 = rotation speed of 0dps
            - error 180 = rotation speed of 96 degrees per second
            - output
            - that gives about 2 seconds to correct any error, no matter how big. Of course, usually more because of inertia.
        That was making him quite "soft" for small changes and rapid for big ones that started to appear
        when iNav introduced real RTH and WAYPOINT that might require rapid heading changes.

        New approach uses modified principle:
            - manual yaw rate is not used. MAG_HOLD is decoupled from manual input settings
            - instead, mag_hold_rate_limit is used. It defines max rotation speed in dps that MAG_HOLD controller can require from RateController
            - computed rotation speed is capped at -mag_hold_rate_limit and mag_hold_rate_limit
            - Default mag_hold_rate_limit = 40dps and default MAG_HOLD P-gain is 40
            - With those values, maximum rotation speed will be required from Rate Controller when error is greater that 30 degrees
            - For smaller error, required rate will be proportional.
            - It uses LPF filter set at 2Hz to additionally smoothen out any rapid changes
            - That makes correction of smaller errors stronger, and those of big errors softer

        This make looks as very slow rotation rate, but please remember this is automatic mode.
        Manual override with YAW input when MAG_HOLD is enabled will still use "manual" rates, not MAG_HOLD rates.
        Highest possible correction is 180 degrees and it will take more less 4.5 seconds. It is even more than sufficient
        to run RTH or WAYPOINT missions. My favourite rate range here is 20dps - 30dps that gives nice and smooth turns.

        Correction for small errors is much faster now. For example, old contrioller for 2deg errors required 1dps (correction in 2 seconds).
        New controller for 2deg error requires 2,6dps. 4dps for 3deg and so on up until mag_hold_rate_limit is reached.
    */

    magHoldRate = error * pidProfile->P8[PIDMAG] / 30;
    magHoldRate = constrainf(magHoldRate, -pidProfile->mag_hold_rate_limit, pidProfile->mag_hold_rate_limit);
    magHoldRate = filterApplyPt1(magHoldRate, &magHoldRateFilter, MAG_HOLD_ERROR_LPF_FREQ, dT);

    return magHoldRate;
}

void pidController(const pidProfile_t *pidProfile, const controlRateConfig_t *controlRateConfig, const rxConfig_t *rxConfig)
{

    uint8_t magHoldState = getMagHoldState();

    if (magHoldState == MAG_HOLD_UPDATE_HEADING) {
        updateMagHoldHeading(DECIDEGREES_TO_DEGREES(attitude.values.yaw));
    }

    for (int axis = 0; axis < 3; axis++) {
        // Step 1: Calculate gyro rates
        pidState[axis].gyroRate = gyroADC[axis] * gyro.scale;

        // Step 2: Read target
        float rateTarget;

        if (axis == FD_YAW && magHoldState == MAG_HOLD_ENABLED) {
            rateTarget = pidMagHold(pidProfile);
        } else {
            rateTarget = pidRcCommandToRate(rcCommand[axis], controlRateConfig->rates[axis]);
        }

        // Limit desired rate to something gyro can measure reliably
        pidState[axis].rateTarget = constrainf(rateTarget, -GYRO_SATURATION_LIMIT, +GYRO_SATURATION_LIMIT);
    }

    // Step 3: Run control for ANGLE_MODE, HORIZON_MODE, and HEADING_LOCK
    if (FLIGHT_MODE(ANGLE_MODE) || FLIGHT_MODE(HORIZON_MODE)) {
        const float horizonLevelStrength = calcHorizonLevelStrength(pidProfile, rxConfig);
        pidLevel(pidProfile, &pidState[FD_ROLL], FD_ROLL, horizonLevelStrength);
        pidLevel(pidProfile, &pidState[FD_PITCH], FD_PITCH, horizonLevelStrength);
    }

    if (FLIGHT_MODE(HEADING_LOCK) && magHoldState != MAG_HOLD_ENABLED) {
        pidApplyHeadingLock(pidProfile, &pidState[FD_YAW]);
    }

    // Step 4: Run gyro-driven control
    for (int axis = 0; axis < 3; axis++) {
        // Apply PID setpoint controller
        pidApplyRateController(pidProfile, &pidState[axis], axis);     // scale gyro rate to DPS
    }
}
