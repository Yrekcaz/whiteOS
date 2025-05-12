/**
 * File: imuFilter.cpp
 *
 * Author: Kevin Yoon
 * Created: 4/1/2014
 *
 * Description:
 *
 *   Filter for gyro and accelerometer
 *   Orientation of gyro axes is assumed to be identical to that of robot when the head is at 0 degrees.
 *   i.e. x-axis points forward, y-axis points to robot's left, z-axis points up.
 *
 * Copyright: Anki, Inc. 2014
 *
 **/

#include "trig_fast.h"
#include "imuFilter.h"
#include <math.h>
#include "headController.h"
#include "liftController.h"
#include "wheelController.h"
#include "pathFollower.h"
#include "pickAndPlaceController.h"
#include "proxSensors.h"
#include "messages.h"

#include "anki/cozmo/robot/logging.h"
#include "anki/cozmo/robot/hal.h"
#include "anki/cozmo/robot/DAS.h"

#include "clad/robotInterface/messageRobotToEngine_send_helper.h"
#include "clad/types/motorTypes.h"

#include "util/container/minMaxQueue.h"

#include <array>
#include <cmath>

#define DEBUG_IMU_FILTER 0

// Define type of data to send when IMURequest received
#define RECORD_AND_SEND_RAW_DATA  0
#define RECORD_AND_SEND_FILT_DATA 1
#define RECORD_AND_SEND_MODE RECORD_AND_SEND_RAW_DATA

// Whether or not to tuck head and lift down when falling is detected
#define DEFAULT_BRACE_WHEN_FALLING true

namespace Anki {
  namespace Vector {
    namespace IMUFilter {

      namespace {
        // Last read IMU data
        HAL::IMU_DataStructure imu_data_;

        // Orientation and speed in XY-plane (i.e. horizontal plane) of robot
        Radians rot_ = 0;   // radians
        f32 rotSpeed_ = 0; // rad/s

        // Roll angle:
        f32 roll_                        = 0.f;
        const f32 ROLL_FILT_COEFF        = 0.75f; // Filter to combine gyro and accel for smooth roll estimation
                                                  // The higher this value, the slower it approaches accel-based roll,
                                                  // but the less noisy it is.
        const f32 ROLL_FILT_COEFF_MOVING = 0.9f;  // Same as above, but used when the robot's wheels are moving

        // Pitch angle: Approaches angle of accelerometer wrt gravity horizontal
        f32 pitch_                      = 0;
        const f32 PITCH_FILT_COEFF      = 0.98f;  // Filter to combine gyro and accel for smooth pitch estimation
                                                  // The higher this value, the slower it approaches accel-based pitch,
                                                  // but the less noisy it is.
        const f32 PITCH_FILT_COEFF_MOVING = 0.998f;  // Same as above, but used when the robot's wheels are moving
        const f32 UNINIT_HEAD_ANGLE     = 10000;  // Just has to be some not physically possible value
        f32 prevHeadAngle_              = UNINIT_HEAD_ANGLE;

        f32 gyro_[3]                    = {0};    // Bias-compensated gyro measurements
        f32 gyro_robot_frame[3]         = {0};    // Unfiltered gyro measurements in robot frame
        f32 gyro_robot_frame_filt[3]    = {0};    // Filtered gyro measurements in robot frame
        const f32 RATE_FILT_COEFF       = 1.f;    // IIR low-pass filter coefficient (1 == disable filter)

        f32 gyro_bias_filt[3]           = {0};     // Filtered gyro bias
        const f32 GYRO_BIAS_FILT_COEFF_NORMAL  = 0.0005f; // IIR low-pass filter coefficient (1 == disable filter).
                                                          // Relatively slow once we're sure calibration is reasonably good since it shouldn't be changing that fast.

        const f32 GYRO_BIAS_FILT_COEFF_TEMP_CHANGING = 0.0025f; // Gyro bias filter coefficient used while the IMU temperature is changing (due to initial warming up)

        const f32 GYRO_BIAS_FILT_COEFF_PRECALIB = 0.017f;   // Gyro bias filter coefficient. Relatively fast before calibration.
        f32 gyroBiasCoeff_              = GYRO_BIAS_FILT_COEFF_PRECALIB;

        // Are we finished initial calibration for the gyro zero-rate bias?
        bool biasFilterComplete_        = false;

        // During initial gyro bias calibration, store recent bias estimates in a queue. This allows us to compute a
        // min and max of recent bias estimates, to ensure that all readings are within a small band before declaring
        // the gyro bias 'calibrated'.
        std::array<Util::MinMaxQueue<f32>, 3> gyroBiasCalibValues_;

        // Max difference between the max and min of recent gyro bias estimates. If any recent readings that fall
        // outside this band, then the gyro bias is not yet calibrated.
        const f32 BIAS_FILT_ALLOWED_BAND = DEG_TO_RAD_F32(0.35f);

        // Number of consecutive gyro bias estimates required to fall within a small min/max window before declaring the gyro 'calibrated'
        const u16 BIAS_FILT_COMPLETE_COUNT = 200;

        bool gyro_sign[3] = {false}; // true is negative, false is positive

        // Circular buffer of robot-frame gyro readings corresponding to every IMU reading that we have received. These
        // get sent to engine as part of RobotState and are used for image de-warping, etc.
        ImuDataBufferType imuDataBuffer_;

        f32 accel_filt[3]               = {0};    // Filtered accelerometer measurements
        f32 accel_robot_frame[3]        = {0};    // Unfiltered accelerometer measurements in robot frame
        f32 accel_robot_frame_filt[3]   = {0};    // Filtered accelerometer measurements in robot frame
        f32 abs_accel_robot_frame_filt[3] = {0};  // Absolute value of accelerations
        const f32 ACCEL_FILT_COEFF      = 0.1f;   // IIR low-pass filter coefficient (1 == disable filter)

        f32 accelMagnitudeSqrd_         = 9810 * 9810;

        const f32 HP_ACCEL_FILT_COEFF   = 0.5f;     // IIR high-pass filter coefficient (0 == no-pass)
        f32 accel_robot_frame_high_pass[3] = {0};

        // These values used to check if IMU temperature is changing (see usage in IMUFilter::Update())
        u32 timeOfLastImuTempSample_ms_ = 0;
        f32 lastImuTempSample_degC_ = 0.f;

        // ==== Pickup detection ===
        bool pickupDetectEnabled_       = true;
        bool pickedUp_                  = false;
        bool isMotionDetected_          = false;

        const f32 PICKUP_WHILE_MOVING_ACC_THRESH[3]  = {5000, 5000, 12000};  // mm/s^2
        const f32 PICKUP_WHILE_WHEELS_NOT_MOVING_GYRO_THRESH = DEG_TO_RAD_F32(10.f);  // rad/s
        const f32 UNEXPECTED_ROTATION_SPEED_THRESH   = DEG_TO_RAD_F32(20.f); //rad/s
        const u8 PICKUP_COUNT_WHILE_MOVING           = 40;
        const u8 PICKUP_COUNT_WHILE_MOTIONLESS       = 20;
        u8 potentialPickupCnt_                       = 0;

