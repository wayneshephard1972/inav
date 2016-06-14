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

#include "build_config.h"
#include "platform.h"
#include "debug.h"

#include "common/axis.h"
#include "common/maths.h"
#include "common/filter.h"

#include "drivers/system.h"
#include "drivers/sensor.h"
#include "drivers/accgyro.h"

#include "sensors/sensors.h"
#include "sensors/acceleration.h"
#include "sensors/boardalignment.h"

#include "io/escservo.h"
#include "io/rc_controls.h"
#include "io/rc_curves.h"

#include "flight/pid.h"
#include "flight/imu.h"
#include "flight/navigation_rewrite.h"
#include "flight/navigation_rewrite_private.h"
#include "flight/failsafe.h"

#include "config/runtime_config.h"
#include "config/config.h"

#if defined(NAV)

/*-----------------------------------------------------------
 * Altitude controller for multicopter aircraft
 *-----------------------------------------------------------*/
static int16_t rcCommandAdjustedThrottle;
static int16_t altHoldThrottleRCZero = 1500;
static filterStatePt1_t altholdThrottleFilterState;
static bool prepareForTakeoffOnReset = false;

/* Calculate global altitude setpoint based on surface setpoint */
static void updateSurfaceTrackingAltitudeSetpoint(uint32_t deltaMicros)
{
    /* If we have a surface offset target and a valid surface offset reading - recalculate altitude target */
    if (posControl.flags.isTerrainFollowEnabled && posControl.desiredState.surface >= 0) {
        if (posControl.actualState.surface >= 0 && posControl.flags.hasValidSurfaceSensor) {
            // We better overshoot a little bit than undershoot
            float targetAltitudeError = navPidApply2(posControl.desiredState.surface, posControl.actualState.surface, US2S(deltaMicros), &posControl.pids.surface, -5.0f, +35.0f, false);
            posControl.desiredState.pos.V.Z = posControl.actualState.pos.V.Z + targetAltitudeError;
        }
        else {
            // TODO: We are possible above valid range, we now descend down to attempt to get back within range
            //updateAltitudeTargetFromClimbRate(-0.10f * posControl.navConfig->emerg_descent_rate, CLIMB_RATE_KEEP_SURFACE_TARGET);
            updateAltitudeTargetFromClimbRate(-20.0f, CLIMB_RATE_KEEP_SURFACE_TARGET);
        }
    }

#if defined(NAV_BLACKBOX)
    navTargetPosition[Z] = constrain(lrintf(posControl.desiredState.pos.V.Z), -32678, 32767);
#endif
}

// Position to velocity controller for Z axis
static void updateAltitudeVelocityController_MC(uint32_t deltaMicros)
{
    float altitudeError = posControl.desiredState.pos.V.Z - posControl.actualState.pos.V.Z;
    float targetVel = altitudeError * posControl.pids.pos[Z].param.kP;

    // hard limit desired target velocity to +/- 20 m/s
    targetVel = constrainf(targetVel, -2000.0f, 2000.0f);

    // limit max vertical acceleration 250 cm/s/s - reach the max 20 m/s target in 80 seconds
    float maxVelDifference = US2S(deltaMicros) * 250.0f;
    posControl.desiredState.vel.V.Z = constrainf(targetVel, posControl.desiredState.vel.V.Z - maxVelDifference, posControl.desiredState.vel.V.Z + maxVelDifference);

#if defined(NAV_BLACKBOX)
    navDesiredVelocity[Z] = constrain(lrintf(posControl.desiredState.vel.V.Z), -32678, 32767);
#endif
}

static void updateAltitudeThrottleController_MC(uint32_t deltaMicros)
{
    // Calculate min and max throttle boundaries (to compensate for integral windup)
    int16_t thrAdjustmentMin = (int16_t)posControl.escAndServoConfig->minthrottle - (int16_t)posControl.navConfig->mc_hover_throttle;
    int16_t thrAdjustmentMax = (int16_t)posControl.escAndServoConfig->maxthrottle - (int16_t)posControl.navConfig->mc_hover_throttle;

    posControl.rcAdjustment[THROTTLE] = navPidApply2(posControl.desiredState.vel.V.Z, posControl.actualState.vel.V.Z, US2S(deltaMicros), &posControl.pids.vel[Z], thrAdjustmentMin, thrAdjustmentMax, false);

    posControl.rcAdjustment[THROTTLE] = filterApplyPt1(posControl.rcAdjustment[THROTTLE], &altholdThrottleFilterState, NAV_THROTTLE_CUTOFF_FREQENCY_HZ, US2S(deltaMicros));
    posControl.rcAdjustment[THROTTLE] = constrain(posControl.rcAdjustment[THROTTLE], thrAdjustmentMin, thrAdjustmentMax);
}

