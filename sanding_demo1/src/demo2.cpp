#include <ros/ros.h>
#include <tesseract_ros/kdl/kdl_env.h>
#include <urdf_parser/urdf_parser.h>

#include <trajopt/problem_description.hpp>
#include <trajectory_msgs/JointTrajectory.h>

#include <actionlib/client/simple_action_client.h>
#include <control_msgs/FollowJointTrajectoryAction.h>

#include <tesseract_msgs/TesseractState.h>
#include <tesseract_ros/ros_tesseract_utils.h>

#include <geometry_msgs/PoseArray.h>

const static double CYL_RADIUS = 0.2;

static bool addObject(tesseract::tesseract_ros::KDLEnv& env)
{
  auto obj = std::make_shared<tesseract::AttachableObject>();

  const static double radius = CYL_RADIUS;
  const static double length = 1.0;
  auto shape = std::make_shared<shapes::Cylinder>(radius, length);

  obj->name = "part";
  obj->visual.shapes.push_back(shape);
  obj->visual.shape_poses.push_back(Eigen::Isometry3d::Identity());
  obj->collision.shapes.push_back(shape);
  obj->collision.shape_poses.push_back(Eigen::Isometry3d::Identity());
  obj->collision.collision_object_types.push_back(tesseract::CollisionObjectType::UseShapeType);

  // This call adds the object to the scene's "database" but does not actuall connect it
  env.addAttachableObject(obj);

  // To include the object in collision checks, you have to attach it
  tesseract::AttachedBodyInfo attached_body;
  attached_body.object_name = "part";
  attached_body.parent_link_name = "world_frame";
  attached_body.transform.setIdentity();
  attached_body.transform.translate(Eigen::Vector3d(1.0, 0, 0.5));

  env.attachBody(attached_body);
  return true;
}

static bool loadEnvironment(tesseract::tesseract_ros::KDLEnvPtr& env)
{
  ros::NodeHandle nh;

  const static std::string ROBOT_DESCRIPTION ("robot_description");
  const static std::string ROBOT_DESCRIPTION_SEMANTIC ("robot_description_semantic");

  std::string urdf_xml, srdf_xml;
  if (!nh.getParam(ROBOT_DESCRIPTION, urdf_xml)) {
    ROS_WARN("robot_description");
    return false;
  }
  if (!nh.getParam(ROBOT_DESCRIPTION_SEMANTIC, srdf_xml)) {
    ROS_WARN("semantic desc");
    return false;
  }

  auto urdf_model = urdf::parseURDF(urdf_xml);
  if (!urdf_model) {
    ROS_WARN("parse urdf");
    return false;
  }

  auto srdf_model = srdf::ModelSharedPtr(new srdf::Model);
  if (!srdf_model->initString(*urdf_model, srdf_xml)) {
    ROS_WARN("parse srdf");
    return false;
  }

  auto env_ptr = std::make_shared<tesseract::tesseract_ros::KDLEnv>();
  if (env_ptr->init(urdf_model, srdf_model))
  {
    env = env_ptr;
    return true;
  }
  else
  {
    ROS_WARN("!env init");
    return false;
  }
}

static bool executeTrajectory(const trajectory_msgs::JointTrajectory& trajectory)
{
  // Create a Follow Joint Trajectory Action Client
  actionlib::SimpleActionClient<control_msgs::FollowJointTrajectoryAction> ac ("joint_trajectory_action", true);
  if (!ac.waitForServer(ros::Duration(2.0)))
  {
    ROS_ERROR("Could not connect to action server");
    return false;
  }

  control_msgs::FollowJointTrajectoryGoal goal;
  goal.trajectory = trajectory;
  goal.goal_time_tolerance = ros::Duration(1.0);

  return ac.sendGoalAndWait(goal) == actionlib::SimpleClientGoalState::SUCCEEDED;
}

