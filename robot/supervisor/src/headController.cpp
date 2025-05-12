#include "headController.h"
#include "anki/cozmo/robot/hal.h"
#include "anki/cozmo/robot/DAS.h"
#include "clad/types/motorTypes.h"
#include "coretech/common/shared/math/radians.h"
#include "velocityProfileGenerator.h"
#include "anki/cozmo/robot/logging.h"
#include "messages.h"
#include "imuFilter.h"
#include "proxSensors.h"
#include "anki/cozmo/shared/factory/emrHelper.h"
#include <math.h>

#define DEBUG_HEAD_CONTROLLER 0

// If defined, angle is calibrated while power is still being applied, versus a short period of time after motor is "relaxed"
//#define CALIB_WHILE_APPLYING_POWER

namespace Anki {
namespace Vector {
namespace HeadController {

    namespace {

      // Used when calling SetDesiredAngle with just an angle:
      const f32 DEFAULT_START_ACCEL_FRAC = 0.1f;
      const f32 DEFAULT_END_ACCEL_FRAC   = 0.1f;

      // Currently applied power
      f32 power_ = 0;

      // Head angle control vars
      // 0 radians == looking straight ahead
      Radians currentAngle_ = 0.f;
      Radians desiredAngle_ = 0.f;
      f32 currDesiredAngle_ = 0.f;
      f32 angleError_ = 0.f;
      f32 angleErrorSum_ = 0.f;
      f32 prevAngleError_ = 0.f;
      f32 prevHalPos_ = 0.f;
      bool inPosition_  = true;

      const f32 SPEED_FILTERING_COEFF = 0.5f;

#ifdef SIMULATOR
      f32 Kp_ = 20.f; // proportional control constant
      f32 Kd_ = 0.f;  // derivative control constant
      f32 Ki_ = 0.1f; // integral control constant
      f32 MAX_ERROR_SUM = 2.f;
#else
      f32 Kp_ = 4.f;  // proportional control constant
      f32 Kd_ = 4000.f;  // derivative control constant
      f32 Ki_ = 0.03f; // integral control constant
      f32 MAX_ERROR_SUM = 15.f;
#endif

      // Motor burnout protection
      u32 potentialBurnoutStartTime_ms_ = 0;
      const f32 BURNOUT_POWER_THRESH = Ki_ * MAX_ERROR_SUM + Kp_ * HEAD_ANGLE_TOL;
      const u32 BURNOUT_TIME_THRESH_MS = 2000.f;

      // Current speed
      f32 radSpeed_ = 0.f;

      // Speed and acceleration params
      f32 maxSpeedRad_ = 1.0f;
      f32 accelRad_ = 2.0f;

      // For generating position and speed profile
      VelocityProfileGenerator vpg_;


      // Calibration parameters
      typedef enum {
        HCS_IDLE,
        HCS_LOWER_HEAD,
        HCS_WAIT_FOR_STOP,
        HCS_SET_CURR_ANGLE,
        HCS_COMPLETE
      } HeadCalibState;

      HeadCalibState calState_ = HCS_IDLE;

      // Whether or not head is calibrated
      bool isCalibrated_ = false;

      // If this is the first time calibrating, repeat until it's done.
      // Shouldn't proceed until calibration is complete.
      bool firstCalibration_ = true;
      
      // Keep track of why we started a calibration, so that we can report this to DAS once the calibration completes
      MotorCalibrationReason calibrationReason_ = MotorCalibrationReason::Startup;

      // Last time head movement was detected
      u32 lastHeadMovedTime_ms = 0;

      // Parameters for determining if head is being messed with during
      // calibration in which case calibration is retried or aborted
      f32 lowHeadAngleDuringCalib_rad_;
      const f32 UPWARDS_HEAD_MOTION_FOR_CALIB_ABORT_RAD = DEG_TO_RAD(5.f);

      u32 lastInPositionTime_ms_;
      const u32 IN_POSITION_TIME_MS = 200;

      const f32 MAX_HEAD_CONSIDERED_STOPPED_RAD_PER_SEC = 0.001f;

      const u32 HEAD_STOP_TIME = 200;  // ms

      bool enable_ = true;

      // If disabled, head motor is automatically re-enabled at this time if non-zero.
      u32 enableAtTime_ms_ = 0;

      // If enableAtTime_ms_ is non-zero, this is the time beyond current time
      // that the motor will be re-enabled if the head is not moving.
      const u32 REENABLE_TIMEOUT_MS = 2000;

