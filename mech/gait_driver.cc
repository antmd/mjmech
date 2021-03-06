// Copyright 2014-2016 Josh Pieper, jjp@pobox.com.  All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "gait_driver.h"

#include "base/deadline_timer.h"
#include "base/fail.h"
#include "base/now.h"
#include "base/program_options_archive.h"
#include "base/telemetry_registry.h"

#include "ripple.h"
#include "servo_interface.h"

namespace mjmech {
namespace mech {
namespace {
struct Parameters {
  /// Update the gait engine (and send servo commands) at this rate.
  double period_s = 0.05; // 20Hz

  /// This long with no commands will result in stopping the gait
  /// engine and setting all servos to unpowered.
  double command_timeout_s = 0.0;

  /// The maximum amount that the gait engine can accelerate in each
  /// axis.  Deceleration is currently unlimited.
  base::Point3D max_acceleration_mm_s2 = base::Point3D(200., 200., 200.);

  template <typename Archive>
  void Serialize(Archive* a) {
    a->Visit(MJ_NVP(period_s));
    a->Visit(MJ_NVP(command_timeout_s));
    a->Visit(MJ_NVP(max_acceleration_mm_s2));
  }
};

enum State : int {
  kUnpowered,
      kActive,
      };

static std::map<State, const char*> StateMapper() {
  return std::map<State, const char*>{
    { kUnpowered, "kUnpowered" },
    { kActive, "kActive" },
        };
}

struct GaitData {
  boost::posix_time::ptime timestamp;

  State state;
  base::Transform body_robot;
  base::Transform cog_robot;
  base::Transform body_world;
  base::Transform robot_world;

  base::Quaternion attitude;
  base::Point3D body_rate_dps;

  std::array<base::Point3D, 4> legs;
  // The command as sent by the user.
  JointCommand command;

  // The command as given to the gait engine.
  Command input_command;
  Command gait_command;

  template <typename Archive>
  void Serialize(Archive* a) {
    a->Visit(MJ_NVP(timestamp));
    a->Visit(MJ_ENUM(state, StateMapper));
    a->Visit(MJ_NVP(body_robot));
    a->Visit(MJ_NVP(cog_robot));
    a->Visit(MJ_NVP(body_world));
    a->Visit(MJ_NVP(robot_world));
    a->Visit(MJ_NVP(attitude));
    a->Visit(MJ_NVP(body_rate_dps));
    a->Visit(MJ_NVP(legs));
    a->Visit(MJ_NVP(command));
    a->Visit(MJ_NVP(input_command));
    a->Visit(MJ_NVP(gait_command));
  }
};

struct CommandData {
  boost::posix_time::ptime timestamp;

  Command command;

  template <typename Archive>
  void Serialize(Archive* a) {
    a->Visit(MJ_NVP(timestamp));
    a->Visit(MJ_NVP(command));
  }
};

double Limit(double val, double max) {
  if (val > max) { return max; }
  else if (val < -max) { return -max; }
  return val;
}

class FailHandler {
 public:
  void operator()(base::ErrorCode ec) const {
    FailIf(ec);
  }
};
}

class GaitDriver::Impl : boost::noncopyable {
 public:
  Impl(boost::asio::io_service& service,
       base::TelemetryRegistry* telemetry_registry,
       ServoInterface* servo)
      : service_(service),
        servo_(servo),
        timer_(service) {
    base::ProgramOptionsArchive(&options_).Accept(&parameters_);

    telemetry_registry->Register("gait", &gait_data_signal_);
    telemetry_registry->Register("gait_command", &command_data_signal_);
  }

  void SetGait(std::unique_ptr<RippleGait> gait) {
    gait_ = std::move(gait);
  }

  void SetCommand(const Command& input_command) {
    input_command_ = input_command;

    const double old_translate_x_mm_s = gait_command_.translate_x_mm_s;
    const double old_translate_y_mm_s = gait_command_.translate_y_mm_s;
    gait_command_ = input_command;
    gait_command_.translate_x_mm_s = old_translate_x_mm_s;
    gait_command_.translate_y_mm_s = old_translate_y_mm_s;

    CommandData data;
    data.timestamp = base::Now(service_);
    data.command = input_command_;
    command_data_signal_(&data);

    last_command_timestamp_ = data.timestamp;

    if (state_ != kActive) {
      servo_->EnablePower(ServoInterface::kPowerEnable, {}, FailHandler());
    }
    state_ = kActive;

    // If our timer isn't started yet, then start it now.
    if (!timer_started_) {
      timer_started_ = true;
      timer_.expires_from_now(base::ConvertSecondsToDuration(0.0));
      StartTimer();
    }

    gait_->SetCommand(gait_command_);
  }

  void SetFree() {
    servo_->EnablePower(ServoInterface::kPowerBrake, {}, FailHandler());
    state_ = kUnpowered;
    timer_.cancel();
    timer_started_ = false;

    Emit();
  }

  void ProcessBodyAhrs(boost::posix_time::ptime timestamp,
                       bool valid,
                       const base::Quaternion& world_attitude,
                       const base::Point3D& body_rate_dps) {
    if (!valid) {
      attitude_ = base::Quaternion();
      body_rate_dps_ = base::Point3D();
      return;
    }

    attitude_ = world_attitude;
    body_rate_dps_ = body_rate_dps;
  }

