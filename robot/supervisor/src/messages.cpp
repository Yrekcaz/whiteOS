#include "messages.h"
#include "anki/cozmo/robot/cozmoBot.h"
#include "anki/cozmo/robot/hal.h"
#include <math.h>

#include "clad/robotInterface/messageRobotToEngine.h"
#include "clad/robotInterface/messageRobotToEngine_send_helper.h"
#include "clad/robotInterface/messageEngineToRobot_send_helper.h"

#include <string.h>

#include "backpackLightController.h"
#include "dockingController.h"
#include "headController.h"
#include "imuFilter.h"
#include "liftController.h"
#include "localization.h"
#include "pathFollower.h"
#include "pickAndPlaceController.h"
#include "proxSensors.h"
#include "speedController.h"
#include "steeringController.h"
#include "testModeController.h"
#include "wheelController.h"

#include <stdio.h>

#include "anki/cozmo/robot/logging.h"

#define SEND_TEXT_REDIRECT_TO_STDOUT 0

namespace Anki {
  namespace Vector {
    namespace Messages {

      namespace {

        u8 pktBuffer_[2048];

        static RobotState robotState_;

        // Flag for receipt of sync message
        bool syncRobotReceived_ = false;
        bool syncRobotAckSent_ = false;

#ifdef SIMULATOR
        bool isForcedDelocalizing_ = false;
#endif

        // For only sending robot state messages every STATE_MESSAGE_FREQUENCY
        // times through the main loop
        u32 robotStateMessageCounter_ = 0;
        bool calmModeEnabledByEngine_ = false;
        bool calmMode_ = false;
        TimeStamp_t timeToEnableCalmMode_ms_ = 0;

      } // private namespace

// #pragma mark --- Messages Method Implementations ---

      Result Init()
      {
        return RESULT_OK;
      }

      void ProcessMessage(RobotInterface::EngineToRobot& msg)
      {
        switch(msg.tag)
        {
          #include "clad/robotInterface/messageEngineToRobot_switch_from_0x01_to_0x4F.def"

          default:
            AnkiWarn( "Messages.ProcessBadTag_EngineToRobot.Recvd", "Received message with bad tag %x", msg.tag);
        }
      } // ProcessMessage()