        const u8 NUM_CLIFF_SENSORS_FOR_CHANGE_DETECT_PICKUP = 3;

        const f32 ACCEL_DISTURBANCE_MOTION_THRESH = 40.f;
        s8 external_accel_disturbance_cnt[3]      = {0};
        // === End of Pickup detection ===


        // Motion detection
        u32 lastMotionDetectedTime_ms = 0;
        const u32 MOTION_DETECT_TIMEOUT_MS = 200;
        const f32 ACCEL_MOTION_THRESH = 10;  // mm/s^2
        const f32 GYRO_MOTION_THRESHOLD = DEG_TO_RAD_F32(0.5f);           // Gyro motion threshold post-calibration
        const f32 GYRO_MOTION_PRECALIB_THRESHOLD = DEG_TO_RAD_F32(10.f);  // Gyro motion threshold pre-calibration
                                                                          // (Max bias according to BMI160 datasheet is +/- 10 deg/s)

        // Poke detection
        TimeStamp_t _lastPokeDetectTime = 0;
        TimeStamp_t _peakGyroStartTime = 0;
        TimeStamp_t _peakGyroMaxTime = 0;
        const u32 POKE_DETECT_COOLDOWN_MS = 1000;
        const f32 PEAK_GYRO_THRESHOLD = DEG_TO_RAD_F32(5.f);  // rad/s
        const u32 MAX_GYRO_PEAK_DURATION_MS = 75;

        // Recorded buffer
        bool isRecording_ = false;

#if(RECORD_AND_SEND_MODE == RECORD_AND_SEND_FILT_DATA)
        u8 recordDataIdx_ = 0;
        RobotInterface::IMUDataChunk imuChunkMsg_;
#else
        RobotInterface::IMURawDataChunk imuRawDataMsg_;
        u32 totalIMUDataMsgsToSend_ = 0;
        u32 sentIMUDataMsgs_ = 0;
#endif

        // ==== Falling detection ===
        bool falling_ = false;
        bool bracingEnabled_ = true;
        bool fallStarted_ = false;               // Indicates that falling is detected by accelerometer, but not necessarily for long enough to trigger falling_ flag
        TimeStamp_t fallStartedTime_ = 0;        // timestamp of when freefall started
        TimeStamp_t freefallDuration_ = 0;       // approximate duration of freefall
        TimeStamp_t braceStartedTime_ = 0;

        // Impact detection

        // note:
        //
        // In brief, there are three events used to determine if an impact was experienced:
        //  BeingHeld, Freefall, and AccelerationSpike.
        // The last one is when the measured acceleration norm from the imu shows high force
        // which we know occurs when the robot hits the ground, or when is snapped or jerked
        // during handling, or forcibly put onto a table.
        // We care about not firing fall-impact events for the latter two cases, as a result
        // we use a combination of heuristics on BeingHeld and Freefall detection to have
        // confidence we experience a true-fall.
        // 
        // Empirical testing shows that both BeingHeld and Freefall are prone to false results
        // as well as inconsistent timing of detection. For example:
        // - the robot may detect teetering right before rolling off of the edge of a table
        //   as being held
        // - during very fast tumbling mid-air, the robot may misclassify the Freefall state
        // - the robot may register as being held either instantly, or up to a second later
        //
        // To combat this, we buffer the AccelerationSpike event with a rolling-window, which
        // means when an impact is detected there is an active period during which we may
        // catch other pertinent events like BeingHeld and Freefall to determine if it was a
        // true-fall.
        //
        // Examples of how this is applied are:
        //
        // - suppressing impact-detection if being-held was recently true prevents handling
        //   and moving the robot around as registering as a true-positive fall.
        //
        // - suppressing impact-detection if being-held is true for a minimum duration means
        //   we can be more confident in the being-held state. The flipside is that it takes
        //   longer to be certain, which necessitates buffering the acceleration spike event
        //   in order to correlate it correctly.
        // 
        // - free-fall detection has some cases (i.e. tumbling) that causes the robot to
        //   misdetect this case. To combat this, we use a timeout that is conditioned on
        //   freefall state variables to catch this case and thus check for impact more
        //   aggressively (thus reducing false-negatives where we ignore impacts because
        //   there was no confirmed fall)

        // compute the max acceleration in a sliding-window for detecting the impact spike
        Util::MinMaxQueue<f32> maxAccelerationMagnitudeBuffer_;

        // previous update tick's state of being-held
        bool lastRobotBeingHeldState_ = false;

        // the instant the robot started being-held
        TimeStamp_t timeOfStartBeingHeld_ = 0;

        // the instant the robot stopped being held
        TimeStamp_t timeOfEndBeingHeld_ = 0;

        // max size of the acceleration-magnitutde buffer
        const size_t MAX_ACCEL_MAGNITUDE_BUFFER_SIZE = 20;

        // while the robot is being held, the minimum duration of a hold that suppresses impact-detection from activating
        const TimeStamp_t MIN_TIME_HELD_SUPPRESS_IMPACT_MS = 500;

        // after the robot is being putdown, suppress impacts for a duration of time (helps ignore being put down forcefully)
        const TimeStamp_t MIN_TIME_SINCE_PUTDOWN_SUPPRESS_IMPACT_MS = 500;
        
        // we expect to see a spike in acceleration in excess of 1.0g's at the time of impact
        const float MIN_ACCEL_MAG_SQUARED_FOR_IMPACT_MMPS2 = pow(2.2e4,2);

        // last time when the falling-detector registered a potential fall
        TimeStamp_t lastPotentialFallTime_ = 0; 

        // minimum time period after a fall was detected to check for impact
        const TimeStamp_t IMPACT_DETECTION_WINDOW_AFTER_FALL_MS = 600;

        // throttle the number of times per second we can send the 
        // ImpactMsg since it may trigger multiple times after the 
        // first possible detection
        const TimeStamp_t IMPACT_MESSAGE_THROTTLE_TIME_MS = 5000;

        TimeStamp_t lastImpactSendTime_ms_ = 0;

        // === End of Falling detection ===

