#include <AP_HAL/AP_HAL.h>

#include "AP_NavEKF2_core.h"
#include <AP_DAL/AP_DAL.h>
#include <AP_AHRS/AP_AHRS.h>
#include <GCS_MAVLink/GCS.h>

extern const AP_HAL::HAL& hal;


// Check basic filter health metrics and return a consolidated health status
bool NavEKF2_core::healthy(void) const
{
    uint16_t faultInt;
    getFilterFaults(faultInt);
    if (faultInt > 0) {
        return false;
    }
    if (velTestRatio > 1 && posTestRatio > 1 && hgtTestRatio > 1) {
        // all three metrics being above 1 means the filter is
        // extremely unhealthy.
        return false;
    }
    // Give the filter a second to settle before use
    if ((imuSampleTime_ms - ekfStartTime_ms) < 1000 ) {
        return false;
    }
    // position and height innovations must be within limits when on-ground and in a static mode of operation
    ftype horizErrSq = sq(innovVelPos[3]) + sq(innovVelPos[4]);
    if (onGround && (PV_AidingMode == AID_NONE) && ((horizErrSq > 1.0f) || (fabsF(hgtInnovFiltState) > 1.0f))) {
        return false;
    }

    // all OK
    return true;
}

// Return a consolidated error score where higher numbers represent larger errors
// Intended to be used by the front-end to determine which is the primary EKF
ftype NavEKF2_core::errorScore() const
{
    ftype score = 0.0f;
    if (tiltAlignComplete && yawAlignComplete) {
        // Check GPS fusion performance
        score = MAX(score, 0.5f * (velTestRatio + posTestRatio));
        // Check altimeter fusion performance
        score = MAX(score, hgtTestRatio);
        // Check attitude corrections
        const ftype tiltErrThreshold = 0.05f;
        score = MAX(score, tiltErrFilt / tiltErrThreshold);
    }
    return score;
}

// provides the height limit to be observed by the control loops
// returns false if no height limiting is required
// this is needed to ensure the vehicle does not fly too high when using optical flow navigation
bool NavEKF2_core::getHeightControlLimit(float &height) const
{
    // only ask for limiting if we are doing optical flow only navigation
    if (frontend->_fusionModeGPS == 3 && (PV_AidingMode == AID_RELATIVE) && flowDataValid) {
        // If are doing optical flow nav, ensure the height above ground is within range finder limits after accounting for vehicle tilt and control errors
#if AP_RANGEFINDER_ENABLED
        const auto *_rng = dal.rangefinder();
        if (_rng == nullptr) {
            // we really, really shouldn't be here.
            return false;
        }
        height = MAX(float(_rng->max_distance_orient(ROTATION_PITCH_270)) * 0.7f - 1.0f, 1.0f);
#else
        return false;
#endif
        // If we are are not using the range finder as the height reference, then compensate for the difference between terrain and EKF origin
        if (frontend->_altSource != 1) {
            height -= terrainState;
        }
        return true;
    } else {
        return false;
    }
}


// return the Euler roll, pitch and yaw angle in radians
void NavEKF2_core::getEulerAngles(Vector3f &euler) const
{
    outputDataNew.quat.to_euler(euler.x, euler.y, euler.z);
    euler = euler - dal.get_trim();
}

// return body axis gyro bias estimates in rad/sec
void NavEKF2_core::getGyroBias(Vector3f &gyroBias) const
{
    if (dtEkfAvg < 1e-6f) {
        gyroBias.zero();
        return;
    }
    gyroBias = stateStruct.gyro_bias.tofloat() / dtEkfAvg;
}

// return body axis gyro scale factor error as a percentage
void NavEKF2_core::getGyroScaleErrorPercentage(Vector3f &gyroScale) const
{
    if (!statesInitialised) {
        gyroScale.x = gyroScale.y = gyroScale.z = 0;
        return;
    }
    gyroScale.x = 100.0f/stateStruct.gyro_scale.x - 100.0f;
    gyroScale.y = 100.0f/stateStruct.gyro_scale.y - 100.0f;
    gyroScale.z = 100.0f/stateStruct.gyro_scale.z - 100.0f;
}