      // Bracing for impact
      // Lowers head quickly during which time it ignores any new commands
      bool bracing_ = false;
      const f32 BRACING_POWER = -0.65;

      // True if encoder was reported as invalid by HAL and has not been calibrated since
      u32 encoderInvalidStartTime_ms_ = 0;

    } // "private" members


    void Enable()
    {
      if (!enable_) {
        enable_ = true;
        enableAtTime_ms_ = 0;  // Reset auto-enable trigger time
        power_ = 0;
        HAL::MotorSetPower(MotorID::MOTOR_HEAD, power_);
      }
    }

    void Disable(bool autoReEnable)
    {
      if(enable_) {
        enable_ = false;

        inPosition_ = true;
        prevAngleError_ = 0;
        angleErrorSum_ = 0.f;

        potentialBurnoutStartTime_ms_ = 0;
        bracing_ = false;

        if (!IsCalibrating()) {
          power_ = 0;
          HAL::MotorSetPower(MotorID::MOTOR_HEAD, power_);
        }
      }

      enableAtTime_ms_ = 0;
      if (autoReEnable) {
        enableAtTime_ms_ = HAL::GetTimeStamp() + REENABLE_TIMEOUT_MS;
      }
    }


    void StartCalibrationRoutine(const bool autoStarted, const MotorCalibrationReason& reason)
    {
      calibrationReason_ = reason;
      potentialBurnoutStartTime_ms_ = 0;
      calState_ = HCS_LOWER_HEAD;
      isCalibrated_ = false;
      inPosition_ = false;
    
      Messages::SendMotorCalibrationMsg(MotorID::MOTOR_HEAD, true, autoStarted);
    }

    bool IsCalibrated()
    {
      return isCalibrated_;
    }

    bool IsCalibrating()
    {
      return calState_ != HCS_IDLE;
    }

    void ClearCalibration()
    {
      isCalibrated_ = false;
    }

    void ResetLowAnglePosition()
    {
      currentAngle_ = MIN_HEAD_ANGLE;
      HAL::MotorResetPosition(MotorID::MOTOR_HEAD);
      prevHalPos_ = HAL::MotorGetPosition(MotorID::MOTOR_HEAD);
      isCalibrated_ = true;
    }

    bool IsMoving()
    {
      return (ABS(radSpeed_) > MAX_HEAD_CONSIDERED_STOPPED_RAD_PER_SEC);
    }

    void Stop()
    {
      SetAngularVelocity(0);
    }

    void OnMotorCalibrated()
    {
      const auto prevAngle = currentAngle_;
      ResetLowAnglePosition();
      
      // How badly out of calibration was the motor?
      const float angleError_deg = (prevAngle - currentAngle_).getDegrees();

      AnkiInfo("HeadController.Calibrated",
               "Head calibrated for reason %s. Calibration error was %.3f deg.",
               EnumToString(calibrationReason_),
               angleError_deg);
      
      // Log DAS, but not if this is a calibration due to normal startup
      const u32 timeUncalibrated_ms = encoderInvalidStartTime_ms_ > 0 ? HAL::GetTimeStamp() - encoderInvalidStartTime_ms_ : 0;
      if (calibrationReason_ != MotorCalibrationReason::Startup) {
        DASMSG(head_motor_calibrated,
               "head_motor_calibrated",
               "The robot's head motor has just completed a calibration");
        DASMSG_SET(s1, EnumToString(calibrationReason_), "Reason for triggering calibration");
        DASMSG_SET(i1, 1000.f * angleError_deg, "Angular error (millidegrees). This represents how far out of calibration the motor was.");
        DASMSG_SET(i2, timeUncalibrated_ms, "Amount of time motor was uncalibrated according to syscon (ms). If syscon didn't know then 0.")
        DASMSG_SEND();
      }
    }
  