        // N-side down
        const f32 NSIDE_DOWN_THRESH_MMPS2 = 8000;

      } // "private" namespace





//      // Implementation of Madgwick's IMU and AHRS algorithms.
//      // See: http://x-io.co.uk/open-source-imu-and-ahrs-algorithms/
//
//      //---------------------------------------------------------------------------------------------------
//      // Definitions
//      const float beta	= 0.1f;		// 2 * proportional gain
//
//      //---------------------------------------------------------------------------------------------------
//      // Variable definitions
//      volatile float q0 = 1.0f, q1 = 0.0f, q2 = 0.0f, q3 = 0.0f;	// quaternion of sensor frame relative to auxiliary frame
//
//      // Euler Z-angle
//      float _zAngle = 0;
//
//      //---------------------------------------------------------------------------------------------------
//      // Function declarations
//
//      float invSqrt(float x);
//
//      //---------------------------------------------------------------------------------------------------
//      // IMU algorithm update
//
//      void MadgwickAHRSupdateIMU(float gx, float gy, float gz, float ax, float ay, float az) {
//        float recipNorm;
//        float s0, s1, s2, s3;
//        float qDot1, qDot2, qDot3, qDot4;
//        float _2q0, _2q1, _2q2, _2q3, _4q0, _4q1, _4q2 ,_8q1, _8q2, q0q0, q1q1, q2q2, q3q3;
//
//        // Rate of change of quaternion from gyroscope
//        qDot1 = 0.5f * (-q1 * gx - q2 * gy - q3 * gz);
//        qDot2 = 0.5f * (q0 * gx + q2 * gz - q3 * gy);
//        qDot3 = 0.5f * (q0 * gy - q1 * gz + q3 * gx);
//        qDot4 = 0.5f * (q0 * gz + q1 * gy - q2 * gx);
//
//        // Compute feedback only if accelerometer measurement valid (avoids NaN in accelerometer normalisation)
//        if(!((ax == 0.0f) && (ay == 0.0f) && (az == 0.0f))) {
//
//          // Normalise accelerometer measurement
//          recipNorm = invSqrt(ax * ax + ay * ay + az * az);
//          ax *= recipNorm;
//          ay *= recipNorm;
//          az *= recipNorm;
//
//          // Auxiliary variables to avoid repeated arithmetic
//          _2q0 = 2.0f * q0;
//          _2q1 = 2.0f * q1;
//          _2q2 = 2.0f * q2;
//          _2q3 = 2.0f * q3;
//          _4q0 = 4.0f * q0;
//          _4q1 = 4.0f * q1;
//          _4q2 = 4.0f * q2;
//          _8q1 = 8.0f * q1;
//          _8q2 = 8.0f * q2;
//          q0q0 = q0 * q0;
//          q1q1 = q1 * q1;
//          q2q2 = q2 * q2;
//          q3q3 = q3 * q3;
//
//          // Gradient decent algorithm corrective step
//          s0 = _4q0 * q2q2 + _2q2 * ax + _4q0 * q1q1 - _2q1 * ay;
//          s1 = _4q1 * q3q3 - _2q3 * ax + 4.0f * q0q0 * q1 - _2q0 * ay - _4q1 + _8q1 * q1q1 + _8q1 * q2q2 + _4q1 * az;
//          s2 = 4.0f * q0q0 * q2 + _2q0 * ax + _4q2 * q3q3 - _2q3 * ay - _4q2 + _8q2 * q1q1 + _8q2 * q2q2 + _4q2 * az;
//          s3 = 4.0f * q1q1 * q3 - _2q1 * ax + 4.0f * q2q2 * q3 - _2q2 * ay;
//          recipNorm = invSqrt(s0 * s0 + s1 * s1 + s2 * s2 + s3 * s3); // normalise step magnitude
//          s0 *= recipNorm;
//          s1 *= recipNorm;
//          s2 *= recipNorm;
//          s3 *= recipNorm;
//
//          // Apply feedback step
//          qDot1 -= beta * s0;
//          qDot2 -= beta * s1;
//          qDot3 -= beta * s2;
//          qDot4 -= beta * s3;
//        }
//
//        // Integrate rate of change of quaternion to yield quaternion
//        q0 += qDot1 * (1.0f * CONTROL_DT);   // q0 += qDot1 * (1.0f / sampleFreq);
//        q1 += qDot2 * (1.0f * CONTROL_DT);
//        q2 += qDot3 * (1.0f * CONTROL_DT);
//        q3 += qDot4 * (1.0f * CONTROL_DT);
//
//        // Normalise quaternion
//        recipNorm = invSqrt(q0 * q0 + q1 * q1 + q2 * q2 + q3 * q3);
//        q0 *= recipNorm;
//        q1 *= recipNorm;
//        q2 *= recipNorm;
//        q3 *= recipNorm;
//
//
//        // Compute zAngle from quaternion
//        _zAngle = atan2_fast(2*(q0*q3 + q1*q2), 1 - 2*(q2*q2 + q3*q3) );
//      }
//
//      //---------------------------------------------------------------------------------------------------
//      // Fast inverse square-root
//      // See: http://en.wikipedia.org/wiki/Fast_inverse_square_root
//
//      float invSqrt(float x) {
//        float halfx = 0.5f * x;
//        float y = x;
//        long i = *(long*)&y;
//        i = 0x5f3759df - (i>>1);
//        y = *(float*)&i;
//        y = y * (1.5f - (halfx * y * y));
//        return y;
//      }
//      /// --------------------------------------------------------------------------------------------------



      void BraceForImpact()
      {
        if (bracingEnabled_) {
          LiftController::Brace();
          HeadController::Brace();
        }
      }

      void UnbraceAfterImpact()
      {
        if (bracingEnabled_) {
          LiftController::Unbrace();
          HeadController::Unbrace();
        }
      }

      void ResetFallingVars() {
        falling_ = false;
        fallStarted_ = false;
        fallStartedTime_ = 0;
        freefallDuration_ = 0;
        braceStartedTime_ = 0;
      }

      void ResetPickupVars() {
        potentialPickupCnt_ = 0;
        external_accel_disturbance_cnt[0] = external_accel_disturbance_cnt[1] = external_accel_disturbance_cnt[2] = 0;
      }

      void SetPickupDetect(bool pickupDetected)
      {
        if (pickedUp_ != pickupDetected) {
          ResetPickupVars();
          pickedUp_ = pickupDetected;
        }
      }

      void EnablePickupDetect(bool enable)
      {
        if (!enable) {
          SetPickupDetect(false);
        }
        pickupDetectEnabled_ = enable;
      }

      void EnableBraceWhenFalling(bool enable)
      {
        AnkiInfo( "IMUFilter.EnableBraceWhenFalling", "%d", enable);
        bracingEnabled_ = enable;
      }

      Result Init()
      {
        EnableBraceWhenFalling(DEFAULT_BRACE_WHEN_FALLING);
        return RESULT_OK;
      }

      // Applies low-pass filtering to 3-element input, storing result to 3-element output assuming
      // output is passed in with previous timestep's filter values.
      void LowPassFilter(f32* output, const f32* input, const f32 coeff)
      {
        output[0] = input[0] * coeff + output[0] * (1.f - coeff);
        output[1] = input[1] * coeff + output[1] * (1.f - coeff);
        output[2] = input[2] * coeff + output[2] * (1.f - coeff);
      }

      // Returns low pass filtered output given single input and previous timestep's output
      f32 LowPassFilter_single(const f32 prev_output, const f32 input, const f32 coeff)
      {
        return input * coeff + prev_output * (1.f - coeff);
      }

      // Applies high-pass filtering to 3-element input and prev_input,
      // storing result to 3-element output assuming output is passed in with previous timestep's filter values.
      void HighPassFilter(f32* output, const f32* input, const f32* prev_input, const f32 coeff)
      {
        output[0] = coeff * (output[0] + input[0] - prev_input[0]);
        output[1] = coeff * (output[1] + input[1] - prev_input[1]);
        output[2] = coeff * (output[2] + input[2] - prev_input[2]);
      }

      void ClearGyroBiasCalibValues()
      {
        for (auto& biasVals : gyroBiasCalibValues_) {
          biasVals.clear();
        }
      }

