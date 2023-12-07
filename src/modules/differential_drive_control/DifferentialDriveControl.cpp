/****************************************************************************
 *
 *   Copyright (c) 2023 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

#include "DifferentialDriveControl.hpp"

using namespace time_literals;
using namespace matrix;
namespace differential_drive_control
{

DifferentialDriveControl::DifferentialDriveControl() :
	ModuleParams(nullptr),
	ScheduledWorkItem(MODULE_NAME, px4::wq_configurations::rate_ctrl),
	_differential_guidance_controller(this)
{
	updateParams();
}

bool DifferentialDriveControl::init()
{
	ScheduleOnInterval(10_ms); // 100 Hz
	return true;
}

void DifferentialDriveControl::updateParams()
{
	ModuleParams::updateParams();
	_max_speed = _param_rdd_max_wheel_speed.get() * _param_rdd_wheel_radius.get();
	_max_angular_velocity = _max_speed / (_param_rdd_wheel_base.get() / 2.f);

	_differential_drive_kinematics.setWheelBase(_param_rdd_wheel_base.get());
	_differential_drive_kinematics.setMaxSpeed(_max_speed);
	_differential_drive_kinematics.setMaxAngularVelocity(_max_angular_velocity);
}

void DifferentialDriveControl::Run()
{
	if (should_exit()) {
		ScheduleClear();
		exit_and_cleanup();
	}

	hrt_abstime now = hrt_absolute_time();

	float dt  = math::min((now - _time_stamp_last), _timeout) / 1e6f;
	_time_stamp_last = now;

	if (_parameter_update_sub.updated()) {
		parameter_update_s pupdate;
		_parameter_update_sub.copy(&pupdate);

		updateParams();
	}

	if (_vehicle_control_mode_sub.updated()) {
		vehicle_control_mode_s vehicle_control_mode;

		if (_vehicle_control_mode_sub.copy(&vehicle_control_mode)) {
			_armed = vehicle_control_mode.flag_armed;
			_manual_driving = vehicle_control_mode.flag_control_manual_enabled; // change this when more modes are supported
			_mission_driving = vehicle_control_mode.flag_control_auto_enabled;
		}
	}

	if (_pos_sp_triplet_sub.updated()) {
		_pos_sp_triplet_sub.copy(&_pos_sp_triplet);
	}

	if (_vehicle_attitude_sub.updated()) {
		_vehicle_attitude_sub.copy(&_vehicle_attitude);
	}

	if (_global_pos_sub.updated()) {
		_global_pos_sub.copy(&_global_pos);
	}

	if (_vehicle_angular_velocity_sub.updated()) {
		_vehicle_angular_velocity_sub.copy(&_vehicle_angular_velocity);
	}

	if (_vehicle_local_position_sub.updated()) {
		_vehicle_local_position_sub.copy(&_vehicle_local_position);
	}

	if (_manual_driving) {
		// Manual mode
		// directly produce setpoints from the manual control setpoint (joystick)
		if (_manual_control_setpoint_sub.updated()) {
			manual_control_setpoint_s manual_control_setpoint{};

			if (_manual_control_setpoint_sub.copy(&manual_control_setpoint)) {
				_differential_drive_setpoint.timestamp = now;
				_differential_drive_setpoint.speed = manual_control_setpoint.throttle * _param_rdd_speed_scale.get() * _max_speed;
				_differential_drive_setpoint.yaw_rate = manual_control_setpoint.roll * _param_rdd_ang_velocity_scale.get() *
									_max_angular_velocity;
				_differential_drive_setpoint_pub.publish(_differential_drive_setpoint);
			}
		}
	}

	if (_mission_driving && _armed) {
		// Mission mode
		// directly receive setpoints from the guidance library
		matrix::Vector2f global_position(_global_pos.lat, _global_pos.lon);
		matrix::Vector2f current_waypoint(_pos_sp_triplet.current.lat, _pos_sp_triplet.current.lon);
		matrix::Vector2f next_waypoint(_pos_sp_triplet.next.lat, _pos_sp_triplet.next.lon);

		// edge case when system is initialized and there is no previous waypoint
		if (!PX4_ISFINITE(_pos_sp_triplet.previous.lat) && !_first_waypoint_intialized) {
			_previous_waypoint(0) = global_position(0);
			_previous_waypoint(1) = global_position(1);
			_first_waypoint_intialized = true;
			printf("yeehaw\n");

		} else if (PX4_ISFINITE(_pos_sp_triplet.previous.lat)) {
			_previous_waypoint(0) = _pos_sp_triplet.previous.lat;
			_previous_waypoint(1) = _pos_sp_triplet.previous.lon;
		}

		const float vehicle_yaw = matrix::Eulerf(matrix::Quatf(_vehicle_attitude.q)).psi();

		// printf("global position: %f, %f\n", (double)global_position(0), (double)global_position(1));
		// printf("current waypoint: %f, %f\n", (double)current_waypoint(0), (double)current_waypoint(1));
		// printf("previous waypoint: %f, %f\n", (double)_previous_waypoint(0), (double)_previous_waypoint(1));
		// printf("next waypoint: %f, %f\n", (double)next_waypoint(0), (double)next_waypoint(1));

		float body_angular_velocity = _vehicle_angular_velocity.xyz[2];

		matrix::Vector3f ground_speed(_vehicle_local_position.vx, _vehicle_local_position.vy,  _vehicle_local_position.vz);
		matrix::Vector2f ground_speed_2d(ground_speed);
		// Velocity in body frame
		const Dcmf R_to_body(Quatf(_vehicle_attitude.q).inversed());
		const Vector3f vel = R_to_body * Vector3f(ground_speed(0), ground_speed(1), ground_speed(2));

		const float x_vel = vel(0);

		matrix::Vector2f guidance_output =
			_differential_guidance_controller.computeGuidance(
				global_position,
				current_waypoint,
				_previous_waypoint,
				next_waypoint,
				vehicle_yaw,
				x_vel,
				body_angular_velocity,
				dt
			);

		// printf("guidance_output: %f, %f\n", (double)guidance_output(0), (double)guidance_output(1));
		printf(" \n");


		_differential_drive_setpoint.timestamp = now;
		_differential_drive_setpoint.speed = guidance_output(0);
		_differential_drive_setpoint.yaw_rate = guidance_output(1);
		_differential_drive_setpoint_pub.publish(_differential_drive_setpoint);

	}

	_differential_drive_setpoint_sub.update(&_differential_drive_setpoint);

	// publish data to actuator_motors (output module)
	// get the wheel speeds from the inverse kinematics class (DifferentialDriveKinematics)
	Vector2f wheel_speeds = _differential_drive_kinematics.computeInverseKinematics(
					_differential_drive_setpoint.speed,
					_differential_drive_setpoint.yaw_rate);

	// Check if max_angular_wheel_speed is zero
	const bool setpoint_timeout = (_differential_drive_setpoint.timestamp + 100_ms) < now;
	const bool valid_max_speed = _param_rdd_speed_scale.get() > FLT_EPSILON;

	if (!_armed || setpoint_timeout || !valid_max_speed) {
		wheel_speeds = {}; // stop
	}

	wheel_speeds = matrix::constrain(wheel_speeds, -1.f, 1.f);

	actuator_motors_s actuator_motors{};
	actuator_motors.reversible_flags = _param_r_rev.get(); // should be 3 see rc.rover_differential_defaults
	wheel_speeds.copyTo(actuator_motors.control);
	actuator_motors.timestamp = now;
	_actuator_motors_pub.publish(actuator_motors);
}

int DifferentialDriveControl::task_spawn(int argc, char *argv[])
{
	DifferentialDriveControl *instance = new DifferentialDriveControl();

	if (instance) {
		_object.store(instance);
		_task_id = task_id_is_work_queue;

		if (instance->init()) {
			return PX4_OK;
		}

	} else {
		PX4_ERR("alloc failed");
	}

	delete instance;
	_object.store(nullptr);
	_task_id = -1;

	return PX4_ERROR;
}

int DifferentialDriveControl::custom_command(int argc, char *argv[])
{
	return print_usage("unknown command");
}

int DifferentialDriveControl::print_usage(const char *reason)
{
	if (reason) {
		PX4_ERR("%s\n", reason);
	}

	PRINT_MODULE_DESCRIPTION(
		R"DESCR_STR(
### Description
Rover Differential Drive controller.
)DESCR_STR");

	PRINT_MODULE_USAGE_NAME("differential_drive_control", "controller");
	PRINT_MODULE_USAGE_COMMAND("start");
	PRINT_MODULE_USAGE_DEFAULT_COMMANDS();
	return 0;
}

extern "C" __EXPORT int differential_drive_control_main(int argc, char *argv[])
{
	return DifferentialDriveControl::main(argc, argv);
}

} // namespace differential_drive_control