    void CalibrationUpdate()
    {
      if (!isCalibrated_) {

        switch(calState_) {

          case HCS_IDLE:
            break;
  
          case HCS_LOWER_HEAD:
            power_ = HAL::MotorGetCalibPower(MotorID::MOTOR_HEAD);
            HAL::MotorSetPower(MotorID::MOTOR_HEAD, power_);
            lastHeadMovedTime_ms = HAL::GetTimeStamp();
            lowHeadAngleDuringCalib_rad_ = currentAngle_.ToFloat();
            calState_ = HCS_WAIT_FOR_STOP;    
            break;

          case HCS_WAIT_FOR_STOP:
            // Check for when head stops moving for 0.2 seconds
            if (!IsMoving()) {

              if (HAL::GetTimeStamp() - lastHeadMovedTime_ms > HEAD_STOP_TIME) {
#ifdef          CALIB_WHILE_APPLYING_POWER
                AnkiInfo( "HeadController.CalibratedWhileApplyingPower", "");
                calState_ = HCS_COMPLETE;
                break;
#else
                // Turn off motor
                power_ = 0.0;
                HAL::MotorSetPower(MotorID::MOTOR_HEAD, power_);

                // Set timestamp to be used in next state to wait for motor to "relax"
                lastHeadMovedTime_ms = HAL::GetTimeStamp();

                // Go to next state
                calState_ = HCS_SET_CURR_ANGLE;
#endif
              }
            } else {
              lastHeadMovedTime_ms = HAL::GetTimeStamp();
            }
            break;
          case HCS_SET_CURR_ANGLE:
            // Wait for motor to relax and then set angle
            if (HAL::GetTimeStamp() - lastHeadMovedTime_ms > HEAD_STOP_TIME) {
              calState_ = HCS_COMPLETE;
              // Intentional fall-through
            } else {
              break;
            }
          case HCS_COMPLETE:
          {
            OnMotorCalibrated();
            
            // Turn off motor
            power_ = 0.0;
            HAL::MotorSetPower(MotorID::MOTOR_HEAD, power_);

            Messages::SendMotorCalibrationMsg(MotorID::MOTOR_HEAD, false);

            firstCalibration_ = false;
            isCalibrated_     = true;
            calState_         = HCS_IDLE;
            inPosition_       = true;
            encoderInvalidStartTime_ms_   = 0;
            break;
          }
        } // end switch(calState_)

        // Check if head is actually moving up when it should be moving down.
        // This means someone's messing with it so just abort calibration.
        if (IsCalibrating() && calState_ >= HCS_WAIT_FOR_STOP) {
          const float currAngle = currentAngle_.ToFloat();
          if (lowHeadAngleDuringCalib_rad_ > currAngle) {
            lowHeadAngleDuringCalib_rad_ = currAngle;
          }

          if (currAngle - lowHeadAngleDuringCalib_rad_ > UPWARDS_HEAD_MOTION_FOR_CALIB_ABORT_RAD) {
            if (firstCalibration_) {
              AnkiWarn("HeadController.CalibrationUpdate.RestartingCalib",
                        "Someone is probably messing with head (low: %fdeg, curr: %fdeg)",
                        RAD_TO_DEG(lowHeadAngleDuringCalib_rad_), RAD_TO_DEG(currAngle));
              calState_ = HCS_LOWER_HEAD;
            } else {
              AnkiInfo("HeadController.CalibrationUpdate.Abort",
                        "Someone is probably messing with head (low: %fdeg, curr: %fdeg)",
                        RAD_TO_DEG(lowHeadAngleDuringCalib_rad_), RAD_TO_DEG(currAngle));

              // Pretend calibration is fine
              calState_ = HCS_COMPLETE;
            }
          }
        }

      }

    }


    f32 GetAngleRad()
    {
      return currentAngle_.ToFloat();
    }

    void SetAngleRad(f32 angle)
    {
      currentAngle_ = angle;
    }

    f32 GetLastCommandedAngle()
    {
      return desiredAngle_.ToFloat();
    }

    void GetCamPose(f32 &x, f32 &z, f32 &angle)
    {
      f32 cosH = cosf(currentAngle_.ToFloat());
      f32 sinH = sinf(currentAngle_.ToFloat());

      x = HEAD_CAM_POSITION[0]*cosH - HEAD_CAM_POSITION[2]*sinH + NECK_JOINT_POSITION[0];
      z = HEAD_CAM_POSITION[2]*cosH + HEAD_CAM_POSITION[0]*sinH + NECK_JOINT_POSITION[2];
      angle = currentAngle_.ToFloat();
    }

