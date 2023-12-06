#include "DifferentialDriveGuidance.hpp"

DifferentialDriveGuidance::DifferentialDriveGuidance(ModuleParams *parent) : ModuleParams(parent)
{
	pid_init(&yaw_rate_pid, PID_MODE_DERIVATIV_CALC, 0.001f); // Replace _dt with actual minimum timestep
	pid_set_parameters(&yaw_rate_pid,
			_param_rdc_p_gain_waypoint_controller.get(),  // Proportional gain
			_param_rdc_i_gain_waypoint_controller.get(),  // Integral gain
			_param_rdc_d_gain_waypoint_controller.get(),  // Derivative gain
			200,  // Integral limit
			200);  // Output limit
}


matrix::Vector2f DifferentialDriveGuidance::computeGuidance(const matrix::Vector2f &current_pos,
		const matrix::Vector2f &waypoint, const matrix::Vector2f &previous_waypoint, const matrix::Vector2f &next_waypoint,
		float vehicle_yaw, float dt)
{
	_global_position = current_pos;
	_current_waypoint = waypoint;
	_previous_waypoint = previous_waypoint;
	_next_waypoint = next_waypoint;
	_dt = dt;
	float max_forwards_velocity = _param_rdd_max_speed.get();
	// float max_angular_velocity = _param_rdd_max_angular_velocity.get();

	float desired_heading = computeAdvancedBearing(_global_position, _current_waypoint, _previous_waypoint);

	float distance_to_next_wp = get_distance_to_next_waypoint(_global_position(0), _global_position(1),
				    _current_waypoint(0), _current_waypoint(1));

	float heading_error = normalizeAngle(desired_heading - vehicle_yaw);

	float align_error = computeAlignment(_global_position, _current_waypoint, _previous_waypoint);

	const float desired_angular_rate =
		pid_calculate(&yaw_rate_pid,
                         0,  // Setpoint
                         heading_error,  // Current state
                         0,  // Derivative (ignored in PID_MODE_DERIVATIV_CALC)
                         _dt);  // Time step

	float desired_linear_velocity = max_forwards_velocity;

	// // initialize this at the start of the function and get parameters
	// // not quite sure about this section, subject to change
	// _forwards_velocity_smoothing.setMaxJerk(_param_rdc_max_jerk.get());
	// _forwards_velocity_smoothing.setMaxAccel(_param_rdc_max_acceleration.get());
	// _forwards_velocity_smoothing.setMaxVel(max_forwards_velocity);
	// const float max_velocity = math::trajectory::computeMaxSpeedFromDistance(_param_rdc_max_jerk.get(),
	// 			   _param_rdc_max_acceleration.get(), distance_to_next_wp, _param_rdc_waypoing_min_vel.get());
	// _forwards_velocity_smoothing.updateDurations(max_velocity);
	// _forwards_velocity_smoothing.updateTraj(_dt);

	// desired_linear_velocity = _forwards_velocity_smoothing.getCurrentVelocity();
	desired_linear_velocity = desired_linear_velocity - (1 - abs(align_error)) * desired_linear_velocity *
				  _param_rdc_velocity_alignment_subtraction.get();

	if (!PX4_ISFINITE(desired_linear_velocity) || desired_linear_velocity < 0) {
		desired_linear_velocity = 0;
	}

	matrix::Vector2f output;

	// logic to stop at the last waypoint
	if ((_current_waypoint == _next_waypoint) && distance_to_next_wp < _param_rdc_accepted_waypoint_radius.get()) {

		output(0) = 0;
		output(1) = 0;

	} else {
		output(0) = desired_linear_velocity;
		output(1) = desired_angular_rate;
	}

	return output;
}


float DifferentialDriveGuidance::computeAdvancedBearing(const matrix::Vector2f &current_pos,
		const matrix::Vector2f &waypoint, const matrix::Vector2f &previous_waypoint)
{
	matrix::Vector2f wanted_path = waypoint - previous_waypoint;
	matrix::Vector2f current_path = current_pos - previous_waypoint;

	// Normalize the vectors
	matrix::Vector2f wanted_path_normalized = wanted_path;
	matrix::Vector2f current_path_normalized = current_path;

	wanted_path_normalized.normalize();
	current_path_normalized.normalize();

	float path_dot_product = wanted_path_normalized.dot(current_path_normalized);
	float theta = acos(path_dot_product);

	matrix::Vector2f new_waypoint = waypoint - (current_pos - (wanted_path_normalized * cos(
						theta) * current_path.norm() + previous_waypoint));

	return computeBearing(current_pos, new_waypoint);
}

float DifferentialDriveGuidance::computeBearing(const matrix::Vector2f &current_pos, const matrix::Vector2f &waypoint)
{
	float delta_x = waypoint(0) - current_pos(0);
	float delta_y = waypoint(1) - current_pos(1);
	return std::atan2(delta_y, delta_x);
}

float DifferentialDriveGuidance::normalizeAngle(float angle)
{
	while (angle > M_PIf) { angle -= 2.0f * M_PIf; }

	while (angle < -M_PIf) { angle += 2.0f * M_PIf; }

	return angle;
}

float DifferentialDriveGuidance::computeAlignment(const matrix::Vector2f &current_pos, const matrix::Vector2f &waypoint,
		const matrix::Vector2f &previous_waypoint)
{
	matrix::Vector2f wanted_path = waypoint - previous_waypoint;
	matrix::Vector2f current_path = current_pos - previous_waypoint;

	// Normalize the vectors
	wanted_path.normalize();
	current_path.normalize();

	float result = wanted_path.dot(current_path);

	// Check if result is finite, and return 0 if not
	if (!PX4_ISFINITE(result)) {
		return 0.0f;
	}

	return result;
}