bool adjustMulticopterAltitudeFromRCInput(void)
{
    int16_t rcThrottleAdjustment = rcCommand[THROTTLE] - altHoldThrottleRCZero;
    if (ABS(rcThrottleAdjustment) > posControl.rcControlsConfig->alt_hold_deadband) {
        // set velocity proportional to stick movement
        float rcClimbRate;

        // Make sure we can satisfy max_manual_climb_rate in both up and down directions
        if (rcThrottleAdjustment > 0) {
            // Scaling from altHoldThrottleRCZero to maxthrottle
            rcClimbRate = rcThrottleAdjustment * posControl.navConfig->max_manual_climb_rate / (posControl.escAndServoConfig->maxthrottle - altHoldThrottleRCZero);
        }
        else {
            // Scaling from minthrottle to altHoldThrottleRCZero
            rcClimbRate = rcThrottleAdjustment * posControl.navConfig->max_manual_climb_rate / (altHoldThrottleRCZero - posControl.escAndServoConfig->minthrottle);
        }

        updateAltitudeTargetFromClimbRate(rcClimbRate, CLIMB_RATE_UPDATE_SURFACE_TARGET);

        return true;
    }
    else {
        // Adjusting finished - reset desired position to stay exactly where pilot released the stick
        if (posControl.flags.isAdjustingAltitude) {
            updateAltitudeTargetFromClimbRate(0, CLIMB_RATE_UPDATE_SURFACE_TARGET);
        }

        return false;
    }
}

void setupMulticopterAltitudeController(void)
{
    throttleStatus_e throttleStatus = calculateThrottleStatus(posControl.rxConfig, posControl.flight3DConfig->deadband3d_throttle);

    if (posControl.navConfig->flags.use_thr_mid_for_althold) {
        altHoldThrottleRCZero = rcLookupThrottleMid();
    }
    else {
        // If throttle status is THROTTLE_LOW - use Thr Mid anyway
        if (throttleStatus == THROTTLE_LOW) {
            altHoldThrottleRCZero = rcLookupThrottleMid();
        }
        else {
            altHoldThrottleRCZero = rcCommand[THROTTLE];
        }
    }

    // Make sure we are able to satisfy the deadband
    altHoldThrottleRCZero = constrain(altHoldThrottleRCZero,
                                      posControl.escAndServoConfig->minthrottle + posControl.rcControlsConfig->alt_hold_deadband + 10,
                                      posControl.escAndServoConfig->maxthrottle - posControl.rcControlsConfig->alt_hold_deadband - 10);

    /* Force AH controller to initialize althold integral for pending takeoff on reset */
    if (throttleStatus == THROTTLE_LOW) {
        prepareForTakeoffOnReset = true;
    }
}

void resetMulticopterAltitudeController(void)
{
    navPidReset(&posControl.pids.vel[Z]);
    navPidReset(&posControl.pids.surface);
    filterResetPt1(&altholdThrottleFilterState, 0.0f);
    posControl.desiredState.vel.V.Z = posControl.actualState.vel.V.Z;   // Gradually transition from current climb
    posControl.rcAdjustment[THROTTLE] = 0;

    /* Prevent jump if activated with zero throttle - start with -50% throttle adjustment. That's obviously too much, but it will prevent jumping */
    if (prepareForTakeoffOnReset) {
        posControl.pids.vel[Z].integrator = -500.0f;
        prepareForTakeoffOnReset = false;
    }
}