  boost::program_options::options_description* options() {
    return &options_;
  }

 private:
  void StartTimer() {
    timer_.expires_at(
        timer_.expires_at() +
        base::ConvertSecondsToDuration(parameters_.period_s));
    timer_.async_wait(std::bind(&Impl::HandleTimer, this,
                                std::placeholders::_1));
  }

  void HandleTimer(const boost::system::error_code& ec) {
    if (ec == boost::asio::error::operation_aborted) { return; }

    const auto now = base::Now(service_);
    const auto elapsed = now - last_command_timestamp_;
    const bool timeout =
        (parameters_.command_timeout_s > 0.0) &&
        (elapsed > base::ConvertSecondsToDuration(
            parameters_.command_timeout_s));
    if (state_ == kUnpowered || timeout) {
      SetFree();
      return;
    }

    StartTimer();

    auto update_axis_accel = [&](double input_mm_s,
                                 double* gait_mm_s,
                                 double accel_mm_s2) {
      // Update the translational velocities.
      if ((input_mm_s * (*gait_mm_s)) >= 0.0) {
        // Same sign or one is zero.
        if (std::abs(input_mm_s) > std::abs(*gait_mm_s)) {
          double delta_mm_s = input_mm_s - *gait_mm_s;
          const double max_step_mm_s =
            parameters_.period_s * accel_mm_s2;
          const double step_mm_s = Limit(delta_mm_s, max_step_mm_s);
          *gait_mm_s += step_mm_s;
        } else {
          *gait_mm_s = input_mm_s;
        }
      } else {
        // Opposite signs.
        *gait_mm_s = 0;
      }
    };

    update_axis_accel(input_command_.translate_x_mm_s,
                      &gait_command_.translate_x_mm_s,
                      parameters_.max_acceleration_mm_s2.x);
    update_axis_accel(input_command_.translate_y_mm_s,
                      &gait_command_.translate_y_mm_s,
                      parameters_.max_acceleration_mm_s2.y);

    // Advance our gait, then send the requisite servo commands out.
    gait_commands_ = gait_->AdvanceTime(parameters_.period_s);

    std::vector<ServoInterface::Joint> servo_commands;
    for (const auto& joint: gait_commands_.joints) {
      servo_commands.emplace_back(
          ServoInterface::Joint{joint.servo_number, joint.angle_deg});
    }

    servo_->SetPose(servo_commands, FailHandler());

    Emit();
  }

  void Emit() {
    const auto& state = gait_->state();

    GaitData data;
    data.timestamp = base::Now(service_);

    data.state = state_;
    data.command = gait_commands_;
    data.body_robot = state.body_frame.TransformToFrame(&state.robot_frame);
    data.cog_robot = state.cog_frame.TransformToFrame(&state.robot_frame);
    data.body_world = state.body_frame.TransformToFrame(&state.world_frame);
    data.robot_world = state.robot_frame.TransformToFrame(&state.world_frame);
    data.attitude = attitude_;
    data.body_rate_dps = body_rate_dps_;
    BOOST_ASSERT(state.legs.size() == 4);
    for (size_t i = 0; i < state.legs.size(); i++) {
      data.legs[i] = state.robot_frame.MapFromFrame(
          state.legs[i].frame, state.legs[i].point);
    }
    data.input_command = input_command_;
    data.gait_command = gait_command_;
    gait_data_signal_(&data);
  }

  boost::asio::io_service& service_;
  std::unique_ptr<RippleGait> gait_;
  ServoInterface* const servo_;

  boost::program_options::options_description options_;

  Parameters parameters_;

  boost::signals2::signal<void (const GaitData*)> gait_data_signal_;
  boost::signals2::signal<void (const CommandData*)> command_data_signal_;

  base::DeadlineTimer timer_;
  bool timer_started_ = false;
  State state_ = kUnpowered;
  boost::posix_time::ptime last_command_timestamp_;
  base::Quaternion attitude_;
  base::Point3D body_rate_dps_;
  JointCommand gait_commands_;

  Command input_command_;
  Command gait_command_;
};

GaitDriver::GaitDriver(boost::asio::io_service& service,
                       base::TelemetryRegistry* registry,
                       ServoInterface* servo)
    : impl_(new Impl(service, registry, servo)) {}

GaitDriver::~GaitDriver() {}

void GaitDriver::SetGait(std::unique_ptr<RippleGait> gait) {
  impl_->SetGait(std::move(gait));
}

void GaitDriver::SetCommand(const Command& command) {
  impl_->SetCommand(command);
}

void GaitDriver::SetFree() {
  impl_->SetFree();
}

boost::program_options::options_description*
GaitDriver::options() { return impl_->options(); }

void GaitDriver::ProcessBodyAhrs(boost::posix_time::ptime timestamp,
                                 bool valid,
                                 const base::Quaternion& attitude,
                                 const base::Point3D& body_rate_dps) {
  impl_->ProcessBodyAhrs(timestamp, valid, attitude, body_rate_dps);
}

}
}