      void UpdateRobotStateMsg()
      {
        robotState_.timestamp = HAL::GetTimeStamp();

        robotState_.pose_frame_id = Localization::GetPoseFrameId();
        robotState_.pose_origin_id = Localization::GetPoseOriginId();

        robotState_.pose.x = Localization::GetCurrPose_x();
        robotState_.pose.y = Localization::GetCurrPose_y();
        robotState_.pose.z = 0;
        robotState_.pose.angle = Localization::GetCurrPose_angle().ToFloat();
        robotState_.pose.pitch_angle = IMUFilter::GetPitch();
        robotState_.pose.roll_angle = IMUFilter::GetRoll();
        WheelController::GetFilteredWheelSpeeds(robotState_.lwheel_speed_mmps, robotState_.rwheel_speed_mmps);
        robotState_.headAngle  = HeadController::GetAngleRad();
        robotState_.liftAngle  = LiftController::GetAngleRad();

        HAL::IMU_DataStructure imuData = IMUFilter::GetLatestRawData();
        robotState_.accel.x = imuData.accel[0];
        robotState_.accel.y = imuData.accel[1];
        robotState_.accel.z = imuData.accel[2];
        robotState_.gyro.x = IMUFilter::GetBiasCorrectedGyroData()[0];
        robotState_.gyro.y = IMUFilter::GetBiasCorrectedGyroData()[1];
        robotState_.gyro.z = IMUFilter::GetBiasCorrectedGyroData()[2];
        
        auto& imuDataBuffer = IMUFilter::GetImuDataBuffer();
        for (int i=0 ; i < IMUConstants::IMU_FRAMES_PER_ROBOT_STATE ; i++) {
          if (!imuDataBuffer.empty()) {
            robotState_.imuData[i] = imuDataBuffer.front();
            imuDataBuffer.pop_front();
          } else {
            static IMUDataFrame invalidDataFrame{0, {0, 0, 0}};
            robotState_.imuData[i] = invalidDataFrame;
          }
        }

        for (int i=0 ; i < HAL::CLIFF_COUNT ; i++) {
          robotState_.cliffDataRaw[i] = ProxSensors::GetCliffValue(i);
        }
        
        robotState_.proxData = ProxSensors::GetProxData();

        robotState_.backpackTouchSensorRaw = HAL::GetButtonState(HAL::BUTTON_CAPACITIVE);

        robotState_.cliffDetectedFlags = ProxSensors::GetCliffDetectedFlags();
        
        robotState_.whiteDetectedFlags = ProxSensors::GetWhiteDetectedFlags();

        robotState_.currPathSegment = PathFollower::GetCurrPathSegment();

        robotState_.batteryVoltage = HAL::BatteryGetVoltage();
        robotState_.chargerVoltage = HAL::ChargerGetVoltage();
        robotState_.battTemp_C = HAL::BatteryGetTemperature_C();

        robotState_.status = 0;
        #define SET_STATUS_BIT(expr, bit) robotState_.status |= ((expr) ? EnumToUnderlyingType(RobotStatusFlag::bit) : 0)
        const bool areWheelsMoving = WheelController::AreWheelsMoving() || 
                                     SteeringController::GetMode() == SteeringController::SM_POINT_TURN;
        const bool isMoving        = HeadController::IsMoving() || LiftController::IsMoving() || areWheelsMoving;
        SET_STATUS_BIT(areWheelsMoving,                             ARE_WHEELS_MOVING);
        SET_STATUS_BIT(isMoving,                                    IS_MOVING);
        SET_STATUS_BIT(PickAndPlaceController::IsCarryingBlock(),   IS_CARRYING_BLOCK);
        SET_STATUS_BIT(PickAndPlaceController::IsBusy(),            IS_PICKING_OR_PLACING);
        SET_STATUS_BIT(IMUFilter::IsPickedUp(),                     IS_PICKED_UP);
        SET_STATUS_BIT(IMUFilter::IsBeingHeld(),                    IS_BEING_HELD);
        SET_STATUS_BIT(IMUFilter::IsMotionDetected(),               IS_MOTION_DETECTED);
        SET_STATUS_BIT(HAL::GetButtonState(HAL::BUTTON_POWER) > 0,  IS_BUTTON_PRESSED);
        SET_STATUS_BIT(PathFollower::IsTraversingPath(),            IS_PATHING);
        SET_STATUS_BIT(LiftController::IsInPosition(),              LIFT_IN_POS);
        SET_STATUS_BIT(HeadController::IsInPosition(),              HEAD_IN_POS);
        SET_STATUS_BIT(calmMode_,                                   CALM_POWER_MODE);
        SET_STATUS_BIT(HAL::BatteryIsDisconnected(),                IS_BATTERY_DISCONNECTED);
        SET_STATUS_BIT(HAL::BatteryIsOnCharger(),                   IS_ON_CHARGER);
        SET_STATUS_BIT(HAL::BatteryIsCharging(),                    IS_CHARGING);
        SET_STATUS_BIT(HAL::BatteryIsOverheated(),                  IS_BATTERY_OVERHEATED);
        SET_STATUS_BIT(HAL::BatteryIsLow(),                         IS_BATTERY_LOW);
        SET_STATUS_BIT(HAL::IsShutdownImminent(),                   IS_SHUTDOWN_IMMINENT);
        SET_STATUS_BIT(ProxSensors::IsAnyCliffDetected(),           CLIFF_DETECTED);
        SET_STATUS_BIT(IMUFilter::IsFalling(),                      IS_FALLING);
        SET_STATUS_BIT(HAL::AreEncodersDisabled(),                  ENCODERS_DISABLED);
        SET_STATUS_BIT(HeadController::IsEncoderInvalid(),          ENCODER_HEAD_INVALID);
        SET_STATUS_BIT(LiftController::IsEncoderInvalid(),          ENCODER_LIFT_INVALID);
#ifdef  SIMULATOR
        SET_STATUS_BIT(isForcedDelocalizing_,                       IS_PICKED_UP);
#endif
        #undef SET_STATUS_BIT


        // Send state message
        ++robotStateMessageCounter_;
        const s32 messagePeriod = calmMode_ ? STATE_MESSAGE_FREQUENCY_CALM : STATE_MESSAGE_FREQUENCY;
        if(robotStateMessageCounter_ >= messagePeriod) {
          SendRobotStateMsg();
          robotStateMessageCounter_ = 0;
        }

      }