// return the transformation matrix from XYZ (body) to NED axes
void NavEKF2_core::getRotationBodyToNED(Matrix3f &mat) const
{
    outputDataNew.quat.rotation_matrix(mat);
    mat = mat * dal.get_rotation_vehicle_body_to_autopilot_body();
}

// return the quaternions defining the rotation from NED to XYZ (body) axes
void NavEKF2_core::getQuaternion(Quaternion& ret) const
{
    ret = outputDataNew.quat.tofloat();
}

// return the amount of yaw angle change due to the last yaw angle reset in radians
// returns the time of the last yaw angle reset or 0 if no reset has ever occurred
uint32_t NavEKF2_core::getLastYawResetAngle(float &yawAng) const
{
    yawAng = yawResetAngle;
    return lastYawReset_ms;
}

// return the amount of NE position change due to the last position reset in metres
// returns the time of the last reset or 0 if no reset has ever occurred
uint32_t NavEKF2_core::getLastPosNorthEastReset(Vector2f &pos) const
{
    pos = posResetNE.tofloat();
    return lastPosReset_ms;
}

// return the amount of vertical position change due to the last vertical position reset in metres
// returns the time of the last reset or 0 if no reset has ever occurred
uint32_t NavEKF2_core::getLastPosDownReset(float &posD) const
{
    posD = posResetD;
    return lastPosResetD_ms;
}

// return the amount of NE velocity change due to the last velocity reset in metres/sec
// returns the time of the last reset or 0 if no reset has ever occurred
uint32_t NavEKF2_core::getLastVelNorthEastReset(Vector2f &vel) const
{
    vel = velResetNE.tofloat();
    return lastVelReset_ms;
}

// return the NED wind speed estimates in m/s (positive is air moving in the direction of the axis)
void NavEKF2_core::getWind(Vector3f &wind) const
{
    wind.x = stateStruct.wind_vel.x;
    wind.y = stateStruct.wind_vel.y;
    wind.z = 0.0f; // currently don't estimate this
}


// return the NED velocity of the body frame origin in m/s
//
void NavEKF2_core::getVelNED(Vector3f &vel) const
{
    // correct for the IMU position offset (EKF calculations are at the IMU)
    vel = (outputDataNew.velocity + velOffsetNED).tofloat();
}

// return estimate of true airspeed vector in body frame in m/s
// returns false if estimate is unavailable
bool NavEKF2_core::getAirSpdVec(Vector3f &vel) const
{
    if (PV_AidingMode == AID_NONE) {
        return false;
    }
    vel = (outputDataNew.velocity + velOffsetNED).tofloat();
    if (!inhibitWindStates) {
        vel.x -= stateStruct.wind_vel.x;
        vel.y -= stateStruct.wind_vel.y;
    }
    Matrix3f Tnb; // rotation from nav to body frame
    outputDataNew.quat.inverse().rotation_matrix(Tnb);
    vel = Tnb * vel;
    return true;
}

// Return the rate of change of vertical position in the down direction (dPosD/dt) of the body frame origin in m/s
float NavEKF2_core::getPosDownDerivative(void) const
{
    // return the value calculated from a complementary filter applied to the EKF height and vertical acceleration
    // correct for the IMU offset (EKF calculations are at the IMU)
    return vertCompFiltState.vel + velOffsetNED.z;
}

// return the Z-accel bias estimate in m/s^2
void NavEKF2_core::getAccelZBias(float &zbias) const {
    if (dtEkfAvg > 0) {
        zbias = stateStruct.accel_zbias / dtEkfAvg;
    } else {
        zbias = 0;
    }
}