      void UpdateGyroBiasFilter(const float rawGyro[3])
      {
        // Gyro bias filter update/calibration:
        //
        // During initial calibration, we maintain a filtered gyro bias estimate for each of the three axes. Once the
        // filtered bias estimates converge on a near-constant value, we declare the gyro bias filter 'complete'. The
        // criterion for convergence is that the filtered bias estimate has remained within a small window
        // (BIAS_FILT_ALLOWED_BAND) for the required number of ticks (BIAS_FILT_COMPLETE_COUNT).
        //
        // Once the initial calibration is complete (i.e. IsBiasFilterComplete() == true), we switch to a much slower
        // filter which keeps track of slow drifting of gyro zero-rate bias over time and temperature changes.

        if (!isMotionDetected_) {
          // Update gyro bias offset while not moving. If the gyro bias value queue is just starting, then set the
          // filtered gyro bias to the actual gyro value to initialize it. Otherwise, pass it through the low pass filter.
          for (int i=0 ; i<3 ; i++) {
            if (!IsBiasFilterComplete() && gyroBiasCalibValues_[i].empty()) {
              gyro_bias_filt[i] = rawGyro[i];
            } else {
              gyro_bias_filt[i] = LowPassFilter_single(gyro_bias_filt[i], rawGyro[i], gyroBiasCoeff_);
            }
          }

          AnkiInfoPeriodic(12000, "IMUFilter.Bias", "%f %f %f (deg/sec)",
                           RAD_TO_DEG_F32(gyro_bias_filt[0]),
                           RAD_TO_DEG_F32(gyro_bias_filt[1]),
                           RAD_TO_DEG_F32(gyro_bias_filt[2]));

          // If initial bias estimate not complete, accumulate
          if (!IsBiasFilterComplete()) {
            for (int i=0 ; i<3 ; i++) {
              auto& biasVals = gyroBiasCalibValues_[i];
              biasVals.push(gyro_bias_filt[i]);
              while (biasVals.size() > BIAS_FILT_COMPLETE_COUNT) {
                biasVals.pop();
              }
            }
            // Only declare the bias filter complete if we have enough filtered bias readings, and they all fall within
            // the required window.
            biasFilterComplete_ = std::all_of(gyroBiasCalibValues_.begin(), gyroBiasCalibValues_.end(),
                                              [](const Util::MinMaxQueue<f32>& biasVals) {
                                                return (biasVals.size() >= BIAS_FILT_COMPLETE_COUNT) &&
                                                       (biasVals.max() - biasVals.min() < BIAS_FILT_ALLOWED_BAND);
                                              });
            if (IsBiasFilterComplete()) {
              // Bias filter has accumulated enough measurements while not moving.
              // Switch to slow filtering.
              const f32 gyro_bias_deg[] = {RAD_TO_DEG_F32(gyro_bias_filt[0]),
                                           RAD_TO_DEG_F32(gyro_bias_filt[1]),
                                           RAD_TO_DEG_F32(gyro_bias_filt[2])};
              AnkiInfo("IMUFilter.GyroCalibrated", "%f %f %f (deg/sec)",
                       gyro_bias_deg[0],
                       gyro_bias_deg[1],
                       gyro_bias_deg[2]);

              DASMSG(imu_filter_gyro_calibrated, "imu_filter.gyro_calibrated", "Gyro calibration values and duration");
              DASMSG_SET(i1, 1000 * gyro_bias_deg[0], "X-axis gyro bias (milli-degrees)");
              DASMSG_SET(i2, 1000 * gyro_bias_deg[1], "Y-axis gyro bias (milli-degrees)");
              DASMSG_SET(i3, 1000 * gyro_bias_deg[2], "Z-axis gyro bias (milli-degrees)");
              DASMSG_SET(i4, HAL::GetTimeStamp() - lastMotionDetectedTime_ms, "Time since last motion detected (ms)");
              DASMSG_SEND();

              gyroBiasCoeff_ = GYRO_BIAS_FILT_COEFF_NORMAL;
              // No longer need to keep the gyro bias calibration values, so discard them to save memory
              ClearGyroBiasCalibValues();
            }
          }
        } else if (!IsBiasFilterComplete()) {
          // Since we're moving, clear out the initial gyro bias value queues so that we restart gyro bias calibration
          ClearGyroBiasCalibValues();
        }
      }

      void DetectFalling()
      {
        // Fall detection accelerometer thresholds:
        const f32 FALLING_THRESH_LOW_MMPS2_SQRD = 6000 * 6000; // If accMag falls below this, fallStarted is triggered...
        const f32 FALLING_THRESH_HIGH_MMPS2_SQRD = 9000 * 9000; // ...and not untriggered until it rises above this.
        const f32 STOPPED_TUMBLING_THRESH = 50.f; // accelerometer value corresponding to "no longer tumbling on the ground after a fall"

        // Fall detection timing:
        const TimeStamp_t now = HAL::GetTimeStamp();
        const TimeStamp_t fallDetectionTimeout_ms = 150; // fallStarted flag must be set for this long in order for 'bracing' to occur.

        // "Bracing manuever" timing
        const TimeStamp_t bracingTime_ms = 250; // this much time (minimum) is allowed for the bracing maneuver to complete.

        if (falling_) {
          if (accelMagnitudeSqrd_ > FALLING_THRESH_HIGH_MMPS2_SQRD) {
            // Estimating time in freefall: Consider the freefall finished
            // once the accelMagnitude rises above the upper threshold
            if (freefallDuration_ == 0) {
              freefallDuration_ = now - fallStartedTime_;
            }

            // Wait for robot to stop moving and bracing to complete, then unbrace.
            // Check for high-freq activity on x-axis (this could easily be any other axis since the threshold is so small)
            // to determine when the robot is definitely no longer moving.
            if((fabsf(accel_robot_frame_high_pass[0]) < STOPPED_TUMBLING_THRESH) &&
               (now - braceStartedTime_ > bracingTime_ms)) {
              // Send the FallingEvent message
              RobotInterface::FallingEvent msg;
              msg.timestamp = fallStartedTime_;
              msg.duration_ms = freefallDuration_;
              RobotInterface::SendMessage(msg);
              DASMSG(imu_filter_falling_event,  "imu_filter.falling_event", "Robot experienced a fall");
              DASMSG_SET(i1, freefallDuration_, "Duration of fall (ms)");
              DASMSG_SEND();

              ResetFallingVars();
              UnbraceAfterImpact();
            }
          }
        }
        else { // 'falling_' flag not set
          if (fallStarted_) {
            // If fallStarted has been set for long enough, set the global falling flag and brace.
            if (now - fallStartedTime_ > fallDetectionTimeout_ms) {
              falling_ = true;
              braceStartedTime_ = now;
              BraceForImpact();
            } else {
              // only clear the flag if aMag rises above the higher threshold.
              fallStarted_ = (accelMagnitudeSqrd_ < FALLING_THRESH_HIGH_MMPS2_SQRD) && ProxSensors::IsAnyCliffDetected();
            }
          } else { // not fallStarted
            if ((accelMagnitudeSqrd_ < FALLING_THRESH_LOW_MMPS2_SQRD) && ProxSensors::IsAnyCliffDetected()) {
              fallStarted_ = true;
              fallStartedTime_ = now;
            }
          }
        }
      }
      