      RobotState const& GetRobotStateMsg() {
        return robotState_;
      }


// #pragma --- Message Dispatch Functions ---

      void Process_syncRobot(const RobotInterface::SyncRobot& msg)
      {
        AnkiInfo( "Messages.Process_syncRobot.Recvd", "");

        // Set SyncRobot received flag
        // Acknowledge in Update()
        syncRobotReceived_ = true;

        // TODO: Compare message ID to robot ID as a handshake?

        // Reset pose history and frameID to zero
        Localization::ResetPoseFrame();

        AnkiInfo("watchdog_reset_count", "%d", HAL::GetWatchdogResetCounter());
      } // ProcessRobotInit()


      void Process_shutdown(const RobotInterface::Shutdown& msg)
      {
        HAL::Shutdown();
      }

      void Process_calmPowerMode(const RobotInterface::CalmPowerMode& msg)
      {
        // NOTE: This used to actually enable calm mode in syscon, but since "quiet" mode
        //       was implemented in syscon where encoders are "off" when the motors are
        //       not being driven leaving minimal difference between calm mode and active mode
        //       in terms of battery life, this now only throttles RobotState messages being 
        //       sent to engine since it still results in a 10+% reduction in CPU consumption.
        //       Not going into syscon calm mode also means that motor calibrations are no 
        //       longer necessary as a precaution when leaving calm mode.
        AnkiInfo("Messages.Process_calmPowerMode.enable", "enable: %d", msg.enable);
        calmModeEnabledByEngine_ = msg.enable;
        calmMode_ = msg.enable;
      }

      void Process_absLocalizationUpdate(const RobotInterface::AbsoluteLocalizationUpdate& msg)
      {
        // Don't modify localization while running path following test.
        // The point of the test is to see how well it follows a path
        // assuming perfect localization.
        if (TestModeController::GetMode() == TestMode::TM_PATH_FOLLOW) {
          return;
        }

        // TODO: Double-check that size matches expected size?

        // TODO: take advantage of timestamp

        f32 currentMatX       = msg.xPosition;
        f32 currentMatY       = msg.yPosition;
        Radians currentMatHeading = msg.headingAngle;
        /*Result res =*/ Localization::UpdatePoseWithKeyframe(msg.origin_id, msg.pose_frame_id, msg.timestamp, currentMatX, currentMatY, currentMatHeading.ToFloat());

        /*
        AnkiInfo( "Messages.Process_absLocalizationUpdate.Recvd", "Result %d, currTime=%d, updated frame time=%d: (%.3f,%.3f) at %.1f degrees (frame = %d)\n",
              res,
              HAL::GetTimeStamp(),
              msg.timestamp,
              currentMatX, currentMatY,
              currentMatHeading.getDegrees(),
              Localization::GetPoseFrameId());
         */
      } // ProcessAbsLocalizationUpdateMessage()

      void Process_forceDelocalizeSimulatedRobot(const RobotInterface::ForceDelocalizeSimulatedRobot& msg)
      {
#ifdef SIMULATOR
        isForcedDelocalizing_ = true;
#endif
      }

      void Process_dockingErrorSignal(const DockingErrorSignal& msg)
      {
        DockingController::SetDockingErrorSignalMessage(msg);
      }