static void applyMulticopterAltitudeController(uint32_t currentTime)
{
    static uint32_t previousTimePositionUpdate;         // Occurs @ altitude sensor update rate (max MAX_ALTITUDE_UPDATE_RATE_HZ)
    static uint32_t previousTimeUpdate;                 // Occurs @ looptime rate

    uint32_t deltaMicros = currentTime - previousTimeUpdate;
    previousTimeUpdate = currentTime;

    // If last position update was too long in the past - ignore it (likely restarting altitude controller)
    if (deltaMicros > HZ2US(MIN_POSITION_UPDATE_RATE_HZ)) {
        previousTimeUpdate = currentTime;
        previousTimePositionUpdate = currentTime;
        resetMulticopterAltitudeController();
        return;
    }

    // If we have an update on vertical position data - update velocity and accel targets
    if (posControl.flags.verticalPositionDataNew) {
        uint32_t deltaMicrosPositionUpdate = currentTime - previousTimePositionUpdate;
        previousTimePositionUpdate = currentTime;

        // Check if last correction was too log ago - ignore this update
        if (deltaMicrosPositionUpdate < HZ2US(MIN_POSITION_UPDATE_RATE_HZ)) {
            updateSurfaceTrackingAltitudeSetpoint(deltaMicrosPositionUpdate);
            updateAltitudeVelocityController_MC(deltaMicrosPositionUpdate);
            updateAltitudeThrottleController_MC(deltaMicrosPositionUpdate);
        }
        else {
            // due to some glitch position update has not occurred in time, reset altitude controller
            resetMulticopterAltitudeController();
        }

        // Indicate that information is no longer usable
        posControl.flags.verticalPositionDataConsumed = 1;
    }

    // Update throttle controller
    rcCommand[THROTTLE] = constrain((int16_t)posControl.navConfig->mc_hover_throttle + posControl.rcAdjustment[THROTTLE], posControl.escAndServoConfig->minthrottle, posControl.escAndServoConfig->maxthrottle);

    // Save processed throttle for future use
    rcCommandAdjustedThrottle = rcCommand[THROTTLE];
}

/*-----------------------------------------------------------
 * Adjusts desired heading from pilot's input
 *-----------------------------------------------------------*/
bool adjustMulticopterHeadingFromRCInput(void)
{
    if (ABS(rcCommand[YAW]) > posControl.rcControlsConfig->pos_hold_deadband) {
        // Can only allow pilot to set the new heading if doing PH, during RTH copter will target itself to home
        posControl.desiredState.yaw = posControl.actualState.yaw;

        return true;
    }
    else {
        return false;
    }
}

/*-----------------------------------------------------------
 * XY-position controller for multicopter aircraft
 *-----------------------------------------------------------*/
static filterStatePt1_t mcPosControllerAccFilterStateX, mcPosControllerAccFilterStateY;
static float lastAccelTargetX = 0.0f, lastAccelTargetY = 0.0f;

void resetMulticopterPositionController(void)
{
    int axis;
    for (axis = 0; axis < 2; axis++) {
        navPidReset(&posControl.pids.vel[axis]);
        posControl.rcAdjustment[axis] = 0;
        filterResetPt1(&mcPosControllerAccFilterStateX, 0.0f);
        filterResetPt1(&mcPosControllerAccFilterStateY, 0.0f);
        lastAccelTargetX = 0.0f;
        lastAccelTargetY = 0.0f;
    }
}

bool adjustMulticopterPositionFromRCInput(void)
{
    int16_t rcPitchAdjustment = applyDeadband(rcCommand[PITCH], posControl.rcControlsConfig->pos_hold_deadband);
    int16_t rcRollAdjustment = applyDeadband(rcCommand[ROLL], posControl.rcControlsConfig->pos_hold_deadband);

    if (rcPitchAdjustment || rcRollAdjustment) {
        // If mode is GPS_CRUISE, move target position, otherwise POS controller will passthru the RC input to ANGLE PID
        if (posControl.navConfig->flags.user_control_mode == NAV_GPS_CRUISE) {
            float rcVelX = rcPitchAdjustment * posControl.navConfig->max_manual_speed / 500;
            float rcVelY = rcRollAdjustment * posControl.navConfig->max_manual_speed / 500;

            // Rotate these velocities from body frame to to earth frame
            float neuVelX = rcVelX * posControl.actualState.cosYaw - rcVelY * posControl.actualState.sinYaw;
            float neuVelY = rcVelX * posControl.actualState.sinYaw + rcVelY * posControl.actualState.cosYaw;

            // Calculate new position target, so Pos-to-Vel P-controller would yield desired velocity
            posControl.desiredState.pos.V.X = posControl.actualState.pos.V.X + (neuVelX / posControl.pids.pos[X].param.kP);
            posControl.desiredState.pos.V.Y = posControl.actualState.pos.V.Y + (neuVelY / posControl.pids.pos[Y].param.kP);
        }

        return true;
    }
    else {
        // Adjusting finished - reset desired position to stay exactly where pilot released the stick
        if (posControl.flags.isAdjustingPosition) {
            t_fp_vector stopPosition;
            calculateMulticopterInitialHoldPosition(&stopPosition);
            setDesiredPosition(&stopPosition, 0, NAV_POS_UPDATE_XY);
        }

        return false;
    }
}