      // impact detection works based on seeing a high enough acceleration norm
      // over a sliding window of time. If the robot is being handled (in-hand
      // manipulation), then impact detection is suppressed.
      // additionally, we check for impact only for a short duration after a fall
      // is potentially detected
      void DetectImpact()
      {
        const TimeStamp_t now = HAL::GetTimeStamp();

        f32 currTickAccelNormSq = (accel_robot_frame[0]*accel_robot_frame[0] +
                                   accel_robot_frame[1]*accel_robot_frame[1] +
                                   accel_robot_frame[2]*accel_robot_frame[2]);
        
        maxAccelerationMagnitudeBuffer_.push(currTickAccelNormSq);
        if(maxAccelerationMagnitudeBuffer_.size() > MAX_ACCEL_MAGNITUDE_BUFFER_SIZE) {
          maxAccelerationMagnitudeBuffer_.pop();
        }
        f32 maxAccMagnitude = maxAccelerationMagnitudeBuffer_.max();

        // compute whether the robot's being-held state should suppress the impact detector
        const bool currRobotBeingHeldState = IMUFilter::IsBeingHeld();
        if(currRobotBeingHeldState && !lastRobotBeingHeldState_) {
          timeOfStartBeingHeld_ = now;
        }
        if(!currRobotBeingHeldState && lastRobotBeingHeldState_) {
          timeOfEndBeingHeld_ = now;
        }
        lastRobotBeingHeldState_ = currRobotBeingHeldState;
        const bool isCurrentlyHeld    = currRobotBeingHeldState && ((now - timeOfStartBeingHeld_) > MIN_TIME_HELD_SUPPRESS_IMPACT_MS);
        const bool wasRecentlyHeld    = !currRobotBeingHeldState && ((now - timeOfEndBeingHeld_) < MIN_TIME_SINCE_PUTDOWN_SUPPRESS_IMPACT_MS);
        const bool beingHeldSuppressImpact = (wasRecentlyHeld || isCurrentlyHeld);

        // determine if we should query for impact detection, because of a possible fall
        if(falling_ || fallStarted_) {
          lastPotentialFallTime_ = now;
        }
        const TimeStamp_t timeSinceFallingDetected = now - lastPotentialFallTime_;
        const TimeStamp_t timeSinceImpactMsg = now - lastImpactSendTime_ms_;
        const bool shouldCheckForImpact = (timeSinceFallingDetected < IMPACT_DETECTION_WINDOW_AFTER_FALL_MS);

        // throttle sending the impact message if we've sent it already
        const bool canSendMessage = (timeSinceImpactMsg > IMPACT_MESSAGE_THROTTLE_TIME_MS);

        if(canSendMessage) {
          const bool didAccelerationSpike = (maxAccMagnitude > MIN_ACCEL_MAG_SQUARED_FOR_IMPACT_MMPS2);
          const bool detectedImpact = shouldCheckForImpact && !beingHeldSuppressImpact && didAccelerationSpike;

          if(detectedImpact) {
            RobotInterface::SendMessage(RobotInterface::FallImpactEvent());

            DASMSG(imu_filter_fall_impact_event,  "imu_filter.fall_impact_event", "Robot experienced a freefall for a minimum duration and then a sudden impact");
            DASMSG_SEND();

            lastImpactSendTime_ms_ = now;
          }
        }
      }

      // Conservative check for unintended acceleration that are
      // valid even while the motors are moving.
      bool CheckPickupWhileMoving() {
        return (abs_accel_robot_frame_filt[0] > PICKUP_WHILE_MOVING_ACC_THRESH[0]) ||
               (abs_accel_robot_frame_filt[1] > PICKUP_WHILE_MOVING_ACC_THRESH[1]) ||
               (abs_accel_robot_frame_filt[2] > PICKUP_WHILE_MOVING_ACC_THRESH[2]);
      }

      bool AreMotorsMoving() {
        return  WheelController::AreWheelsMoving() || HeadController::IsMoving() || LiftController::IsMoving();
      }