      void Update()
      {
        // Send ACK of SyncRobot message when system is ready
        if (!syncRobotAckSent_) {
          if (syncRobotReceived_ &&
              IMUFilter::IsBiasFilterComplete() &&
              LiftController::IsCalibrated() &&
              HeadController::IsCalibrated()) {
            RobotInterface::SyncRobotAck syncRobotAckMsg;
            memcpy(&syncRobotAckMsg.sysconVersion, HAL::GetSysconVersionInfo(), 16);
            while (RobotInterface::SendMessage(syncRobotAckMsg) == false);
            syncRobotAckSent_ = true;

            // Send up gyro calibration
            // Since the bias is typically calibrate before the robot is even connected,
            // this is the time when the data can actually be sent up to engine.
            AnkiInfo("Messages.Update.GyroCalibrated", "%f %f %f",
                     RAD_TO_DEG_F32(IMUFilter::GetGyroBias()[0]),
                     RAD_TO_DEG_F32(IMUFilter::GetGyroBias()[1]),
                     RAD_TO_DEG_F32(IMUFilter::GetGyroBias()[2]));
          }
        }

        // Temporarily unset calm mode when button is pressed so that 
        // we can still go into pairing/debug screens
        TimeStamp_t now_ms = HAL::GetTimeStamp();
        static const u32 TEMP_CALM_MODE_DISABLE_TIME_MS = 1000;
        if (calmModeEnabledByEngine_) {
          if (HAL::GetButtonState(HAL::BUTTON_POWER)) {
            calmMode_ = false;
            timeToEnableCalmMode_ms_ = now_ms + TEMP_CALM_MODE_DISABLE_TIME_MS;
          } else if (timeToEnableCalmMode_ms_ != 0 && timeToEnableCalmMode_ms_ < now_ms) {
            calmMode_ = true;
            timeToEnableCalmMode_ms_ = 0;
          }
        }

        // Process incoming messages
        u32 dataLen;

        // Each packet is a single message
        while((dataLen = HAL::RadioGetNextPacket(pktBuffer_)) > 0)
        {
          Anki::Vector::RobotInterface::EngineToRobot msgBuf;

          // Copy into structured memory
          memcpy(msgBuf.GetBuffer(), pktBuffer_, dataLen);
          if (!msgBuf.IsValid())
          {
            AnkiWarn( "Receiver.ReceiveData.Invalid", "Receiver got %02x[%d] invalid", pktBuffer_[0], dataLen);
          }
          else if (msgBuf.Size() != dataLen)
          {
            AnkiWarn( "Receiver.ReceiveData.SizeError", "Parsed message size error %d != %d", dataLen, msgBuf.Size());
          }
          else
          {
            Anki::Vector::Messages::ProcessMessage(msgBuf);
          }
        }

      }

      void Process_clearPath(const RobotInterface::ClearPath& msg) {
        SpeedController::SetUserCommandedDesiredVehicleSpeed(0);
        PathFollower::ClearPath();
        //SteeringController::ExecuteDirectDrive(0,0);
      }

      void Process_appendPathSegArc(const RobotInterface::AppendPathSegmentArc& msg) {
        PathFollower::AppendPathSegment_Arc(msg.x_center_mm, msg.y_center_mm,
                                            msg.radius_mm, msg.startRad, msg.sweepRad,
                                            msg.speed.target, msg.speed.accel, msg.speed.decel);
      }

      void Process_appendPathSegLine(const RobotInterface::AppendPathSegmentLine& msg) {
        PathFollower::AppendPathSegment_Line(msg.x_start_mm, msg.y_start_mm,
                                             msg.x_end_mm, msg.y_end_mm,
                                             msg.speed.target, msg.speed.accel, msg.speed.decel);
      }

      void Process_appendPathSegPointTurn(const RobotInterface::AppendPathSegmentPointTurn& msg) {
        PathFollower::AppendPathSegment_PointTurn(msg.x_center_mm, msg.y_center_mm, msg.startRad, msg.targetRad,
                                                  msg.speed.target, msg.speed.accel, msg.speed.decel,
                                                  msg.angleTolerance, msg.useShortestDir);
      }

      void Process_trimPath(const RobotInterface::TrimPath& msg) {
        PathFollower::TrimPath(msg.numPopFrontSegments, msg.numPopBackSegments);
      }

      void Process_executePath(const RobotInterface::ExecutePath& msg) {
        const bool result = PathFollower::StartPathTraversal(msg.pathID);
        AnkiInfo( "Messages.Process_executePath.Result", "id=%d result=%d", msg.pathID, result);
      }