static float getVelocityHeadingAttenuationFactor(void)
{
    // In WP mode scale velocity if heading is different from bearing
    if (navGetCurrentStateFlags() & NAV_AUTO_WP) {
        int32_t headingError = constrain(wrap_18000(posControl.desiredState.yaw - posControl.actualState.yaw), -9000, 9000);
        float velScaling = cos_approx(CENTIDEGREES_TO_RADIANS(headingError));

        return constrainf(velScaling * velScaling, 0.05f, 1.0f);
    } else {
        return 1.0f;
    }
}

static float getVelocityExpoAttenuationFactor(float velTotal, float velMax)
{
    // Calculate factor of how velocity with applied expo is different from unchanged velocity
    float velScale = constrainf(velTotal / velMax, 0.01f, 1.0f);

    // posControl.navConfig->max_speed * ((velScale * velScale * velScale) * posControl.posResponseExpo + velScale * (1 - posControl.posResponseExpo)) / velTotal;
    // ((velScale * velScale * velScale) * posControl.posResponseExpo + velScale * (1 - posControl.posResponseExpo)) / velScale
    // ((velScale * velScale) * posControl.posResponseExpo + (1 - posControl.posResponseExpo));
    return 1.0f - posControl.posResponseExpo * (1.0f - (velScale * velScale));  // x^3 expo factor
}

static void updatePositionVelocityController_MC(void)
{
    float posErrorX = posControl.desiredState.pos.V.X - posControl.actualState.pos.V.X;
    float posErrorY = posControl.desiredState.pos.V.Y - posControl.actualState.pos.V.Y;

    // Calculate target velocity
    float newVelX = posErrorX * posControl.pids.pos[X].param.kP;
    float newVelY = posErrorY * posControl.pids.pos[Y].param.kP;

    // Get max speed from generic NAV (waypoint specific), don't allow to move slower than 0.5 m/s
    float maxSpeed = getActiveWaypointSpeed();

    // Scale velocity to respect max_speed
    float newVelTotal = sqrtf(sq(newVelX) + sq(newVelY));
    if (newVelTotal > maxSpeed) {
        newVelX = maxSpeed * (newVelX / newVelTotal);
        newVelY = maxSpeed * (newVelY / newVelTotal);
        newVelTotal = maxSpeed;
    }

    // Apply expo & attenuation if heading in wrong direction - turn first, accelerate later (effective only in WP mode)
    float velHeadFactor = getVelocityHeadingAttenuationFactor();
    float velExpoFactor = getVelocityExpoAttenuationFactor(newVelTotal, maxSpeed);
    posControl.desiredState.vel.V.X = newVelX * velHeadFactor * velExpoFactor;
    posControl.desiredState.vel.V.Y = newVelY * velHeadFactor * velExpoFactor;

#if defined(NAV_BLACKBOX)
    navDesiredVelocity[X] = constrain(lrintf(posControl.desiredState.vel.V.X), -32678, 32767);
    navDesiredVelocity[Y] = constrain(lrintf(posControl.desiredState.vel.V.Y), -32678, 32767);
#endif
}