      // Checks for conditions to enter/exit picked up state
      void DetectPickup()
      {
        if (!pickupDetectEnabled_)
          return;

        // If enough of the cliff sensors detect cliffs, this is indicative of pickup.
        u8 numCliffSensorsDetectingChange = 0;
        for (int i=0 ; i < HAL::CLIFF_COUNT ; i++) {
          if (ProxSensors::IsCliffDetected(i)) {
            ++numCliffSensorsDetectingChange;
          }
        }
        bool cliffBasedPickupDetect = numCliffSensorsDetectingChange >= NUM_CLIFF_SENSORS_FOR_CHANGE_DETECT_PICKUP;


        if (IsPickedUp()) {
          // Picked up flag is reset only when the robot has
          // stopped moving, detects no cliffs, and has been set upright.
          if (!cliffBasedPickupDetect &&
              !isMotionDetected_ &&
              (accel_robot_frame_filt[2] > NSIDE_DOWN_THRESH_MMPS2)) {
            SetPickupDetect(false);
          }

        } else {

          // Gyro activity can also indicate pickup.
          // Different detection thresholds are used depending on whether or not the wheels are moving.
          bool gyroZBasedMotionDetect = false;
          if (!AreMotorsMoving()) {

            // As long as wheels aren't moving, we can also check for Z-axis gyro motion
            gyroZBasedMotionDetect = ABS(gyro_robot_frame_filt[2]) > PICKUP_WHILE_WHEELS_NOT_MOVING_GYRO_THRESH;

          } else {

            // Is the robot turning at a radically different speed than what it should be experiencing given current wheel speeds?
            // UNEXPECTED_ROTATION_SPEED_THRESH is being used as a multipurpose margin here. Because GetCurrNoSlipBodyRotSpeed() is based
            // on filtered wheel speeds there's a little delay which permits measuredBodyRotSpeed to be a little faster than maxPossibleBodyRotSpeed.
            const f32 maxPossibleBodyRotSpeed = WheelController::GetCurrNoSlipBodyRotSpeed();
            const f32 measuredBodyRotSpeed = IMUFilter::GetRotationSpeed();
            gyroZBasedMotionDetect = (((maxPossibleBodyRotSpeed > UNEXPECTED_ROTATION_SPEED_THRESH) &&
                                       ((measuredBodyRotSpeed < -UNEXPECTED_ROTATION_SPEED_THRESH) || (measuredBodyRotSpeed > maxPossibleBodyRotSpeed + UNEXPECTED_ROTATION_SPEED_THRESH))) ||
                                      ((maxPossibleBodyRotSpeed < -UNEXPECTED_ROTATION_SPEED_THRESH) &&
                                       ((measuredBodyRotSpeed > UNEXPECTED_ROTATION_SPEED_THRESH) || (measuredBodyRotSpeed < maxPossibleBodyRotSpeed - UNEXPECTED_ROTATION_SPEED_THRESH))));

          }


          // Sensitive check
          // If motors aren't moving, any motion is because a person was messing with it!
          if (!AreMotorsMoving()) {

            // Sufficient gyro motion is evidence of pickup
            bool gyroBasedMotionDetected = (ABS(gyro_robot_frame_filt[0]) > PICKUP_WHILE_WHEELS_NOT_MOVING_GYRO_THRESH) ||
                                           (ABS(gyro_robot_frame_filt[1]) > PICKUP_WHILE_WHEELS_NOT_MOVING_GYRO_THRESH) ||
                                           (ABS(gyro_robot_frame_filt[2]) > PICKUP_WHILE_WHEELS_NOT_MOVING_GYRO_THRESH);

            if (cliffBasedPickupDetect || gyroBasedMotionDetected)
            {
              ++potentialPickupCnt_;
            }
            else if(potentialPickupCnt_ > 0)
            {
              // Decrease potentialPickupCnt while no motion is detected
              --potentialPickupCnt_;
            }

            // If the sign of the gyro data changes then reset potentialPickupCnt
            // This is to prevent oscillations from triggering pickup
            for(u8 i = 0; i < 3; ++i)
            {
              if(gyro_sign[i] != signbit(gyro_robot_frame_filt[i]))
              {
                potentialPickupCnt_ = 0;
              }
            }


            // Sufficient acceleration is evidence of pickup.
            // Only evaluating the horizontal axes. Z-acceleration is sensitive to surface vibrations,
            // plus z-motion should be captured more reliably by the cliff sensor.
            // Keeping track of sign of disturbance because if it crosses 0 this is more indicative of
            // vibration versus steady motion.
            for (u8 i=0; i < 2; ++i) {
              if (ABS(accel_robot_frame_high_pass[i]) > ACCEL_DISTURBANCE_MOTION_THRESH) {
                s8 incr = accel_robot_frame_high_pass[i] > 0 ? 1 : -1;
                if (accel_robot_frame_high_pass[i] > 0 == external_accel_disturbance_cnt[i] >= 0) {
                  external_accel_disturbance_cnt[i] += incr;
                } else {
                  external_accel_disturbance_cnt[i] = incr;
                }
              } else {
                external_accel_disturbance_cnt[i] = 0;
              }
            }

            bool accelBasedMotionDetected = (ABS(external_accel_disturbance_cnt[0]) > PICKUP_COUNT_WHILE_MOTIONLESS) ||
                                            (ABS(external_accel_disturbance_cnt[1]) > PICKUP_COUNT_WHILE_MOTIONLESS);

            if (potentialPickupCnt_ > PICKUP_COUNT_WHILE_MOTIONLESS || accelBasedMotionDetected) {
              AnkiInfo( "IMUFilter.PDWhileStationary", "acc (%f, %f, %f), gyro (%f, %f, %f), cliff %d",
                    accel_robot_frame_filt[0], accel_robot_frame_filt[1], accel_robot_frame_filt[2],
                    gyro_robot_frame_filt[0], gyro_robot_frame_filt[1], gyro_robot_frame_filt[2],
                    cliffBasedPickupDetect);
              SetPickupDetect(true);
            }

          } else {

            // Do conservative check for pickup.
            // Only when we're really sure it's moving!
            // TODO: Make this smarter!
            if (CheckPickupWhileMoving() || cliffBasedPickupDetect || gyroZBasedMotionDetect) {
              if (++potentialPickupCnt_ > PICKUP_COUNT_WHILE_MOVING) {
                SetPickupDetect(true);
                AnkiInfo( "IMUFilter.PickupDetected", "accX %f, accY %f, accZ %f, cliff %d, gyroZ %d",
                         accel_robot_frame_filt[0], accel_robot_frame_filt[1], accel_robot_frame_filt[2], cliffBasedPickupDetect, gyroZBasedMotionDetect);
              }
            } else {
              potentialPickupCnt_ = 0;
            }

          }

        }
      }

      // Update the last time motion was detected
      void DetectMotion()
      {
        u32 currTime = HAL::GetTimeStamp();

        if (AreMotorsMoving() ||

            (IsBiasFilterComplete() &&
            (ABS(gyro_[0]) > GYRO_MOTION_THRESHOLD ||
             ABS(gyro_[1]) > GYRO_MOTION_THRESHOLD ||
             ABS(gyro_[2]) > GYRO_MOTION_THRESHOLD)) ||

            (!IsBiasFilterComplete() &&
             (ABS(imu_data_.gyro[0]) > GYRO_MOTION_PRECALIB_THRESHOLD ||
              ABS(imu_data_.gyro[1]) > GYRO_MOTION_PRECALIB_THRESHOLD ||
              ABS(imu_data_.gyro[2]) > GYRO_MOTION_PRECALIB_THRESHOLD)) ||

            ABS(accel_robot_frame_high_pass[0]) > ACCEL_MOTION_THRESH ||
            ABS(accel_robot_frame_high_pass[1]) > ACCEL_MOTION_THRESH ||
            ABS(accel_robot_frame_high_pass[2]) > ACCEL_MOTION_THRESH) {
          lastMotionDetectedTime_ms = currTime;
        }

        // TODO: Gyro seems to be sensitive enough that it's sufficient for detecting motion, but if
        //       that's not the case, check for changes in gravity vector.
        // ...


        isMotionDetected_ = (lastMotionDetectedTime_ms + MOTION_DETECT_TIMEOUT_MS) >= currTime;

      }

      // Simple poke detect
      // If wheels aren't moving but a sudden rotation about z-axis was detected
      void DetectPoke()
      {
        // Do nothing during cooldown period
        TimeStamp_t currTime = HAL::GetTimeStamp();
        if (currTime - _lastPokeDetectTime < POKE_DETECT_COOLDOWN_MS) {
          _peakGyroStartTime = currTime;
          return;
        }
        // Do nothing if motors are moving, as detecting pokes while the robot is attempting to move is far more challenging.
        // Also don't attempt to detect pokes if a pickup was detected.
        // TODO(GB): Poke detection should still run if the robot is being held upright in the user's hand.
        if (!AreMotorsMoving() && !pickedUp_) {
          // Check for a gyro rotation spike
          if (fabsf(gyro_robot_frame_filt[2]) > PEAK_GYRO_THRESHOLD) {
            _peakGyroMaxTime = currTime;
          } else {
            if ((_peakGyroMaxTime > _peakGyroStartTime) && (_peakGyroMaxTime - _peakGyroStartTime < MAX_GYRO_PEAK_DURATION_MS)) {
              AnkiInfo( "IMUFilter.PokeDetected.Gyro", "");
              _peakGyroStartTime = currTime;
              _lastPokeDetectTime = currTime;
              RobotInterface::RobotPoked m;
              RobotInterface::SendMessage(m);
            } else {
              _peakGyroStartTime = currTime;
            }
          }
        } else {
          _peakGyroStartTime = currTime;
        }
      }