      void Process_dockWithObject(const DockWithObject& msg)
      {
        AnkiInfo( "Messages.Process_dockWithObject.Recvd", "action %hhu, dockMethod %hhu, doLiftLoadCheck %d, backUpWhileLiftingCube %d, speed %f, accel %f, decel %f",
                 msg.action, msg.dockingMethod, msg.doLiftLoadCheck, msg.backUpWhileLiftingCube, msg.speed_mmps, msg.accel_mmps2, msg.decel_mmps2);

        PickAndPlaceController::SetBackUpWhileLiftingCube(msg.backUpWhileLiftingCube);
        
        DockingController::SetDockingMethod(msg.dockingMethod);

        // Currently passing in default values for rel_x, rel_y, and rel_angle
        PickAndPlaceController::DockToBlock(msg.action,
                                            msg.doLiftLoadCheck,
                                            msg.speed_mmps,
                                            msg.accel_mmps2,
                                            msg.decel_mmps2,
                                            0, 0, 0,
                                            msg.numRetries);
      }

      void Process_placeObjectOnGround(const PlaceObjectOnGround& msg)
      {
        //AnkiInfo( "Messages.Process_placeObjectOnGround.Recvd", "");
        PickAndPlaceController::PlaceOnGround(msg.speed_mmps,
                                              msg.accel_mmps2,
                                              msg.decel_mmps2,
                                              msg.rel_x_mm,
                                              msg.rel_y_mm,
                                              msg.rel_angle);
      }

      void Process_startMotorCalibration(const RobotInterface::StartMotorCalibration& msg) {
        const bool autoStarted = false;
        if (msg.calibrateHead) {
          HeadController::StartCalibrationRoutine(autoStarted, msg.reason);
        }

        if (msg.calibrateLift) {
          LiftController::StartCalibrationRoutine(autoStarted, msg.reason);
        }
      }


      void Process_drive(const RobotInterface::DriveWheels& msg) {
        // Do not process external drive commands if following a test path
        if (PathFollower::IsTraversingPath()) {
          AnkiWarn( "Messages.Process_drive.IgnoringBecauseAlreadyOnPath", "");
          return;
        }

        //AnkiInfo( "Messages.Process_drive.Executing", "left=%f mm/s, right=%f mm/s", msg.lwheel_speed_mmps, msg.rwheel_speed_mmps);

        //PathFollower::ClearPath();
        SteeringController::ExecuteDirectDrive(msg.lwheel_speed_mmps, msg.rwheel_speed_mmps,
                                               msg.lwheel_accel_mmps2, msg.rwheel_accel_mmps2);
      }

      void Process_driveCurvature(const RobotInterface::DriveWheelsCurvature& msg) {
        SteeringController::ExecuteDriveCurvature(msg.speed,
                                                  msg.curvatureRadius_mm,
                                                  msg.accel);
      }

      void Process_moveLift(const RobotInterface::MoveLift& msg) {
        LiftController::SetAngularVelocity(msg.speed_rad_per_sec, MAX_LIFT_ACCEL_RAD_PER_S2);
      }

      void Process_moveHead(const RobotInterface::MoveHead& msg) {
        HeadController::SetAngularVelocity(msg.speed_rad_per_sec, MAX_HEAD_ACCEL_RAD_PER_S2);
      }

      // Send ack of head motor action
      void AckMotorCommand(u8 actionID) {
        if (actionID != 0) {
          RobotInterface::MotorActionAck ack;
          ack.actionID = actionID;
          RobotInterface::SendMessage(ack);
        }
      }

      void Process_liftHeight(const RobotInterface::SetLiftHeight& msg) {
        //AnkiInfo( "Messages.Process_liftHeight.Recvd", "height %f, maxSpeed %f, duration %f", msg.height_mm, msg.max_speed_rad_per_sec, msg.duration_sec);
        if (msg.duration_sec > 0) {
          LiftController::SetDesiredHeightByDuration(msg.height_mm, 0.1f, 0.1f, msg.duration_sec);
        } else {
          LiftController::SetDesiredHeight(msg.height_mm, msg.max_speed_rad_per_sec, msg.accel_rad_per_sec2);
        }
        AckMotorCommand(msg.actionID);
      }