static void updatePositionAccelController_MC(uint32_t deltaMicros, float maxAccelLimit)
{
    float velErrorX, velErrorY, newAccelX, newAccelY;

    // Calculate velocity error
    velErrorX = posControl.desiredState.vel.V.X - posControl.actualState.vel.V.X;
    velErrorY = posControl.desiredState.vel.V.Y - posControl.actualState.vel.V.Y;

    // Calculate XY-acceleration limit according to velocity error limit
    float accelLimitX, accelLimitY;
    float velErrorMagnitude = sqrtf(sq(velErrorX) + sq(velErrorY));
    if (velErrorMagnitude > 0.1f) {
        accelLimitX = maxAccelLimit / velErrorMagnitude * fabsf(velErrorX);
        accelLimitY = maxAccelLimit / velErrorMagnitude * fabsf(velErrorY);
    }
    else {
        accelLimitX = maxAccelLimit / 1.414213f;
        accelLimitY = accelLimitX;
    }

    // Apply additional jerk limiting of 1700 cm/s^3 (~100 deg/s), almost any copter should be able to achieve this rate
    // This will assure that we wont't saturate out LEVEL and RATE PID controller
    float maxAccelChange = US2S(deltaMicros) * 1700.0f;
    float accelLimitXMin = constrainf(lastAccelTargetX - maxAccelChange, -accelLimitX, +accelLimitX);
    float accelLimitXMax = constrainf(lastAccelTargetX + maxAccelChange, -accelLimitX, +accelLimitX);
    float accelLimitYMin = constrainf(lastAccelTargetY - maxAccelChange, -accelLimitY, +accelLimitY);
    float accelLimitYMax = constrainf(lastAccelTargetY + maxAccelChange, -accelLimitY, +accelLimitY);

    // TODO: Verify if we need jerk limiting after all

    // Apply PID with output limiting and I-term anti-windup
    // Pre-calculated accelLimit and the logic of navPidApply2 function guarantee that our newAccel won't exceed maxAccelLimit
    // Thus we don't need to do anything else with calculated acceleration
    newAccelX = navPidApply2(posControl.desiredState.vel.V.X, posControl.actualState.vel.V.X, US2S(deltaMicros), &posControl.pids.vel[X], accelLimitXMin, accelLimitXMax, false);
    newAccelY = navPidApply2(posControl.desiredState.vel.V.Y, posControl.actualState.vel.V.Y, US2S(deltaMicros), &posControl.pids.vel[Y], accelLimitYMin, accelLimitYMax, false);

    // Save last acceleration target
    lastAccelTargetX = newAccelX;
    lastAccelTargetY = newAccelY;

    // Apply LPF to jerk limited acceleration target
    float accelN = filterApplyPt1(newAccelX, &mcPosControllerAccFilterStateX, NAV_ACCEL_CUTOFF_FREQUENCY_HZ, US2S(deltaMicros));
    float accelE = filterApplyPt1(newAccelY, &mcPosControllerAccFilterStateY, NAV_ACCEL_CUTOFF_FREQUENCY_HZ, US2S(deltaMicros));

    // Rotate acceleration target into forward-right frame (aircraft)
    float accelForward = accelN * posControl.actualState.cosYaw + accelE * posControl.actualState.sinYaw;
    float accelRight = -accelN * posControl.actualState.sinYaw + accelE * posControl.actualState.cosYaw;

    // Calculate banking angles
    float desiredPitch = atan2_approx(accelForward, GRAVITY_CMSS);
    float desiredRoll = atan2_approx(accelRight * cos_approx(desiredPitch), GRAVITY_CMSS);

    int16_t maxBankAngle = DEGREES_TO_DECIDEGREES(posControl.navConfig->mc_max_bank_angle);
    posControl.rcAdjustment[ROLL] = constrain(RADIANS_TO_DECIDEGREES(desiredRoll), -maxBankAngle, maxBankAngle);
    posControl.rcAdjustment[PITCH] = constrain(RADIANS_TO_DECIDEGREES(desiredPitch), -maxBankAngle, maxBankAngle);
}

