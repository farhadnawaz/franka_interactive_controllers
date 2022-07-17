// This code was derived from franka_example controllers
// Copyright (c) 2017 Franka Emika GmbH
// Use of this source code is governed by the Apache-2.0 license.
// Current development and modification of this code by Nadia Figueroa (MIT) 2021.

#include <cartesian_twist_impedance_controller.h>

#include <cmath>
#include <memory>

#include <controller_interface/controller_base.h>
#include <franka/robot_state.h>
#include <pluginlib/class_list_macros.h>
#include <ros/ros.h>

#include <pseudo_inversion.h>
#include <hardware_interface/joint_command_interface.h>

namespace franka_interactive_controllers {

bool CartesianTwistImpedanceController::init(hardware_interface::RobotHW* robot_hw,
                                               ros::NodeHandle& node_handle) {
  std::vector<double> cartesian_stiffness_vector;
  std::vector<double> cartesian_damping_vector;

  // sub_desired_pose_ = node_handle.subscribe(
  //     "/cartesian_impedance_controller/desired_pose", 20, &CartesianTwistImpedanceController::desiredPoseCallback, this,
  //     ros::TransportHints().reliable().tcpNoDelay());

  sub_desired_twist_ = node_handle.subscribe(
      "/cartesian_impedance_controller/desired_twist", 20, &CartesianTwistImpedanceController::desiredTwistCallback, this,
      ros::TransportHints().reliable().tcpNoDelay());

  // Getting ROSParams
  std::string arm_id;
  if (!node_handle.getParam("arm_id", arm_id)) {
    ROS_ERROR_STREAM("CartesianTwistImpedanceController: Could not read parameter arm_id");
    return false;
  }
  std::vector<std::string> joint_names;
  if (!node_handle.getParam("joint_names", joint_names) || joint_names.size() != 7) {
    ROS_ERROR(
        "CartesianTwistImpedanceController: Invalid or no joint_names parameters provided, "
        "aborting controller init!");
    return false;
  }

  // Initialize variables for tool compensation from yaml config file
  activate_tool_compensation_ = true;
  tool_compensation_force_.setZero();
  std::vector<double> external_tool_compensation;
  // tool_compensation_force_ << 0.46, -0.17, -1.64, 0, 0, 0;  //read from yaml
  if (!node_handle.getParam("external_tool_compensation", external_tool_compensation) || external_tool_compensation.size() != 6) {
      ROS_ERROR(
          "CartesianTwistImpedanceController: Invalid or no external_tool_compensation parameters provided, "
          "aborting controller init!");
      return false;
    }
  for (size_t i = 0; i < 6; ++i) 
    tool_compensation_force_[i] = external_tool_compensation.at(i);
  ROS_INFO_STREAM("External tool compensation force: " << std::endl << tool_compensation_force_);

  // Initialize variables for nullspace control from yaml config file
  q_d_nullspace_.setZero();
  std::vector<double> q_nullspace;
  if (node_handle.getParam("q_nullspace", q_nullspace)) {
    q_d_nullspace_initialized_ = true;
    if (q_nullspace.size() != 7) {
      ROS_ERROR(
        "CartesianPoseImpedanceController: Invalid or no q_nullspace parameters provided, "
        "aborting controller init!");
      return false;
    }
    for (size_t i = 0; i < 7; ++i) 
      q_d_nullspace_[i] = q_nullspace.at(i);
    ROS_INFO_STREAM("Desired nullspace position (from YAML): " << std::endl << q_d_nullspace_);
  }


  if (!node_handle.getParam("nullspace_stiffness", nullspace_stiffness_target_) || nullspace_stiffness_target_ <= 0) {
    ROS_ERROR(
      "CartesianPoseImpedanceController: Invalid or no nullspace_stiffness parameters provided, "
      "aborting controller init!");
    return false;
  }
  ROS_INFO_STREAM("nullspace_stiffness_target_: " << std::endl <<  nullspace_stiffness_target_);



  // Getting libranka control interfaces
  auto* model_interface = robot_hw->get<franka_hw::FrankaModelInterface>();
  if (model_interface == nullptr) {
    ROS_ERROR_STREAM(
        "CartesianTwistImpedanceController: Error getting model interface from hardware");
    return false;
  }
  try {
    model_handle_ = std::make_unique<franka_hw::FrankaModelHandle>(
        model_interface->getHandle(arm_id + "_model"));
  } catch (hardware_interface::HardwareInterfaceException& ex) {
    ROS_ERROR_STREAM(
        "CartesianTwistImpedanceController: Exception getting model handle from interface: "
        << ex.what());
    return false;
  }

  auto* state_interface = robot_hw->get<franka_hw::FrankaStateInterface>();
  if (state_interface == nullptr) {
    ROS_ERROR_STREAM(
        "CartesianTwistImpedanceController: Error getting state interface from hardware");
    return false;
  }
  try {
    state_handle_ = std::make_unique<franka_hw::FrankaStateHandle>(
        state_interface->getHandle(arm_id + "_robot"));
  } catch (hardware_interface::HardwareInterfaceException& ex) {
    ROS_ERROR_STREAM(
        "CartesianTwistImpedanceController: Exception getting state handle from interface: "
        << ex.what());
    return false;
  }

  auto* effort_joint_interface = robot_hw->get<hardware_interface::EffortJointInterface>();
  if (effort_joint_interface == nullptr) {
    ROS_ERROR_STREAM(
        "CartesianTwistImpedanceController: Error getting effort joint interface from hardware");
    return false;
  }
  for (size_t i = 0; i < 7; ++i) {
    try {
      joint_handles_.push_back(effort_joint_interface->getHandle(joint_names[i]));
    } catch (const hardware_interface::HardwareInterfaceException& ex) {
      ROS_ERROR_STREAM(
          "CartesianTwistImpedanceController: Exception getting joint handles: " << ex.what());
      return false;
    }
  }

  // Getting Dynamic Reconfigure objects
  dynamic_reconfigure_compliance_param_node_ =
      ros::NodeHandle(node_handle.getNamespace() + "dynamic_reconfigure_compliance_param_node");

  dynamic_server_compliance_param_ = std::make_unique<
      dynamic_reconfigure::Server<franka_interactive_controllers::compliance_paramConfig>>(

      dynamic_reconfigure_compliance_param_node_);
  dynamic_server_compliance_param_->setCallback(
      boost::bind(&CartesianTwistImpedanceController::complianceParamCallback, this, _1, _2));


  // Initializing variables
  position_d_.setZero();
  orientation_d_.coeffs() << 0.0, 0.0, 0.0, 1.0;
  position_d_target_.setZero();
  orientation_d_target_.coeffs() << 0.0, 0.0, 0.0, 1.0;

  velocity_d_.setZero();

  cartesian_stiffness_.setZero();
  cartesian_damping_.setZero();

  // // Parameters for goto_home at initialization!!
  // _goto_home = false;

  // // Parameters for jointDS controller (THIS SHOULD BE IN ANOTHER SCRIPT!! DS MOTION GENERATOR?)
  // q_home_ << 0, -M_PI_4, 0, -3 * M_PI_4, 0, M_PI_2, M_PI_4;
  // jointDS_epsilon_  = 0.05;
  // dq_filter_params_ = 0.555;

  // A_jointDS_home_ = Eigen::MatrixXd::Identity(7, 7);
  // A_jointDS_home_(0,0) = 10; A_jointDS_home_(1,1) = 10; A_jointDS_home_(2,2) = 10;
  // A_jointDS_home_(3,3) = 10; A_jointDS_home_(4,4) = 15; A_jointDS_home_(5,5) = 15;
  // A_jointDS_home_(6,6) = 15;
  // ROS_INFO_STREAM("A (jointDS): " << std::endl <<  A_jointDS_home_);

  // // Parameters for joint PD controller
  // // Ideal gains for Joint Impedance Controller
  // // k_gains: 600.0, 600.0, 600.0, 600.0, 250.0, 150.0, 50.0
  // // d_gains: 50.0, 50.0, 50.0, 20.0, 20.0, 20.0, 10.0

  // // Gains for P error stiffness term
  // k_joint_gains_ = Eigen::MatrixXd::Identity(7, 7);
  // k_joint_gains_(0,0) = 500; k_joint_gains_(1,1) = 500; k_joint_gains_(2,2) = 500;
  // k_joint_gains_(3,3) = 500; k_joint_gains_(4,4) = 500; k_joint_gains_(5,5) = 500;
  // k_joint_gains_(6,6) = 200;
  // ROS_INFO_STREAM("K (joint stiffness): " << std::endl <<  k_joint_gains_);

  // // Gains for D error damping term
  // d_joint_gains_ = Eigen::MatrixXd::Identity(7, 7);
  // ROS_INFO_STREAM("D (joint damping): " << std::endl << d_joint_gains_);
  // d_joint_gains_(0,0) = 5; d_joint_gains_(1,1) = 5; d_joint_gains_(2,2) = 5;
  // d_joint_gains_(3,3) = 2; d_joint_gains_(4,4) = 2; d_joint_gains_(5,5) = 2;
  // d_joint_gains_(6,6) = 1;

  // // Gains for feed-forward damping term
  // d_ff_joint_gains_ = Eigen::MatrixXd::Identity(7, 7);


  return true;
}

void CartesianTwistImpedanceController::starting(const ros::Time& /*time*/) {

  // Get robot current/initial joint state
  franka::RobotState initial_state = state_handle_->getRobotState();
  Eigen::Map<Eigen::Matrix<double, 7, 1>> q_initial(initial_state.q.data());

  // get jacobian
  std::array<double, 42> jacobian_array =
      model_handle_->getZeroJacobian(franka::Frame::kEndEffector);
  // convert to eigen
  Eigen::Affine3d initial_transform(Eigen::Matrix4d::Map(initial_state.O_T_EE.data()));

  // set desired point to current state
  position_d_           = initial_transform.translation();
  orientation_d_        = Eigen::Quaterniond(initial_transform.linear());
  position_d_target_    = initial_transform.translation();
  orientation_d_target_ = Eigen::Quaterniond(initial_transform.linear());

  if (!q_d_nullspace_initialized_) {
    q_d_nullspace_ = q_initial;
    q_d_nullspace_initialized_ = true;
    ROS_INFO_STREAM("Desired nullspace position (from q_initial): " << std::endl << q_d_nullspace_);
  }

  // To compute 0 velocities if no command has been given
  elapsed_time    = ros::Duration(0.0);
  last_cmd_time   = 0.0;
  vel_cmd_timeout = 0.1;

}

void CartesianTwistImpedanceController::update(const ros::Time& /*time*/,
                                                 const ros::Duration& period) {
  // get state variables
  franka::RobotState robot_state = state_handle_->getRobotState();
  std::array<double, 7> coriolis_array = model_handle_->getCoriolis();
  std::array<double, 42> jacobian_array =
      model_handle_->getZeroJacobian(franka::Frame::kEndEffector);
  std::array<double, 7> gravity_array = model_handle_->getGravity();

  // convert to Eigen
  Eigen::Map<Eigen::Matrix<double, 7, 1>> coriolis(coriolis_array.data());
  Eigen::Map<Eigen::Matrix<double, 7, 1>> gravity(gravity_array.data());
  Eigen::Map<Eigen::Matrix<double, 6, 7>> jacobian(jacobian_array.data());
  Eigen::Map<Eigen::Matrix<double, 7, 1>> q(robot_state.q.data());
  Eigen::Map<Eigen::Matrix<double, 7, 1>> dq(robot_state.dq.data());
  Eigen::Map<Eigen::Matrix<double, 7, 1>> tau_J_d(  // NOLINT (readability-identifier-naming)
      robot_state.tau_J_d.data());
  Eigen::Affine3d transform(Eigen::Matrix4d::Map(robot_state.O_T_EE.data()));
  Eigen::Vector3d position(transform.translation());
  Eigen::Quaterniond orientation(transform.linear());

  // Current and Desired EE velocity
  Eigen::Matrix<double, 6, 1> velocity;
  Eigen::Matrix<double, 6, 1> velocity_desired_;
  velocity << jacobian * dq;
  velocity_desired_.setZero();
  velocity_desired_.head(3) << velocity_d_;

  // Check velocity command
  elapsed_time += period;
  if(ros::Time::now().toSec() - last_cmd_time > vel_cmd_timeout){
    velocity_d_.setZero();
    // ROS_WARN_STREAM_THROTTLE(0.5,"Command Timeout!");
  }

  //////////////////////////////////////////////////////////////////////////////////////////////////
  //////////////////////              COMPUTING TASK CONTROL TORQUE           //////////////////////
  //////////////////////////////////////////////////////////////////////////////////////////////////

  // compute control
  // allocate variables
  Eigen::VectorXd tau_task(7), tau_nullspace(7), tau_d(7), tau_tool(7);

  //+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++//
  //++++++++++++++ CLASSICAL IMPEDANCE CONTROL FOR CARTESIAN COMMAND ++++++++++++++//
  //+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++//

  // Compute task-space errors
  Eigen::Matrix<double, 6, 1> pose_error;
  pose_error.setZero();

  // --- Pose Error  --- //     
  position_d_        << position + velocity_d_*dt_*150; //Scaling to make it faster! (200 goes way faster than the desired)
  pose_error.head(3) << position - position_d_;

  // orientation error
  if (orientation_d_.coeffs().dot(orientation.coeffs()) < 0.0) {
    orientation.coeffs() << -orientation.coeffs();
  }
  // "difference" quaternion
  Eigen::Quaterniond error_quaternion(orientation.inverse() * orientation_d_);
  pose_error.tail(3) << error_quaternion.x(), error_quaternion.y(), error_quaternion.z();
  // Transform to base frame
  pose_error.tail(3) << -transform.linear() * pose_error.tail(3);

  // Computing control torque from cartesian pose error from integrated velocity command
  Eigen::VectorXd     F_ee_des_;
  F_ee_des_.resize(6);
  F_ee_des_ << -cartesian_stiffness_ * pose_error - cartesian_damping_ * velocity;
  tau_task << jacobian.transpose() * F_ee_des_;

  ROS_WARN_STREAM_THROTTLE(0.5, "Desired Velocity Norm:" << velocity_d_.norm());
  ROS_WARN_STREAM_THROTTLE(0.5, "Current Velocity Norm:" << velocity.head(3).norm());
  ROS_WARN_STREAM_THROTTLE(0.5, "Classic Linear Control Force:" << F_ee_des_.head(3).norm());
  ROS_WARN_STREAM_THROTTLE(0.5, "Classic Angular Control Force :" << F_ee_des_.tail(3).norm());
  //+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++//
  //+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++//

  //++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++//
  //++++++++++++++ ADDITIONAL CONTROL TORQUES (NULLSPACE AND TOOL COMPENSATION) ++++++++++++++//
  //++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++//


  // pseudoinverse for nullspace handling
  // kinematic pseudoinverse
  Eigen::MatrixXd jacobian_transpose_pinv;
  pseudoInverse(jacobian.transpose(), jacobian_transpose_pinv);

  // nullspace PD control with damping ratio = 1
  tau_nullspace << (Eigen::MatrixXd::Identity(7, 7) -
                    jacobian.transpose() * jacobian_transpose_pinv) *
                       (nullspace_stiffness_ * (q_d_nullspace_ - q) -
                        (2.0 * sqrt(nullspace_stiffness_)) * dq);
  double tau_nullspace_0 = tau_nullspace(0);
  tau_nullspace.setZero();
  tau_nullspace[0] = tau_nullspace_0;                     
                       
  // Compute tool compensation (scoop/camera in scooping task)
  if (activate_tool_compensation_)
    tau_tool << jacobian.transpose() * tool_compensation_force_;
  else
    tau_tool.setZero();

  // FINAL DESIRED CONTROL TORQUE SENT TO ROBOT
  tau_d << tau_task + tau_nullspace + coriolis - tau_tool;

  // Saturate torque rate to avoid discontinuities
  tau_d << saturateTorqueRate(tau_d, tau_J_d);

  for (size_t i = 0; i < 7; ++i) {
    joint_handles_[i].setCommand(tau_d(i));
  }

  //////////////////////////////////////////////////////////////////////////////////////////////////
  //////////////////////////////////////////////////////////////////////////////////////////////////

  // update parameters changed online either through dynamic reconfigure or through the interactive
  // target by filtering
  cartesian_stiffness_ =
      filter_params_ * cartesian_stiffness_target_ + (1.0 - filter_params_) * cartesian_stiffness_;
  cartesian_damping_ =
      filter_params_ * cartesian_damping_target_ + (1.0 - filter_params_) * cartesian_damping_;
  nullspace_stiffness_ =
      filter_params_ * nullspace_stiffness_target_ + (1.0 - filter_params_) * nullspace_stiffness_;
  
  // position_d_ = filter_params_ * position_d_target_ + (1.0 - filter_params_) * position_d_;
  // position_d_ = position_d_target_;
  orientation_d_ = orientation_d_.slerp(filter_params_, orientation_d_target_);
}

Eigen::Matrix<double, 7, 1> CartesianTwistImpedanceController::saturateTorqueRate(
    const Eigen::Matrix<double, 7, 1>& tau_d_calculated,
    const Eigen::Matrix<double, 7, 1>& tau_J_d) {  // NOLINT (readability-identifier-naming)
  Eigen::Matrix<double, 7, 1> tau_d_saturated{};
  for (size_t i = 0; i < 7; i++) {
    double difference = tau_d_calculated[i] - tau_J_d[i];
    tau_d_saturated[i] =
        tau_J_d[i] + std::max(std::min(difference, delta_tau_max_), -delta_tau_max_);
  }
  return tau_d_saturated;
}

void CartesianTwistImpedanceController::complianceParamCallback(
    franka_interactive_controllers::compliance_paramConfig& config,
    uint32_t /*level*/) {
  cartesian_stiffness_target_.setIdentity();
  cartesian_stiffness_target_.topLeftCorner(3, 3)
      << config.translational_stiffness * Eigen::Matrix3d::Identity();
  cartesian_stiffness_target_.bottomRightCorner(3, 3)
      << config.rotational_stiffness * Eigen::Matrix3d::Identity();
  

  cartesian_damping_target_.setIdentity();
  // Damping ratio = 1
  cartesian_damping_target_.topLeftCorner(3, 3)
      << 2.0 * sqrt(config.translational_stiffness) * Eigen::Matrix3d::Identity();
  cartesian_damping_target_.bottomRightCorner(3, 3)
      << 2.0 * sqrt(config.rotational_stiffness) * Eigen::Matrix3d::Identity();
  
  nullspace_stiffness_target_ = config.nullspace_stiffness;

  activate_tool_compensation_ = config.activate_tool_compensation;
}


void CartesianTwistImpedanceController::desiredTwistCallback(
    const geometry_msgs::TwistConstPtr& msg) {

  
  franka::RobotState robot_state = state_handle_->getRobotState();
  Eigen::Affine3d transform(Eigen::Matrix4d::Map(robot_state.O_T_EE.data()));
  Eigen::Vector3d position(transform.translation());

  velocity_d_      << msg->linear.x, msg->linear.y, msg->linear.z;
  last_cmd_time    = ros::Time::now().toSec();

  // ROS_INFO_STREAM("[CALLBACK] Desired velocity from DS: " << velocity_d_);
  // ROS_INFO_STREAM("[CALLBACK] Desired ee position from DS: " << position_d_target_);


  // position_d_target_ << msg->pose.position.x, msg->pose.position.y, msg->pose.position.z;
  
  // Eigen::Quaterniond last_orientation_d_target(orientation_d_target_);
  // orientation_d_target_.coeffs() << msg->pose.orientation.x, msg->pose.orientation.y,
  //     msg->pose.orientation.z, msg->pose.orientation.w;
  
  // if (last_orientation_d_target.coeffs().dot(orientation_d_target_.coeffs()) < 0.0) {
  //   orientation_d_target_.coeffs() << -orientation_d_target_.coeffs();
  // }
}

}  // namespace franka_interactive_controllers

PLUGINLIB_EXPORT_CLASS(franka_interactive_controllers::CartesianTwistImpedanceController,
                       controller_interface::ControllerBase)