static EigenSTL::vector_Isometry3d makePath()
{
  EigenSTL::vector_Isometry3d v;
  Eigen::Isometry3d origin = Eigen::Isometry3d::Identity();
  origin.translation() = Eigen::Vector3d(1.0, 0, 0.5);

  // Create slices of the cylinder
  const static double radius = CYL_RADIUS;
  const static std::size_t n_slices = 5;
  const static double slice_height = 0.1;
  for (std::size_t i = 0; i < n_slices; ++i)
  {
    const double z = i * slice_height;
    const Eigen::Isometry3d slice_center = origin * Eigen::Translation3d(0.0, 0.0, z);
    for (double r = 0.0; r <= 2 * M_PI; r += M_PI / 12.0)
    {
      Eigen::Vector3d offset (radius * std::cos(r), radius * std::sin(r), 0.);
      Eigen::Isometry3d pose = slice_center * Eigen::Translation3d(offset);

      Eigen::Vector3d z_axis = -(pose.translation() - slice_center.translation()).normalized();
      Eigen::Vector3d y_axis = Eigen::Vector3d(-std::sin(r), std::cos(r), 0.0).normalized();
      Eigen::Vector3d x_axis= y_axis.cross(z_axis).normalized();
      std::cout << "z_axis: " << z_axis.transpose() << "\n";
      std::cout << "y_axis: " << y_axis.transpose() << "\n";
      std::cout << "x_axis: " << x_axis.transpose() << "\n";

      pose.matrix().col(2).head<3>() = z_axis;
      pose.matrix().col(1).head<3>() = y_axis;
      pose.matrix().col(0).head<3>() = x_axis;

      v.push_back(pose);
    }
  }

//  const double tilt = -10 * M_PI / 180.;
//  for (auto& p : v)
//  {
//    p = p * Eigen::AngleAxisd(tilt, Eigen::Vector3d::UnitX()) * Eigen::Translation3d(0, 0.05, 0);
//  }


  return v;
}


static trajopt::TrajOptProbPtr makeProblem(tesseract::BasicEnvConstPtr env,
                                           const EigenSTL::vector_Isometry3d& geometric_path)
{
  trajopt::ProblemConstructionInfo pci (env);

  // Populate Basic Info
  pci.basic_info.n_steps = geometric_path.size();
  pci.basic_info.manip = "my_robot";
  pci.basic_info.start_fixed = false;

  // Create Kinematic Object
  pci.kin = pci.env->getManipulator(pci.basic_info.manip);

  const auto dof = pci.kin->numJoints();

  // Populate Init Info
  Eigen::VectorXd start_pos = pci.env->getCurrentJointValues(pci.kin->getName());

  pci.init_info.type = trajopt::InitInfo::STATIONARY;
  pci.init_info.data = start_pos.transpose().replicate(pci.basic_info.n_steps, 1);

  // Populate Cost Info
  std::shared_ptr<trajopt::JointVelCostInfo> jv = std::shared_ptr<trajopt::JointVelCostInfo>(new trajopt::JointVelCostInfo);
  jv->coeffs = std::vector<double>(dof, 2.5);
  jv->name = "joint_vel";
  jv->term_type = trajopt::TT_COST;
  pci.cost_infos.push_back(jv);

  std::shared_ptr<trajopt::JointAccCostInfo> ja = std::shared_ptr<trajopt::JointAccCostInfo>(new trajopt::JointAccCostInfo);
  ja->coeffs = std::vector<double>(dof, 5.0);
  ja->name = "joint_acc";
  ja->term_type = trajopt::TT_COST;
  pci.cost_infos.push_back(ja);

  std::shared_ptr<trajopt::CollisionCostInfo> collision = std::shared_ptr<trajopt::CollisionCostInfo>(new trajopt::CollisionCostInfo);
  collision->name = "collision";
  collision->term_type = trajopt::TT_COST;
  collision->continuous = false;
  collision->first_step = 0;
  collision->last_step = pci.basic_info.n_steps - 1;
  collision->gap = 1;
  collision->info = trajopt::createSafetyMarginDataVector(pci.basic_info.n_steps, 0.025, 20);

  // Apply a special cost between the sander_disks and the part
  for (auto& c : collision->info)
  {
    c->SetPairSafetyMarginData("sander_disk", "part", -0.01, 20.0);
    c->SetPairSafetyMarginData("sander_shaft", "part", 0.0, 20.0);
  }

  pci.cost_infos.push_back(collision);

  auto to_wxyz = [](const Eigen::Isometry3d& p) {
    Eigen::Quaterniond q (p.linear());
    Eigen::Vector4d wxyz;
    wxyz(0) = q.w();
    wxyz(1) = q.x();
    wxyz(2) = q.y();
    wxyz(3) = q.z();
    return wxyz;
  };

  // Populate Constraints
  for (std::size_t i = 0; i < geometric_path.size(); ++i)
  {
    std::shared_ptr<trajopt::StaticPoseCostInfo> pose = std::shared_ptr<trajopt::StaticPoseCostInfo>(new trajopt::StaticPoseCostInfo);
    pose->term_type = trajopt::TT_CNT;
    pose->name = "waypoint_cart_" + std::to_string(i);
    pose->link = "sander_tcp";
    pose->timestep = i;
    pose->xyz = geometric_path[i].translation();
    pose->wxyz = to_wxyz(geometric_path[i]);
    pose->pos_coeffs = Eigen::Vector3d(10, 10, 10);
    pose->rot_coeffs = Eigen::Vector3d(10, 10, 0);
    pci.cnt_infos.push_back(pose);
  }

  return trajopt::ConstructProblem(pci);
}