      // This pitch measurement isn't precise to begin with, but it's extra imprecise when the head is moving
      // so be careful relying on it when the head is moving!
      void UpdatePitch()
      {
        f32 headAngle = HeadController::GetAngleRad();

        if (prevHeadAngle_ != UNINIT_HEAD_ANGLE) {
          const f32 accelBasedPitch = atan2f(imu_data_.accel[0], imu_data_.accel[2]) - headAngle;
          const f32 gyroBasedPitch = pitch_ - (gyro_robot_frame[1] * CONTROL_DT) - (headAngle - prevHeadAngle_);

          // Complementary filter to mostly trust gyro integration for current pitch in the short term
          // but always approach accelerometer-based pitch in the "long" term. Because of things like
          // keepaway and pounce which require fast and somewhat accurate measurements of pitch, use
          // a different coefficient while the wheels are moving.
          const f32 coeff = (WheelController::AreWheelsPowered() || WheelController::AreWheelsMoving() || HeadController::IsMoving()) ?
                              PITCH_FILT_COEFF_MOVING :
                              PITCH_FILT_COEFF;
          pitch_ = (coeff * gyroBasedPitch) + ((1.f - coeff) * accelBasedPitch);
        }

        prevHeadAngle_ = headAngle;

        //AnkiDebugPeriodic(50, "RobotPitch", "%f deg (motion %d, gyro %f)", RAD_TO_DEG_F32(pitch_), MotionDetected(), gyro_robot_frame_filt[1]);
      }

      void UpdateRoll()
      {
        const f32 headAngle = HeadController::GetAngleRad();
        const f32 accelBasedRoll = atan2f(imu_data_.accel[1], imu_data_.accel[2] * cosf(headAngle));
        const f32 gyroBasedRoll = roll_ - (gyro_robot_frame[0] * CONTROL_DT);

        // Complementary filter to mostly trust gyro integration for current roll in the short term
        // but always approach accelerometer-based roll in the "long" term.
        const bool isMoving = WheelController::AreWheelsPowered() || WheelController::AreWheelsMoving() || HeadController::IsMoving();
        const f32 coeff = isMoving ? ROLL_FILT_COEFF_MOVING : ROLL_FILT_COEFF;
        roll_ = (coeff * gyroBasedRoll) + ((1.f - coeff) * accelBasedRoll);
//        AnkiDebugPeriodic(250, "IMUFilter.UpdateRoll",
//                         "Filtered, accel-based, gyro-based: %f %f %f, (In motion: %d)",
//                         RAD_TO_DEG_F32(roll_),
//                         RAD_TO_DEG_F32(accelBasedRoll),
//                         RAD_TO_DEG_F32(gyroBasedRoll),
//                         isMoving);
      }