      void Process_setLiftAngle(const RobotInterface::SetLiftAngle& msg) {
        // AnkiInfo( "Messages.Process_liftAngle.Recvd", 
        //           "height %f, maxSpeed %f, duration %f", 
        //           msg.angle_rad, msg.max_speed_rad_per_sec, msg.duration_sec);
        if (msg.duration_sec > 0) {
          LiftController::SetDesiredAngleByDuration(msg.angle_rad, 0.1f, 0.1f, msg.duration_sec);
        } else {
          LiftController::SetDesiredAngle(msg.angle_rad, msg.max_speed_rad_per_sec, msg.accel_rad_per_sec2);
        }
        AckMotorCommand(msg.actionID);
      }

      void Process_headAngle(const RobotInterface::SetHeadAngle& msg) {
        //AnkiInfo( "Messages.Process_headAngle.Recvd", "angle %f, maxSpeed %f, duration %f", msg.angle_rad, msg.max_speed_rad_per_sec, msg.duration_sec);
        if (msg.duration_sec > 0) {
          HeadController::SetDesiredAngleByDuration(msg.angle_rad, 0.1f, 0.1f, msg.duration_sec);
        } else {
          HeadController::SetDesiredAngle(msg.angle_rad, msg.max_speed_rad_per_sec, msg.accel_rad_per_sec2);
        }
        AckMotorCommand(msg.actionID);
      }

      void Process_setBodyAngle(const RobotInterface::SetBodyAngle& msg)
      {
        SteeringController::ExecutePointTurn(msg.angle_rad, msg.max_speed_rad_per_sec,
                                             msg.accel_rad_per_sec2,
                                             msg.accel_rad_per_sec2,
                                             msg.angle_tolerance,
                                             msg.use_shortest_direction,
                                             msg.num_half_revolutions);
        AckMotorCommand(msg.actionID);
      }

      void Process_setCarryState(const CarryStateUpdate& update)
      {
        PickAndPlaceController::SetCarryState(update.state);
      }

      void Process_imuRequest(const IMURequest& msg)
      {
        IMUFilter::RecordAndSend(msg.length_ms);
      }

      void Process_turnInPlaceAtSpeed(const RobotInterface::TurnInPlaceAtSpeed& msg) {
        //AnkiInfo( "Messages.Process_turnInPlaceAtSpeed.Recvd", "speed %f rad/s, accel %f rad/s2", msg.speed_rad_per_sec, msg.accel_rad_per_sec2);
        SteeringController::ExecutePointTurn(msg.speed_rad_per_sec, msg.accel_rad_per_sec2);
      }

      void Process_stop(const RobotInterface::StopAllMotors& msg) {
        LiftController::SetAngularVelocity(0);
        HeadController::SetAngularVelocity(0);
        SteeringController::ExecuteDirectDrive(0,0);
      }

      void Process_startControllerTestMode(const StartControllerTestMode& msg)
      {
        TestModeController::Start((TestMode)(msg.mode), msg.p1, msg.p2, msg.p3);
      }

      void Process_cameraFOVInfo(const CameraFOVInfo& msg)
      {
        DockingController::SetCameraFieldOfView(msg.horizontalFOV, msg.verticalFOV);
      }

      void Process_rollActionParams(const RobotInterface::RollActionParams& msg) {
        PickAndPlaceController::SetRollActionParams(msg.liftHeight_mm,
                                                    msg.driveSpeed_mmps,
                                                    msg.driveAccel_mmps2,
                                                    msg.driveDuration_ms,
                                                    msg.backupDist_mm);
      }

      void Process_playpenStart(const RobotInterface::PlaypenStart& msg) {
      }

      void Process_printBodyData(const RobotInterface::PrintBodyData& msg) {
        HAL::PrintBodyData(msg.period_tics, msg.motors, msg.prox, msg.battery);
      }

      void Process_setControllerGains(const RobotInterface::ControllerGains& msg) {
        switch (msg.controller)
        {
          case ControllerChannel::controller_wheel:
          {
            WheelController::SetGains(msg.kp, msg.ki, msg.maxIntegralError);
            break;
          }
          case ControllerChannel::controller_head:
          {
            HeadController::SetGains(msg.kp, msg.ki, msg.kd, msg.maxIntegralError);
            break;
          }
          case ControllerChannel::controller_lift:
          {
            LiftController::SetGains(msg.kp, msg.ki, msg.kd, msg.maxIntegralError);
            break;
          }
          case ControllerChannel::controller_steering:
          {
            SteeringController::SetGains(msg.kp, msg.ki, msg.kd, msg.maxIntegralError); // Coopting structure
            break;
          }
          case ControllerChannel::controller_pointTurn:
          {
            SteeringController::SetPointTurnGains(msg.kp, msg.ki, msg.kd, msg.maxIntegralError);
            break;
          }
          default:
          {
            AnkiWarn( "Messages.Process_setControllerGains.InvalidController", "controller: %hhu",  msg.controller);
          }
        }
      }