static void applyMulticopterPositionController(uint32_t currentTime)
{
    static uint32_t previousTimePositionUpdate;         // Occurs @ GPS update rate
    static uint32_t previousTimeUpdate;                 // Occurs @ looptime rate

    uint32_t deltaMicros = currentTime - previousTimeUpdate;
    previousTimeUpdate = currentTime;
    bool bypassPositionController;

    // We should passthrough rcCommand is adjusting position in GPS_ATTI mode
    bypassPositionController = (posControl.navConfig->flags.user_control_mode == NAV_GPS_ATTI) && posControl.flags.isAdjustingPosition;

    // If last call to controller was too long in the past - ignore it (likely restarting position controller)
    if (deltaMicros > HZ2US(MIN_POSITION_UPDATE_RATE_HZ)) {
        previousTimeUpdate = currentTime;
        previousTimePositionUpdate = currentTime;
        resetMulticopterPositionController();
        return;
    }

    // Apply controller only if position source is valid. In absence of valid pos sensor (GPS loss), we'd stick in forced ANGLE mode
    // and pilots input would be passed thru to PID controller
    if (posControl.flags.hasValidPositionSensor) {
        // If we have new position - update velocity and acceleration controllers
        if (posControl.flags.horizontalPositionDataNew) {
            uint32_t deltaMicrosPositionUpdate = currentTime - previousTimePositionUpdate;
            previousTimePositionUpdate = currentTime;

            if (!bypassPositionController) {
                // Update position controller
                if (deltaMicrosPositionUpdate < HZ2US(MIN_POSITION_UPDATE_RATE_HZ)) {
                    updatePositionVelocityController_MC();
                    updatePositionAccelController_MC(deltaMicrosPositionUpdate, NAV_ACCELERATION_XY_MAX);
                }
                else {
                    resetMulticopterPositionController();
                }
            }

            // Indicate that information is no longer usable
            posControl.flags.horizontalPositionDataConsumed = 1;
        }
    }
    else {
        /* No position data, disable automatic adjustment, rcCommand passthrough */
        posControl.rcAdjustment[PITCH] = 0;
        posControl.rcAdjustment[ROLL] = 0;
        bypassPositionController = true;
    }

    if (!bypassPositionController) {
        rcCommand[PITCH] = pidAngleToRcCommand(posControl.rcAdjustment[PITCH]);
        rcCommand[ROLL] = pidAngleToRcCommand(posControl.rcAdjustment[ROLL]);
    }
}

/*-----------------------------------------------------------
 * Multicopter land detector
 *-----------------------------------------------------------*/
bool isMulticopterLandingDetected(uint32_t * landingTimer, bool * hasHadSomeVelocity)
{
    uint32_t currentTime = micros();
    
    // When descend stage is activated velocity is ~0, so wait until we have descended faster than -25cm/s
    if (!*hasHadSomeVelocity && posControl.actualState.vel.V.Z < -25.0f) *hasHadSomeVelocity = true;

    // Average climb rate should be low enough
    bool verticalMovement = fabsf(posControl.actualState.vel.V.Z) > 25.0f;

    // check if we are moving horizontally
    bool horizontalMovement = posControl.actualState.velXY > 100.0f;

    // Throttle should be low enough
    // We use rcCommandAdjustedThrottle to keep track of NAV corrected throttle (isLandingDetected is executed
    // from processRx() and rcCommand at that moment holds rc input, not adjusted values from NAV core)
    bool minimalThrust = rcCommandAdjustedThrottle < posControl.navConfig->mc_min_fly_throttle;
    
    bool possibleLandingDetected = hasHadSomeVelocity && minimalThrust && !verticalMovement && !horizontalMovement;
    
    // If we have surface sensor (for example sonar) - use it to detect touchdown
    if (posControl.flags.hasValidSurfaceSensor && posControl.actualState.surface >= 0 && posControl.actualState.surfaceMin >= 0) {
        // TODO: Come up with a clever way to let sonar increase detection performance, not just add extra safety.
        // TODO: Out of range sonar may give reading that looks like we landed, find a way to check if sonar is healthy.
        // surfaceMin is our ground reference. If we are less than 5cm above the ground - we are likely landed
        possibleLandingDetected = possibleLandingDetected && posControl.actualState.surface <= (posControl.actualState.surfaceMin + 5.0f);
    }

    if (!possibleLandingDetected) {
        *landingTimer = currentTime;
        return false;
    }
    else {
        return ((currentTime - *landingTimer) > (LAND_DETECTOR_TRIGGER_TIME_MS * 1000)) ? true : false;
    }
}