int main(int argc, char** argv)
{
  ros::init(argc, argv, "demo1");
  ros::NodeHandle nh, pnh ("~");

  tesseract::tesseract_ros::KDLEnvPtr env;
  if (!loadEnvironment(env))
  {
    return -1;
  }

  addObject(*env);

  if (!env->addManipulator("world_frame", "sander_tcp", "my_robot"))
  {
    ROS_ERROR("Could not create group");
    return -2;
  }

  // initial conditions?
  auto names = env->getJointNames();
  env->setState(names, std::vector<double>(names.size(), 0.0));

  std::vector<std::string> start_state_names = {"joint_6"};
  std::vector<double> start_state_values = {-M_PI};
//  env->setState(start_state_names, start_state_values);


  ros::Publisher pub = nh.advertise<geometry_msgs::PoseArray>("poses", 0, true);

  // visualize
  geometry_msgs::PoseArray poses_msg;
  poses_msg.header.frame_id = env->getManipulator("my_robot")->getBaseLinkName();
  poses_msg.header.stamp = ros::Time::now();

  auto geometric_path = makePath();
  for (const auto& p : geometric_path)
  {
    geometry_msgs::Pose p_msg;
    tf::poseEigenToMsg(p, p_msg);
    poses_msg.poses.push_back(p_msg);
  }

  pub.publish(poses_msg);

  auto opt_problem = makeProblem(env, geometric_path);

  trajopt::BasicTrustRegionSQP optimizer (opt_problem);
  optimizer.initialize(trajopt::trajToDblVec(opt_problem->GetInitTraj()));

  const auto opt_status = optimizer.optimize();
  if (opt_status != trajopt::OptStatus::OPT_CONVERGED) ROS_WARN("Did not converge");

  auto result = trajopt::getTraj(optimizer.x(), opt_problem->GetVars());

  // To & From Vector of parameters
  trajectory_msgs::JointTrajectory out;
  out.joint_names = env->getManipulator("my_robot")->getJointNames();
  for (int i = 0; i < result.rows(); ++i)
  {
    trajectory_msgs::JointTrajectoryPoint jtp;
    for (int j = 0; j < result.cols(); ++j)
    {
      jtp.positions.push_back(result(i, j));
    }
    jtp.time_from_start = ros::Duration(1.0 * i);
    out.points.push_back(jtp);
  }

  tesseract_msgs::TesseractState msg;
  tesseract::tesseract_ros::tesseractToTesseractStateMsg(msg, *env);

  ros::Publisher scene_pub = nh.advertise<tesseract_msgs::TesseractState>("scene", 1, true);


  scene_pub.publish(msg);


  executeTrajectory(out);

  ros::spin();
  return 0;
}