      void Process_setMotionModelParams(const RobotInterface::SetMotionModelParams& msg)
      {
        Localization::SetMotionModelParams(msg.slipFactor);
      }

      void Process_abortDocking(const AbortDocking& msg)
      {
        DockingController::StopDocking();
      }

      void Process_checkLiftLoad(const RobotInterface::CheckLiftLoad& msg)
      {
        LiftController::CheckForLoad();
      }

      void Process_enableMotorPower(const RobotInterface::EnableMotorPower& msg)
      {
        switch(msg.motorID) {
          case MotorID::MOTOR_HEAD:
          {
            if (msg.enable) {
              HeadController::Enable();
            } else {
              HeadController::Disable();
            }
            break;
          }
          case MotorID::MOTOR_LIFT:
          {
            if (msg.enable) {
              LiftController::Enable();
            } else {
              LiftController::Disable();
            }
            break;
          }
          default:
          {
            AnkiWarn( "Messages.enableMotorPower.UnhandledMotorID", "%hhu", msg.motorID);
            break;
          }
        }
      }

      void Process_robotStoppedAck(const RobotInterface::RobotStoppedAck& msg)
      {
        AnkiInfo("Messages.Process_robotStoppedAck", "");
        SteeringController::Enable();
      }

      void Process_enableStopOnCliff(const RobotInterface::EnableStopOnCliff& msg)
      {
        ProxSensors::EnableStopOnCliff(msg.enable);
      }

      void Process_enableStopOnWhite(const RobotInterface::EnableStopOnWhite& msg)
      {
        ProxSensors::EnableStopOnWhite(msg.enable);
      }
      
      void Process_setCliffDetectThresholds(const SetCliffDetectThresholds& msg)
      {
        for (int i = 0 ; i < HAL::CLIFF_COUNT ; i++) {
          ProxSensors::SetCliffDetectThreshold(i, msg.thresholds[i]);
        }
      }
      
      void Process_setWhiteDetectThresholds(const SetWhiteDetectThresholds& msg)
      {
        for (int i = 0 ; i < HAL::CLIFF_COUNT ; i++) {
          ProxSensors::SetWhiteDetectThreshold(i, msg.whiteThresholds[i]);
        }
      }

      void Process_cliffAlignToWhiteAction(const RobotInterface::CliffAlignToWhiteAction& msg)
      {
        if (msg.start) {
          PickAndPlaceController::CliffAlignToWhite();
        } else {
          PickAndPlaceController::StopCliffAlignToWhite();
        }
      }

      void Process_enableBraceWhenFalling(const RobotInterface::EnableBraceWhenFalling& msg)
      {
        IMUFilter::EnableBraceWhenFalling(msg.enable);
      }

      void Process_recordHeading(RobotInterface::RecordHeading const& msg)
      {
        SteeringController::RecordHeading();
      }

      void Process_turnToRecordedHeading(RobotInterface::TurnToRecordedHeading const& msg)
      {
        SteeringController::ExecutePointTurnToRecordedHeading(DEG_TO_RAD_F32(msg.offset_deg),
                                                              DEG_TO_RAD_F32(msg.speed_degPerSec),
                                                              DEG_TO_RAD_F32(msg.accel_degPerSec2),
                                                              DEG_TO_RAD_F32(msg.decel_degPerSec2),
                                                              DEG_TO_RAD_F32(msg.tolerance_deg),
                                                              msg.numHalfRevs,
                                                              msg.useShortestDir);
      }

      void Process_setBackpackLights(RobotInterface::SetBackpackLights const& msg)
      {
        BackpackLightController::SetParams(msg);
      }

      void Process_setSystemLight(RobotInterface::SetSystemLight const& msg)
      {
        BackpackLightController::SetParams(msg);
      }