    void PoseAndSpeedFilterUpdate()
    {
      // Get encoder speed measurements
      f32 measuredSpeed = Vector::HAL::MotorGetSpeed(MotorID::MOTOR_HEAD);

      radSpeed_ = (measuredSpeed *
                   (1.0f - SPEED_FILTERING_COEFF) +
                   (radSpeed_ * SPEED_FILTERING_COEFF));

      // Update position
      currentAngle_ += (HAL::MotorGetPosition(MotorID::MOTOR_HEAD) - prevHalPos_);

#if(DEBUG_HEAD_CONTROLLER)
      AnkiDebug( "HeadController", "HEAD FILT: speed %f, speedFilt %f, currentAngle %f, currHalPos %f, prevPos %f, pwr %f",
            measuredSpeed, radSpeed_, currentAngle_.ToFloat(), HAL::MotorGetPosition(MOTOR_HEAD), prevHalPos_, power_);
#endif
      prevHalPos_ = HAL::MotorGetPosition(MotorID::MOTOR_HEAD);
    }

    f32 GetAngularVelocity()
    {
      return radSpeed_;
    }

    void SetAngularVelocity(const f32 speed_rad_per_sec, const f32 accel_rad_per_sec2)
    {
      // Command a target angle based on the sign of the desired speed
      f32 targetAngle = 0;
      bool useVPG = true;
      if (speed_rad_per_sec > 0) {
        targetAngle = MAX_HEAD_ANGLE;
      } else if (speed_rad_per_sec < 0) {
        targetAngle = MIN_HEAD_ANGLE;
      } else {
        // Stop immediately!
        targetAngle = currentAngle_.ToFloat();
        useVPG = false;
      }
      SetDesiredAngle(targetAngle, speed_rad_per_sec, accel_rad_per_sec2, useVPG);
    }

    void SetMaxSpeedAndAccel(const f32 max_speed_rad_per_sec, const f32 accel_rad_per_sec2)
    {
      maxSpeedRad_ = ABS(max_speed_rad_per_sec);
      accelRad_ = ABS(accel_rad_per_sec2);

      if (NEAR_ZERO(maxSpeedRad_)) {
        maxSpeedRad_ = MAX_HEAD_SPEED_RAD_PER_S;
      }
      if (NEAR_ZERO(accelRad_)) {
        accelRad_ = MAX_HEAD_ACCEL_RAD_PER_S2;
      }

      maxSpeedRad_ = CLIP(maxSpeedRad_, 0, MAX_HEAD_SPEED_RAD_PER_S);
      accelRad_    = CLIP(accelRad_, 0, MAX_HEAD_ACCEL_RAD_PER_S2);

    }

    // TODO: There is common code with the other SetDesiredAngle() that can be pulled out into a shared function.
    void SetDesiredAngle_internal(f32 angle, f32 acc_start_frac, f32 acc_end_frac, f32 duration_seconds,
                                  f32 speed_rad_per_sec, f32 accel_rad_per_sec2, bool useVPG)
    {
      if (!enable_ || bracing_) {
        return;
      }

      SetMaxSpeedAndAccel(speed_rad_per_sec, accel_rad_per_sec2);

      // Do range check on angle
      angle = CLIP(angle, MIN_HEAD_ANGLE, MAX_HEAD_ANGLE);

      // Check if already at desired angle
      if (inPosition_ &&
          (angle == desiredAngle_) &&
          (fabsf((desiredAngle_ - currentAngle_).ToFloat()) < HEAD_ANGLE_TOL) ) {
        #if(DEBUG_HEAD_CONTROLLER)
        AnkiDebug( "HeadController", "Already at desired angle %f degrees", RAD_TO_DEG_F32(angle));
        #endif
        HAL::MotorSetPower(MotorID::MOTOR_HEAD,0);
        return;
      }


      desiredAngle_ = angle;
      angleError_ = desiredAngle_.ToFloat() - currentAngle_.ToFloat();


#if(DEBUG_HEAD_CONTROLLER)
      AnkiDebug( "HeadController", "(fixedDuration): SetDesiredAngle %f rads (duration %f)", desiredAngle_.ToFloat(), duration_seconds);
#endif

      f32 startRadSpeed = radSpeed_;
      f32 startRad = currentAngle_.ToFloat();
      if (!inPosition_) {
        vpg_.Step(startRadSpeed, startRad);
      } else {

        if (FLT_NEAR(angleError_,0.f)) {
          #if(DEBUG_HEAD_CONTROLLER)
          AnkiDebug( "HeadController", "(fixedDuration): Already at desired position");
          #endif
          HAL::MotorSetPower(MotorID::MOTOR_HEAD,0);
          return;
        }

        startRadSpeed = 0;
        prevAngleError_ = 0;
        angleErrorSum_ = 0.f;
      }

      lastInPositionTime_ms_ = 0;
      inPosition_ = false;

      bool res = false;
      // Start profile of head trajectory
      if (duration_seconds > 0) {
        res = vpg_.StartProfile_fixedDuration(startRad, startRadSpeed, acc_start_frac*duration_seconds,
                                              desiredAngle_.ToFloat(), acc_end_frac*duration_seconds,
                                              MAX_HEAD_SPEED_RAD_PER_S,
                                              MAX_HEAD_ACCEL_RAD_PER_S2,
                                              duration_seconds,
                                              CONTROL_DT);

        if (!res) {
          AnkiInfo("HeadController.SetDesiredAngle.VPGFixedDurationFailed", "startVel %f, startPos %f, acc_start_frac %f, acc_end_frac %f, endPos %f, duration %f. Trying VPG without fixed duration.",
                   startRadSpeed,
                   startRad,
                   acc_start_frac,
                   acc_end_frac,
                   desiredAngle_.ToFloat(),
                   duration_seconds);
        }

      }
      if (!res) {
        f32 vpgSpeed = maxSpeedRad_;
        f32 vpgAccel = accelRad_;
        if (!useVPG) {
          // If not useVPG, just use really large velocity and accelerations
          vpgSpeed = 1000000.f;
          vpgAccel = 1000000.f;
        }

        vpg_.StartProfile(startRadSpeed, startRad,
                          vpgSpeed, vpgAccel,
                          0, desiredAngle_.ToFloat(),
                          CONTROL_DT);
      }

#if(DEBUG_HEAD_CONTROLLER)
      AnkiDebug( "HeadController", "VPG (fixedDuration): startVel %f, startPos %f, acc_start_frac %f, acc_end_frac %f, endPos %f, duration %f",
            startRadSpeed, startRad, acc_start_frac, acc_end_frac, desiredAngle_.ToFloat(), duration_seconds);
#endif

    } // SetDesiredAngle_internal()