// Write the last estimated NE position of the body frame origin relative to the reference point (m).
// Return true if the estimate is valid
bool NavEKF2_core::getPosNE(Vector2p &posNE) const
{
    // There are three modes of operation, absolute position (GPS fusion), relative position (optical flow fusion) and constant position (no position estimate available)
    if (PV_AidingMode != AID_NONE) {
        // This is the normal mode of operation where we can use the EKF position states
        // correct for the IMU offset (EKF calculations are at the IMU)
        posNE.x = outputDataNew.position.x + posOffsetNED.x;
        posNE.y = outputDataNew.position.y + posOffsetNED.y;
        return true;

    } else {
        // In constant position mode the EKF position states are at the origin, so we cannot use them as a position estimate
        if(validOrigin) {
            if ((dal.gps().status(dal.gps().primary_sensor()) >= AP_DAL_GPS::GPS_OK_FIX_2D)) {
                // If the origin has been set and we have GPS, then return the GPS position relative to the origin
                const Location &gpsloc = dal.gps().location();
                posNE = EKF_origin.get_distance_NE_postype(gpsloc);
                return false;
            } else if (rngBcnAlignmentStarted) {
                // If we are attempting alignment using range beacon data, then report the position
                posNE.x = receiverPos.x;
                posNE.y = receiverPos.y;
                return false;
            } else {
                // If no GPS fix is available, all we can do is provide the last known position
                posNE.x = outputDataNew.position.x;
                posNE.y = outputDataNew.position.y;
                return false;
            }
        } else {
            // If the origin has not been set, then we have no means of providing a relative position
            posNE.x = 0.0f;
            posNE.y = 0.0f;
            return false;
        }
    }
    return false;
}

// Write the last calculated D position of the body frame origin relative to the EKF origin (m).
// Return true if the estimate is valid
bool NavEKF2_core::getPosD(postype_t &posD) const
{
    // The EKF always has a height estimate regardless of mode of operation
    // Correct for the IMU offset in body frame (EKF calculations are at the IMU)
    // Also correct for changes to the origin height
    if ((frontend->_originHgtMode & (1<<2)) == 0) {
        // Any sensor height drift corrections relative to the WGS-84 reference are applied to the origin.
        posD = outputDataNew.position.z + posOffsetNED.z;
    } else {
        // The origin height is static and corrections are applied to the local vertical position
        // so that height returned by getLLH() = height returned by getOriginLLH - posD
        posD = outputDataNew.position.z + posOffsetNED.z + 0.01f * (float)EKF_origin.alt - (float)ekfGpsRefHgt;
    }

    // Return the current height solution status
    return filterStatus.flags.vert_pos;

}

// return the estimated height of body frame origin above ground level
bool NavEKF2_core::getHAGL(float &HAGL) const
{
    HAGL = terrainState - outputDataNew.position.z - posOffsetNED.z;
    // If we know the terrain offset and altitude, then we have a valid height above ground estimate
    return !hgtTimeout && gndOffsetValid && healthy();
}


// Return the last calculated latitude, longitude and height of the body frame origin in WGS-84
// If a calculated location isn't available, return a raw GPS measurement
// The status will return true if a calculation or raw measurement is available
// The getFilterStatus() function provides a more detailed description of data health and must be checked if data is to be used for flight control
bool NavEKF2_core::getLLH(Location &loc) const
{
    const auto &gps = dal.gps();
    Location origin;
    postype_t posD;

    if(getPosD(posD) && getOriginLLH(origin)) {
        // Altitude returned is an absolute altitude relative to the WGS-84 spherioid
        loc.set_alt_cm(origin.alt - posD*100, Location::AltFrame::ABSOLUTE);

        // there are three modes of operation, absolute position (GPS fusion), relative position (optical flow fusion) and constant position (no aiding)
        if (filterStatus.flags.horiz_pos_abs || filterStatus.flags.horiz_pos_rel) {
            loc.lat = EKF_origin.lat;
            loc.lng = EKF_origin.lng;
            // correct for IMU offset (EKF calculations are at the IMU position)
            loc.offset((outputDataNew.position.x + posOffsetNED.x), (outputDataNew.position.y + posOffsetNED.y));
            return true;
        } else {
            // we could be in constant position mode  because the vehicle has taken off without GPS, or has lost GPS
            // in this mode we cannot use the EKF states to estimate position so will return the best available data
            if ((gps.status() >= AP_DAL_GPS::GPS_OK_FIX_2D)) {
                // we have a GPS position fix to return
                const Location &gpsloc = gps.location();
                loc.lat = gpsloc.lat;
                loc.lng = gpsloc.lng;
                return true;
            } else {
                // if no GPS fix, provide last known position before entering the mode
                // correct for IMU offset (EKF calculations are at the IMU position)
                loc.lat = EKF_origin.lat;
                loc.lng = EKF_origin.lng;
                if (PV_AidingMode == AID_NONE) {
                    loc.offset((lastKnownPositionNE.x + posOffsetNED.x), (lastKnownPositionNE.y + posOffsetNED.y));
                } else {
                    loc.offset((outputDataNew.position.x + posOffsetNED.x), (outputDataNew.position.y + posOffsetNED.y));
                }
                return false;
            }
        }
    } else {
        // If no origin has been defined for the EKF, then we cannot use its position states so return a raw
        // GPS reading if available and return false
        if ((gps.status() >= AP_DAL_GPS::GPS_OK_FIX_3D)) {
            loc = gps.location();
        }
        return false;
    }
}