      void Process_setBackpackLayer(const RobotInterface::BackpackSetLayer& msg) {
        BackpackLightController::EnableLayer((BackpackLightLayer)msg.layer);
      }
      
// ----------- Send messages -----------------

      Result SendRobotStateMsg()
      {
        // Don't send robot state updates unless the init message was received
        if (!syncRobotReceived_) {
          return RESULT_FAIL;
        }


        if(RobotInterface::SendMessage(robotState_) == true) {
          #ifdef SIMULATOR
          {
            isForcedDelocalizing_ = false;
          }
          #endif

          return RESULT_OK;
        } else {
          return RESULT_FAIL;
        }
      }


      Result SendMotorCalibrationMsg(MotorID motor, bool calibStarted, bool autoStarted)
      {
        MotorCalibration m;
        m.motorID = motor;
        m.calibStarted = calibStarted;
        m.autoStarted = autoStarted;
        return RobotInterface::SendMessage(m) ? RESULT_OK : RESULT_FAIL;
      }

      Result SendMotorAutoEnabledMsg(MotorID motor, bool enabled)
      {
        MotorAutoEnabled m;
        m.motorID = motor;
        m.enabled = enabled;
        return RobotInterface::SendMessage(m) ? RESULT_OK : RESULT_FAIL;
      }

      Result SendMicDataFunction(const s16* latestMicData, uint32_t numSamples) 
      {
        static int chunkID = 0;
        static const int numChannels = 4;
        static const int samplesPerChunk = 80;
        static const int samplesPerDeinterlacedChunk = 160;
        static int16_t sampleBuffer[numChannels * samplesPerDeinterlacedChunk];
        RobotInterface::MicData micData{};
        micData.timestamp = HAL::GetTimeStamp();
        micData.robotStatusFlags = robotState_.status;
        micData.robotRotationAngle = robotState_.pose.angle;

        /*
        Deinterlace the audio before sending it to Engine/Anim. Coming into this method, we
        have chunks of 80 samples each, but the data for 4 mics are interleaved;
        
            m0, m1, m2, m3, m0, m1, m2, m3....
        
        What we need is;

            m0 (80x), m1 (80x), ....

        Since the recipient doesn't process data any more frequently than every 10ms, we're
        minimizing the message overhead by doing the deinterlacing here and sending the 10ms
        message instead of 2 5ms messages.
        */

        int chunkOffset = chunkID * samplesPerChunk;
        for (size_t channel=0; channel<numChannels; ++channel) {
          for (size_t sample=0; sample<samplesPerChunk; ++sample) {
            const auto sampleBufferIndex = channel*samplesPerDeinterlacedChunk + chunkOffset + sample;
            const auto latestMicDataIndex = sample*numChannels + channel;
            sampleBuffer[sampleBufferIndex] = latestMicData[latestMicDataIndex];
          }
        }

        chunkID = chunkID ? 0 : 1;
        if (chunkID == 0) {
          memcpy(micData.data, sampleBuffer, numChannels * samplesPerDeinterlacedChunk * sizeof(s16));
          return RobotInterface::SendMessage(micData) ? RESULT_OK : RESULT_FAIL;
        }
        return RESULT_OK;
      }

      Result SendMicDataMsgs()
      {
        while (HAL::HandleLatestMicData(&SendMicDataFunction))
        {
          // Keep calling HandleLatestMicData until it returns false
        }
        return RESULT_OK;
      }

      bool ReceivedInit()
      {
        return syncRobotReceived_;
      }

      void ResetInit()
      {
        syncRobotReceived_ = false;
        syncRobotAckSent_ = false;
      }

    } // namespace Messages

    namespace HAL {
      bool RadioSendMessage(const void *buffer, const u16 size, const u8 msgID)
      {
        //Stuff msgID up front
        size_t newSize = size + 1;
        u8 buf[newSize];

        memcpy(buf, &msgID, 1);
        memcpy(buf + 1, buffer, size);

        //fprintf(stderr, "RadioSendMsg: %02x [%d]", msgID, newSize);

        return HAL::RadioSendPacket(buf, newSize);
      }


    } // namespace HAL
  } // namespace Vector
} // namespace Anki