    void SetDesiredAngleByDuration(f32 angle_rad, f32 acc_start_frac, f32 acc_end_frac, f32 duration_seconds)
    {
      SetDesiredAngle_internal(angle_rad, acc_start_frac, acc_end_frac, duration_seconds,
                               MAX_HEAD_SPEED_RAD_PER_S, MAX_HEAD_ACCEL_RAD_PER_S2, true);
    }

    void SetDesiredAngle(f32 angle_rad,
                         f32 speed_rad_per_sec,
                         f32 accel_rad_per_sec2,
                         bool useVPG) {
      SetDesiredAngle_internal(angle_rad, DEFAULT_START_ACCEL_FRAC, DEFAULT_END_ACCEL_FRAC, 0,
                               speed_rad_per_sec, accel_rad_per_sec2, useVPG);
    }

    bool IsInPosition(void) {
      return inPosition_;
    }


    // Check for conditions that could lead to motor burnout.
    // If motor is powered at greater than BURNOUT_POWER_THRESH for more than BURNOUT_TIME_THRESH_MS, stop it!
    // Assuming that motor is mis-calibrated and it's hitting the low or high hard limit. Do calibration.
    // Returns true if a protection action was triggered.
    bool MotorBurnoutProtection() {

      if (ABS(power_) < BURNOUT_POWER_THRESH) {
        potentialBurnoutStartTime_ms_ = 0;
        return false;
      }

      if (potentialBurnoutStartTime_ms_ == 0) {
        potentialBurnoutStartTime_ms_ = HAL::GetTimeStamp();
      } else if (HAL::GetTimeStamp() - potentialBurnoutStartTime_ms_ > BURNOUT_TIME_THRESH_MS) {
        if (IsInPosition() || IMUFilter::IsBeingHeld() || ProxSensors::IsAnyCliffDetected()) {
          // Stop messing with the head! Going limp until you do!
          Messages::SendMotorAutoEnabledMsg(MotorID::MOTOR_HEAD, false);
          Disable(true);
        } else {
          // Burnout protection triggered. Recalibrating.
          AnkiWarn( "HeadController.MotorBurnoutProtection", "Recalibrating (power = %f)", power_);
          const bool autoStarted = true;
          StartCalibrationRoutine(autoStarted, MotorCalibrationReason::HeadMotorBurnoutProtection);
        }
        return true;
      }
      return false;
    }

    void Brace() {
      AnkiInfo("HeadController.Brace", "");
      HAL::MotorSetPower(MotorID::MOTOR_HEAD, BRACING_POWER);
      bracing_ = true;
    }