// return the horizontal speed limit in m/s set by optical flow sensor limits
// return the scale factor to be applied to navigation velocity gains to compensate for increase in velocity noise with height when using optical flow
void NavEKF2_core::getEkfControlLimits(float &ekfGndSpdLimit, float &ekfNavVelGainScaler) const
{
    if (PV_AidingMode == AID_RELATIVE) {
        // allow 1.0 rad/sec margin for angular motion
        ekfGndSpdLimit = MAX((frontend->_maxFlowRate - 1.0f), 0.0f) * MAX((terrainState - stateStruct.position[2]), rngOnGnd);
        // use standard gains up to 5.0 metres height and reduce above that
        ekfNavVelGainScaler = 4.0f / MAX((terrainState - stateStruct.position[2]),4.0f);
    } else {
        ekfGndSpdLimit = 400.0f; //return 80% of max filter speed
        ekfNavVelGainScaler = 1.0f;
    }
}


// return the LLH location of the filters NED origin
bool NavEKF2_core::getOriginLLH(Location &loc) const
{
    if (validOrigin) {
        loc = EKF_origin;
        // report internally corrected reference height if enabled
        if ((frontend->_originHgtMode & (1<<2)) == 0) {
            loc.alt = (int32_t)(100.0f * (float)ekfGpsRefHgt);
        }
    }
    return validOrigin;
}

// return earth magnetic field estimates in measurement units / 1000
void NavEKF2_core::getMagNED(Vector3f &magNED) const
{
    magNED = (stateStruct.earth_magfield * 1000.0).tofloat();
}

// return body magnetic field estimates in measurement units / 1000
void NavEKF2_core::getMagXYZ(Vector3f &magXYZ) const
{
    magXYZ = (stateStruct.body_magfield*1000.0).tofloat();
}

// return magnetometer offsets
// return true if offsets are valid
bool NavEKF2_core::getMagOffsets(uint8_t mag_idx, Vector3f &magOffsets) const
{
    const auto &compass = dal.compass();
    if (!compass.available()) {
        return false;
    }

    // compass offsets are valid if we have finalised magnetic field initialisation, magnetic field learning is not prohibited,
    // primary compass is valid and state variances have converged
    const ftype maxMagVar = 5E-6f;
    bool variancesConverged = (P[19][19] < maxMagVar) && (P[20][20] < maxMagVar) && (P[21][21] < maxMagVar);
    if ((mag_idx == magSelectIndex) &&
            finalInflightMagInit &&
            !inhibitMagStates &&
            compass.healthy(magSelectIndex) &&
            variancesConverged) {
        magOffsets = compass.get_offsets(magSelectIndex) - (stateStruct.body_magfield*1000.0).tofloat();
        return true;
    } else {
        magOffsets = compass.get_offsets(magSelectIndex);
        return false;
    }
}