      Result Update()
      {
        Result retVal = RESULT_OK;

        // Get IMU data
        // NB: Only call IMUReadData once per mainExecution tic!
        while (HAL::IMUReadData(imu_data_)) {

        // IMU temperature-induced bias correction
        //
        // Gyro zero-rate bias changes with IMU temperature, so a more aggressive gyro bias filter coefficient
        // is required for times when IMU temperature is changing 'rapidly'. If IMU temperature has changed
        // a lot in the past x seconds, apply a more aggressive gyro bias filter coef.
        // Use a faster rate for playpen, since it needs to log IMU temperature changes.
        const u32 imuTempReadingRate_ms = FACTORY_TEST ? (1*1000) : (10*1000);
        const f32 temperatureDiffThresh_degC = 0.5f;
        const TimeStamp_t curTime = HAL::GetTimeStamp();

        if (curTime > timeOfLastImuTempSample_ms_ + imuTempReadingRate_ms && IsBiasFilterComplete()) {
          const bool temperatureChanging = fabsf(lastImuTempSample_degC_ - imu_data_.temperature_degC) > temperatureDiffThresh_degC;
          gyroBiasCoeff_ = temperatureChanging ?
            GYRO_BIAS_FILT_COEFF_TEMP_CHANGING :
            GYRO_BIAS_FILT_COEFF_NORMAL;

          lastImuTempSample_degC_ = imu_data_.temperature_degC;
          timeOfLastImuTempSample_ms_ = curTime;

          // Also send an IMUTemperature message to engine
          RobotInterface::IMUTemperature m;
          m.temperature_degC = imu_data_.temperature_degC;
          RobotInterface::SendMessage(m);
        }

        ////// Gyro Update //////

        // Bias corrected gyro readings
        for (int i=0 ; i<3 ; i++) {
          gyro_[i] = imu_data_.gyro[i] - gyro_bias_filt[i];
        }

#ifndef SIMULATOR
        // VECTOR: Correct for observed sensitivity error on z axis of gyro (VIC-285)
        // It has been observed that the z axis gyro usually reports about a 1.03% higher
        // rate than it is actually experiencing, so simply scale it here.
         gyro_[2] *= 0.989f;
#endif


        // Update gyro bias filter
        DetectMotion();

        UpdateGyroBiasFilter(imu_data_.gyro);

        // Don't do any other IMU updates until head is calibrated
        if (!HeadController::IsCalibrated()) {
          pitch_ = 0.f;
          prevHeadAngle_ = UNINIT_HEAD_ANGLE;
          ResetPickupVars();
          return retVal;
        }

        if (!IsBiasFilterComplete()) {
          return retVal;
        }

        // Compute head angle wrt to world horizontal plane
        const f32 headAngle = HeadController::GetAngleRad();  // TODO: Use encoders or accelerometer data? If encoders,
                                                              // may need to use accelerometer data anyway for when it's on ramps.

        // Compute rotation speeds in robot XY-plane.
        // https://www.chrobotics.com/library/understanding-euler-angles
        // http://ocw.mit.edu/courses/mechanical-engineering/2-017j-design-of-electromechanical-robotic-systems-fall-2009/course-text/MIT2_017JF09_ch09.pdf
        //
        // r: roll angle (x-axis), p: pitch angle (y-axis), y: yaw angle (z-axis)
        //
        //            |  1    sin(r)*tan(p)    cos(r)*tan(p)  |
        // D(r,p,y) = |  0       cos(r)           -sin(r)     |
        //            |  0    sin(r)/cos(p)    cos(r)/cos(p)  |
        //
        // Rotation in robot frame = D * [dr/dt, dp/dt, dy,dt] where the latter vector is given by gyro readings.
        // In our case, we only care about yaw. In other words, it's always true that r = y = 0.
        // (NOTE: This is true as long as we don't start turning on ramps!!!)
        // So the result simplifies to...
        gyro_robot_frame[0] = gyro_[0] + gyro_[2] * tanf(headAngle);
        gyro_robot_frame[1] = gyro_[1];
        gyro_robot_frame[2] = gyro_[2] / cosf(headAngle);
        // TODO: We actually only care about gyro_robot_frame_filt[2]. Any point in computing the others?

        for(u8 i = 0; i < 3; ++i)
        {
          gyro_sign[i] = signbit(gyro_robot_frame_filt[i]);
        }

        // Filter gyro readings in robot frame
        LowPassFilter(gyro_robot_frame_filt, gyro_robot_frame, RATE_FILT_COEFF);


        ///// Accelerometer update /////

        LowPassFilter(accel_filt, imu_data_.accel, ACCEL_FILT_COEFF);

        // Compute accelerations in robot frame
        const f32 xzAccelMagnitude = std::hypot(imu_data_.accel[0], imu_data_.accel[2]);
        const f32 accel_angle_imu_frame = atan2_fast(imu_data_.accel[2], imu_data_.accel[0]);
        const f32 accel_angle_robot_frame = accel_angle_imu_frame + headAngle;

        accel_robot_frame[0] = xzAccelMagnitude * cosf(accel_angle_robot_frame);
        accel_robot_frame[1] = imu_data_.accel[1];
        accel_robot_frame[2] = xzAccelMagnitude * sinf(accel_angle_robot_frame);


        f32 prev_accel_robot_frame_filt[3] = { accel_robot_frame_filt[0],
                                               accel_robot_frame_filt[1],
                                               accel_robot_frame_filt[2] };

        // Filter accel readings in robot frame
        LowPassFilter(accel_robot_frame_filt, accel_robot_frame, ACCEL_FILT_COEFF);

        // High-pass filter accelerations
        HighPassFilter(accel_robot_frame_high_pass, accel_robot_frame_filt, prev_accel_robot_frame_filt, HP_ACCEL_FILT_COEFF);

        // Absolute values (fall-detection)
        accelMagnitudeSqrd_ = 0;
        for (int i=0 ; i<3 ; i++) {
          abs_accel_robot_frame_filt[i] = ABS(accel_robot_frame_filt[i]);
          accelMagnitudeSqrd_ += (imu_data_.accel[i] * imu_data_.accel[i]);
        }

#if(DEBUG_IMU_FILTER)
        PERIODIC_PRINT(200, "Accel angle %f %f\n", accel_angle_imu_frame, accel_angle_robot_frame);
        PERIODIC_PRINT(200, "Accel (robot frame): %f %f %f\n",
                       accel_robot_frame_filt[0],
                       accel_robot_frame_filt[1],
                       accel_robot_frame_filt[2]);
#endif

        UpdatePitch();
        UpdateRoll();

        // XY-plane rotation rate is robot frame z-axis rotation rate
        rotSpeed_ = gyro_robot_frame_filt[2];

        // Update orientation
        f32 dAngle = rotSpeed_ * CONTROL_DT;
        rot_ += dAngle;

        //MadgwickAHRSupdateIMU(gyro_[0], gyro_[1], gyro_[2],
        //                      imu_data_.accel[0], imu_data_.accel[1], imu_data_.accel[2]);


        // XXX: Commenting this out because pickup detection seems to be firing
        //      when the robot drives up ramp (or the side of a platform) and
        //      clearing pose history.
        DetectPickup();

        // Poke detection MUST occur after pick-up detection, to avoid confusing an
        // overly-aggressive pickup with a poke.
        DetectPoke();

        DetectFalling();

        DetectImpact();

        // Queue ImuData to be sent as part of RobotState
        auto& thisDataFrame = imuDataBuffer_.push_back();
        thisDataFrame.timestamp = curTime;
        thisDataFrame.gyroRobotFrame.x = gyro_robot_frame_filt[0];
        thisDataFrame.gyroRobotFrame.y = gyro_robot_frame_filt[1];
        thisDataFrame.gyroRobotFrame.z = gyro_robot_frame_filt[2];

        // Recording IMU data for sending to basestation
        if (isRecording_) {

#if(RECORD_AND_SEND_MODE == RECORD_AND_SEND_FILT_DATA)
          imuChunkMsg_.aX[recordDataIdx_] = accel_robot_frame_filt[0];
          imuChunkMsg_.aY[recordDataIdx_] = accel_robot_frame_filt[1];
          imuChunkMsg_.aZ[recordDataIdx_] = accel_robot_frame_filt[2];

          imuChunkMsg_.gX[recordDataIdx_] = gyro_robot_frame_filt[0];
          imuChunkMsg_.gY[recordDataIdx_] = gyro_robot_frame_filt[1];
          imuChunkMsg_.gZ[recordDataIdx_] = gyro_robot_frame_filt[2];


          // Send message when it's full
          if (++recordDataIdx_ == IMU_CHUNK_SIZE) {
            RobotInterface::SendMessage(imuChunkMsg_);
            recordDataIdx_ = 0;
            ++imuChunkMsg_.chunkId;

            if (imuChunkMsg_.chunkId == imuChunkMsg_.totalNumChunks) {
              AnkiDebug( "IMUFilter.IMURecording.Complete", "time %dms", HAL::GetTimeStamp());
              isRecording_ = false;
            }
          }
#else
          ++sentIMUDataMsgs_;
          if (sentIMUDataMsgs_ == totalIMUDataMsgsToSend_) {
            AnkiDebug( "IMUFilter.IMURecording.CompleteRaw", "time %dms", HAL::GetTimeStamp());
            isRecording_ = false;
            imuRawDataMsg_.order = 2;  // 2 == last msg of sequence
          }

          for (int i=0 ; i<3 ; i++) {
            imuRawDataMsg_.a[i] = (int16_t) imu_data_.accel[i]; // mm/s^2
            imuRawDataMsg_.g[i] = (int16_t) 1000.f * imu_data_.gyro[i]; // millirad/sec
          }

          RobotInterface::SendMessage(imuRawDataMsg_);
          imuRawDataMsg_.order = 1;    // 1 == intermediate msg of sequence
#endif

        }

        } // while (HAL::IMUReadData())

        return retVal;

      } // Update()

      HAL::IMU_DataStructure GetLatestRawData()
      {
        return imu_data_;
      }

      const f32* GetBiasCorrectedGyroData()
      {
        return gyro_;
      }

      ImuDataBufferType& GetImuDataBuffer()
      {
        return imuDataBuffer_;
      }

      f32 GetRotation()
      {
        //return _zAngle;  // Computed from 3D orientation tracker (Madgwick filter)
        return rot_.ToFloat();     // Computed from simplified yaw-only tracker
      }

      f32 GetRotationSpeed()
      {
        return rotSpeed_;
      }

      f32 GetPitch()
      {
        return pitch_;
      }

      f32 GetRoll()
      {
        return roll_;
      }

      bool IsPickedUp()
      {
        return pickedUp_ || falling_;
      }

      bool IsBeingHeld()
      {
        return IsPickedUp() && isMotionDetected_;
      }

      bool IsMotionDetected()
      {
        return isMotionDetected_;
      }

      bool IsFalling()
      {
        return falling_;
      }

      bool IsBiasFilterComplete()
      {
        return biasFilterComplete_;
      }

      const f32* GetGyroBias()
      {
        return gyro_bias_filt;
      }

      void RecordAndSend(const u32 length_ms)
      {
        AnkiDebug( "IMUFilter.IMURecording.Start", "time = %dms", HAL::GetTimeStamp());
        isRecording_ = true;
#if(RECORD_AND_SEND_MODE == RECORD_AND_SEND_FILT_DATA)
        recordDataIdx_ = 0;
        imuChunkMsg_.seqId++;
        imuChunkMsg_.chunkId=0;
        imuChunkMsg_.totalNumChunks = length_ms / (ROBOT_TIME_STEP_MS * IMU_CHUNK_SIZE);
#else
        imuRawDataMsg_.order = 0; // 0 == first message of sequence
        sentIMUDataMsgs_ = 0;
        totalIMUDataMsgsToSend_ = length_ms / ROBOT_TIME_STEP_MS;
#endif
      }

    } // namespace IMUFilter
  } // namespace Vector
} // namespace Anki