    void Unbrace() {
      AnkiInfo("HeadController.Unbrace", "");
      HAL::MotorSetPower(MotorID::MOTOR_HEAD, 0.f);
      bracing_ = false;
    }

    bool IsBracing() {
      return bracing_;
    }

    Result Update()
    {
      CalibrationUpdate();

      PoseAndSpeedFilterUpdate();

      // Check encoder validity
      if (HAL::IsHeadEncoderInvalid() && encoderInvalidStartTime_ms_ == 0) {
        encoderInvalidStartTime_ms_ = HAL::GetTimeStamp();
      }

      // If disabled, do not activate motors
      if(!enable_) {
        if (enableAtTime_ms_ == 0) {
          return RESULT_OK;
        }

        // Auto-enable check
        if (IsMoving()) {
          enableAtTime_ms_ = HAL::GetTimeStamp() + REENABLE_TIMEOUT_MS;
          return RESULT_OK;
        } else if (HAL::GetTimeStamp() >= enableAtTime_ms_) {
          Messages::SendMotorAutoEnabledMsg(MotorID::MOTOR_HEAD, true);
          Enable();
        } else {
          return RESULT_OK;
        }
      }


      if (!IsCalibrated() || bracing_ || MotorBurnoutProtection()) {
        return RESULT_OK;
      }

      // Note that a new call to SetDesiredAngle will get
      // Update() working again after it has reached a previous
      // setting.
      if(not inPosition_) {

        // Get the current desired head angle
        f32 currDesiredRadVel;
        vpg_.Step(currDesiredRadVel, currDesiredAngle_);

        // Compute current angle error
        angleError_ = currDesiredAngle_ - currentAngle_.ToFloat();

        // Compute power value
        power_ = (Kp_ * angleError_) + (Kd_ * (angleError_ - prevAngleError_) * CONTROL_DT) + (Ki_ * angleErrorSum_);

        // Update angle error sum
        prevAngleError_ = angleError_;
        angleErrorSum_ += angleError_;
        angleErrorSum_ = CLIP(angleErrorSum_, -MAX_ERROR_SUM, MAX_ERROR_SUM);


        // If accurately tracking current desired angle...
        if(((ABS(angleError_) < HEAD_ANGLE_TOL) && (desiredAngle_ == currDesiredAngle_))) {

          if (lastInPositionTime_ms_ == 0) {
            lastInPositionTime_ms_ = HAL::GetTimeStamp();
          } else if (HAL::GetTimeStamp() - lastInPositionTime_ms_ > IN_POSITION_TIME_MS) {

            power_ = 0;
            inPosition_ = true;

#         if(DEBUG_HEAD_CONTROLLER)
            AnkiDebug( "HeadController", " HEAD ANGLE REACHED (%f rad)\n", GetAngleRad() );
#         endif
          }
        } else {
          lastInPositionTime_ms_ = 0;
        }




#       if(DEBUG_HEAD_CONTROLLER)
        PERIODIC_PRINT(100, "HEAD: currA %f, curDesA %f, desA %f, err %f, errSum %f, pwr %f, spd %f\n",
                       currentAngle_.ToFloat(),
                       currDesiredAngle_,
                       desiredAngle_.ToFloat(),
                       angleError_,
                       angleErrorSum_,
                       power_,
                       radSpeed_);
        PERIODIC_PRINT(100, "  POWER terms: %f  %f\n", (Kp_ * angleError_), (Ki_ * angleErrorSum_))
#       endif

        power_ = CLIP(power_, -1.0, 1.0);


        HAL::MotorSetPower(MotorID::MOTOR_HEAD, power_);
      } // if not in position

      return RESULT_OK;
    }

    void SetGains(const f32 kp, const f32 ki, const f32 kd, const f32 maxIntegralError)
    {
      Kp_ = kp;
      Ki_ = ki;
      Kd_ = kd;
      MAX_ERROR_SUM = maxIntegralError;
      AnkiInfo( "HeadController.SetGains", "New head gains: kp = %f, ki = %f, kd = %f, maxSum = %f",
            Kp_, Ki_, Kd_, MAX_ERROR_SUM);
    }


    bool IsEncoderInvalid()
    {
      return encoderInvalidStartTime_ms_ > 0;
    }

  } // namespace HeadController
  } // namespace Vector
} // namespace Anki