// return the innovations for the NED Pos, NED Vel, XYZ Mag and Vtas measurements
bool NavEKF2_core::getInnovations(Vector3f &velInnov, Vector3f &posInnov, Vector3f &magInnov, float &tasInnov, float &yawInnov) const
{
    velInnov.x = innovVelPos[0];
    velInnov.y = innovVelPos[1];
    velInnov.z = innovVelPos[2];
    posInnov.x = innovVelPos[3];
    posInnov.y = innovVelPos[4];
    posInnov.z = innovVelPos[5];
    magInnov.x = 1e3f*innovMag[0]; // Convert back to sensor units
    magInnov.y = 1e3f*innovMag[1]; // Convert back to sensor units
    magInnov.z = 1e3f*innovMag[2]; // Convert back to sensor units
    tasInnov   = innovVtas;
    yawInnov   = innovYaw;

    return true;
}

// return the innovation consistency test ratios for the velocity, position, magnetometer and true airspeed measurements
// this indicates the amount of margin available when tuning the various error traps
// also return the delta in position due to the last position reset
bool NavEKF2_core::getVariances(float &velVar, float &posVar, float &hgtVar, Vector3f &magVar, float &tasVar, Vector2f &offset) const
{
    velVar   = sqrtF(velTestRatio);
    posVar   = sqrtF(posTestRatio);
    hgtVar   = sqrtF(hgtTestRatio);
    // If we are using simple compass yaw fusion, populate all three components with the yaw test ratio to provide an equivalent output
    magVar.x = sqrtF(MAX(magTestRatio.x,yawTestRatio));
    magVar.y = sqrtF(MAX(magTestRatio.y,yawTestRatio));
    magVar.z = sqrtF(MAX(magTestRatio.z,yawTestRatio));
    tasVar   = sqrtF(tasTestRatio);
    offset   = posResetNE.tofloat();

    return true;
}


/*
return the filter fault status as a bitmasked integer
 0 = quaternions are NaN
 1 = velocities are NaN
 2 = badly conditioned X magnetometer fusion
 3 = badly conditioned Y magnetometer fusion
 4 = badly conditioned Z magnetometer fusion
 5 = badly conditioned airspeed fusion
 6 = badly conditioned synthetic sideslip fusion
 7 = filter is not initialised
*/
void  NavEKF2_core::getFilterFaults(uint16_t &faults) const
{
    faults = (stateStruct.quat.is_nan()<<0 |
              stateStruct.velocity.is_nan()<<1 |
              faultStatus.bad_xmag<<2 |
              faultStatus.bad_ymag<<3 |
              faultStatus.bad_zmag<<4 |
              faultStatus.bad_airspeed<<5 |
              faultStatus.bad_sideslip<<6 |
              !statesInitialised<<7);
}

/*
return filter timeout status as a bitmasked integer
 0 = position measurement timeout
 1 = velocity measurement timeout
 2 = height measurement timeout
 3 = magnetometer measurement timeout
 4 = true airspeed measurement timeout
 5 = unassigned
 6 = unassigned
 7 = unassigned
*/
// Return the navigation filter status message
void  NavEKF2_core::getFilterStatus(nav_filter_status &status) const
{
    status = filterStatus;
}