/*-----------------------------------------------------------
 * Multicopter emergency landing
 *-----------------------------------------------------------*/
static void applyMulticopterEmergencyLandingController(uint32_t currentTime)
{
    static uint32_t previousTimeUpdate;
    static uint32_t previousTimePositionUpdate;
    uint32_t deltaMicros = currentTime - previousTimeUpdate;
    previousTimeUpdate = currentTime;

    /* Attempt to stabilise */
    rcCommand[ROLL] = 0;
    rcCommand[PITCH] = 0;
    rcCommand[YAW] = 0;

    if (posControl.flags.hasValidAltitudeSensor) {
        /* We have an altitude reference, apply AH-based landing controller */

        // If last position update was too long in the past - ignore it (likely restarting altitude controller)
        if (deltaMicros > HZ2US(MIN_POSITION_UPDATE_RATE_HZ)) {
            previousTimeUpdate = currentTime;
            previousTimePositionUpdate = currentTime;
            resetMulticopterAltitudeController();
            return;
        }

        if (posControl.flags.verticalPositionDataNew) {
            uint32_t deltaMicrosPositionUpdate = currentTime - previousTimePositionUpdate;
            previousTimePositionUpdate = currentTime;

            // Check if last correction was too log ago - ignore this update
            if (deltaMicrosPositionUpdate < HZ2US(MIN_POSITION_UPDATE_RATE_HZ)) {
                updateAltitudeTargetFromClimbRate(-1.0f * posControl.navConfig->emerg_descent_rate, CLIMB_RATE_RESET_SURFACE_TARGET);
                updateAltitudeVelocityController_MC(deltaMicrosPositionUpdate);
                updateAltitudeThrottleController_MC(deltaMicrosPositionUpdate);
            }
            else {
                // due to some glitch position update has not occurred in time, reset altitude controller
                resetMulticopterAltitudeController();
            }

            // Indicate that information is no longer usable
            posControl.flags.verticalPositionDataConsumed = 1;
        }

        // Update throttle controller
        rcCommand[THROTTLE] = constrain((int16_t)posControl.navConfig->mc_hover_throttle + posControl.rcAdjustment[THROTTLE], posControl.escAndServoConfig->minthrottle, posControl.escAndServoConfig->maxthrottle);
    }
    else {
        /* Sensors has gone haywire, attempt to land regardless */
        failsafeConfig_t * failsafeConfig = getActiveFailsafeConfig();

        if (failsafeConfig) {
            rcCommand[THROTTLE] = failsafeConfig->failsafe_throttle;
        }
        else {
            rcCommand[THROTTLE] = posControl.escAndServoConfig->minthrottle;
        }
    }
}

/*-----------------------------------------------------------
 * Calculate loiter target based on current position and velocity
 *-----------------------------------------------------------*/
void calculateMulticopterInitialHoldPosition(t_fp_vector * pos)
{
    float stoppingDistanceX = posControl.actualState.vel.V.X * posControl.posDecelerationTime;
    float stoppingDistanceY = posControl.actualState.vel.V.Y * posControl.posDecelerationTime;

    pos->V.X = posControl.actualState.pos.V.X + stoppingDistanceX;
    pos->V.Y = posControl.actualState.pos.V.Y + stoppingDistanceY;
}

void resetMulticopterHeadingController(void)
{
    updateMagHoldHeading(CENTIDEGREES_TO_DEGREES(posControl.actualState.yaw));
}

static void applyMulticopterHeadingController(void)
{
    updateMagHoldHeading(CENTIDEGREES_TO_DEGREES(posControl.desiredState.yaw));
}

void applyMulticopterNavigationController(navigationFSMStateFlags_t navStateFlags, uint32_t currentTime)
{
    if (navStateFlags & NAV_CTL_EMERG) {
        applyMulticopterEmergencyLandingController(currentTime);
    }
    else {
        if (navStateFlags & NAV_CTL_ALT)
            applyMulticopterAltitudeController(currentTime);

        if (navStateFlags & NAV_CTL_POS)
            applyMulticopterPositionController(currentTime);

        if (navStateFlags & NAV_CTL_YAW)
            applyMulticopterHeadingController();
    }
}
#endif  // NAV