/*
return filter gps quality check status
*/
void  NavEKF2_core::getFilterGpsStatus(nav_gps_status &faults) const
{
    // init return value
    faults.value = 0;

    // set individual flags
    faults.flags.bad_sAcc           = gpsCheckStatus.bad_sAcc; // reported speed accuracy is insufficient
    faults.flags.bad_hAcc           = gpsCheckStatus.bad_hAcc; // reported horizontal position accuracy is insufficient
    faults.flags.bad_vAcc           = gpsCheckStatus.bad_vAcc; // reported vertical position accuracy is insufficient
    faults.flags.bad_yaw            = gpsCheckStatus.bad_yaw; // EKF heading accuracy is too large for GPS use
    faults.flags.bad_sats           = gpsCheckStatus.bad_sats; // reported number of satellites is insufficient
    faults.flags.bad_horiz_drift    = gpsCheckStatus.bad_horiz_drift; // GPS horizontal drift is too large to start using GPS (check assumes vehicle is static)
    faults.flags.bad_hdop           = gpsCheckStatus.bad_hdop; // reported HDoP is too large to start using GPS
    faults.flags.bad_vert_vel       = gpsCheckStatus.bad_vert_vel; // GPS vertical speed is too large to start using GPS (check assumes vehicle is static)
    faults.flags.bad_fix            = gpsCheckStatus.bad_fix; // The GPS cannot provide the 3D fix required
    faults.flags.bad_horiz_vel      = gpsCheckStatus.bad_horiz_vel; // The GPS horizontal speed is excessive (check assumes the vehicle is static)
}

#if HAL_GCS_ENABLED
// send an EKF_STATUS message to GCS
void NavEKF2_core::send_status_report(GCS_MAVLINK &link) const
{
    // prepare flags
    uint16_t flags = 0;
    if (filterStatus.flags.attitude) {
        flags |= EKF_ATTITUDE;
    }
    if (filterStatus.flags.horiz_vel) {
        flags |= EKF_VELOCITY_HORIZ;
    }
    if (filterStatus.flags.vert_vel) {
        flags |= EKF_VELOCITY_VERT;
    }
    if (filterStatus.flags.horiz_pos_rel) {
        flags |= EKF_POS_HORIZ_REL;
    }
    if (filterStatus.flags.horiz_pos_abs) {
        flags |= EKF_POS_HORIZ_ABS;
    }
    if (filterStatus.flags.vert_pos) {
        flags |= EKF_POS_VERT_ABS;
    }
    if (filterStatus.flags.terrain_alt) {
        flags |= EKF_POS_VERT_AGL;
    }
    if (filterStatus.flags.const_pos_mode) {
        flags |= EKF_CONST_POS_MODE;
    }
    if (filterStatus.flags.pred_horiz_pos_rel) {
        flags |= EKF_PRED_POS_HORIZ_REL;
    }
    if (filterStatus.flags.pred_horiz_pos_abs) {
        flags |= EKF_PRED_POS_HORIZ_ABS;
    }
    if (!filterStatus.flags.initalized) {
        flags |= EKF_UNINITIALIZED;
    }

    // get variances
    float velVar = 0, posVar = 0, hgtVar = 0, tasVar = 0;
    Vector3f magVar;
    Vector2f offset;
    getVariances(velVar, posVar, hgtVar, magVar, tasVar, offset);

    const float mag_max = fmaxF(fmaxF(magVar.x,magVar.y),magVar.z);

    // Only report range finder normalised innovation levels if the EKF needs the data for primary
    // height estimation or optical flow operation. This prevents false alarms at the GCS if a
    // range finder is fitted for other applications
    float temp;
    if (((frontend->_useRngSwHgt > 0) && activeHgtSource == HGT_SOURCE_RNG) || (PV_AidingMode == AID_RELATIVE && flowDataValid)) {
        temp = sqrtF(auxRngTestRatio);
    } else {
        temp = 0.0f;
    }

    // send message
    mavlink_msg_ekf_status_report_send(link.get_chan(), flags, velVar, posVar, hgtVar, mag_max, temp, tasVar);
}
#endif  // HAL_GCS_ENABLED

// report the reason for why the backend is refusing to initialise
const char *NavEKF2_core::prearm_failure_reason(void) const
{
    if (gpsGoodToAlign) {
        // we are not failing
        return nullptr;
    }
    return prearm_fail_string;
}


// report the number of frames lapsed since the last state prediction
// this is used by other instances to level load
uint8_t NavEKF2_core::getFramesSincePredict(void) const
{
    return framesSincePredict;
}

// return true when external nav data is also being used as a yaw observation
bool NavEKF2_core::isExtNavUsedForYaw() const
{
    return extNavUsedForYaw;
}
