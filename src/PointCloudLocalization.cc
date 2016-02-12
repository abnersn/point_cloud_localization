/*
 * Copyright (c) 2016, The Regents of the University of California (Regents).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *    2. Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *
 *    3. Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS AS IS
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * Please contact the author(s) of this library if you have any questions.
 * Authors: Erik Nelson            ( eanelson@eecs.berkeley.edu )
 */

#include <point_cloud_localization/PointCloudLocalization.h>
#include <geometry_utils/GeometryUtilsROS.h>
#include <parameter_utils/ParameterUtils.h>
#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/TransformStamped.h>

#include <pcl/registration/gicp.h>

namespace gu = geometry_utils;
namespace gr = gu::ros;
namespace pu = parameter_utils;

PointCloudLocalization::PointCloudLocalization() {}
PointCloudLocalization::~PointCloudLocalization() {}

bool PointCloudLocalization::Initialize(const ros::NodeHandle& n) {
  name_ = ros::names::append(n.getNamespace(), "PointCloudLocalization");

  if (!LoadParameters(n)) {
    ROS_ERROR("%s: Failed to load parameters.", name_.c_str());
    return false;
  }

  if (!RegisterCallbacks(n)) {
    ROS_ERROR("%s: Failed to register callbacks.", name_.c_str());
    return false;
  }

  return true;
}

bool PointCloudLocalization::LoadParameters(const ros::NodeHandle& n) {
  // Load frame ids.
  if (!pu::Get("frame_id/fixed", fixed_frame_id_)) return false;
  if (!pu::Get("frame_id/base", base_frame_id_)) return false;

  // Load initial position.
  double init_x = 0.0, init_y = 0.0, init_z = 0.0;
  double init_roll = 0.0, init_pitch = 0.0, init_yaw = 0.0;
  if (!pu::Get("init/position/x", init_x)) return false;
  if (!pu::Get("init/position/y", init_y)) return false;
  if (!pu::Get("init/position/z", init_z)) return false;
  if (!pu::Get("init/orientation/roll", init_roll)) return false;
  if (!pu::Get("init/orientation/pitch", init_pitch)) return false;
  if (!pu::Get("init/orientation/yaw", init_yaw)) return false;

  integrated_estimate_.translation = gu::Vec3(init_x, init_y, init_z);
  integrated_estimate_.rotation = gu::Rot3(init_roll, init_pitch, init_yaw);

  // Load algorithm parameters.
  if (!pu::Get("localization/ransac_thresh", params_.ransac_thresh)) return false;
  if (!pu::Get("localization/tf_epsilon", params_.tf_epsilon)) return false;
  if (!pu::Get("localization/corr_dist", params_.corr_dist)) return false;
  if (!pu::Get("localization/iterations", params_.iterations)) return false;

  return true;
}

bool PointCloudLocalization::RegisterCallbacks(const ros::NodeHandle& n) {
  // Create a local nodehandle to manage callback subscriptions.
  ros::NodeHandle nl(n);

  integrated_estimate_pub_ = nl.advertise<geometry_msgs::PoseStamped>(
      "localization_integrated_estimate", 10, false);

  return true;
}

bool PointCloudLocalization::MotionUpdate(
    const gu::Transform3& incremental_odom) {
  // Store the incremental transform from odometry.
  incremental_estimate_ = incremental_odom;
  return true;
}

bool PointCloudLocalization::TransformPointsToBaseFrame(
    const PointCloud& points, PointCloud* points_transformed) const {
  if (points_transformed == NULL) {
    ROS_ERROR("%s: Output is null.", name_.c_str());
    return false;
  }

  // Compose the current incremental estimate (from odometry) with the
  // integrated estimate, and transform the incoming point cloud.
  const gu::Transform3 estimate =
      gu::PoseUpdate(integrated_estimate_, incremental_estimate_);
  const Eigen::Matrix<double, 3, 3> R = estimate.rotation.Eigen();
  const Eigen::Matrix<double, 3, 1> T = estimate.translation.Eigen();

  Eigen::Matrix4d tf;
  tf.block(0, 0, 3, 3) = R;
  tf.block(0, 3, 3, 1) = T;

  pcl::transformPointCloud(points, *points_transformed, tf);

  return true;
}

bool PointCloudLocalization::MeasurementUpdate(const PointCloud::Ptr& query,
                                               const PointCloud::Ptr& reference,
                                               PointCloud* aligned_query) {
  if (aligned_query == NULL) {
    ROS_ERROR("%s: Output is null.", name_.c_str());
    return false;
  }

  // Store time stamp.
  stamp_.fromNSec(query->header.stamp*1e3);

  // ICP-based alignment. Generalized ICP does (roughly) plane-to-plane
  // matching, and is much more robust than standard ICP.
  pcl::GeneralizedIterativeClosestPoint<pcl::PointXYZ, pcl::PointXYZ> icp;
  icp.setRANSACOutlierRejectionThreshold(params_.ransac_thresh);
  icp.setTransformationEpsilon(params_.tf_epsilon);
  icp.setMaxCorrespondenceDistance(params_.corr_dist);
  icp.setMaximumIterations(params_.iterations);

  icp.setInputSource(query);
  icp.setInputTarget(reference);

  icp.align(*aligned_query);

  // Retrieve transformation and estimate and update.
  const Eigen::Matrix4f T = icp.getFinalTransformation();
  gu::Transform3 pose_update;
  pose_update.translation = gu::Vec3(T(0, 3), T(1, 3), T(2, 3));
  pose_update.rotation = gu::Rot3(T(0, 0), T(0, 1), T(0, 2),
                                  T(1, 0), T(1, 1), T(1, 2),
                                  T(2, 0), T(2, 1), T(2, 2));

  incremental_estimate_ = gu::PoseUpdate(incremental_estimate_, pose_update);
  integrated_estimate_ =
      gu::PoseUpdate(integrated_estimate_, incremental_estimate_);

  // Convert pose estimates to ROS format and publish.
  PublishPose(incremental_estimate_, incremental_estimate_pub_);
  PublishPose(integrated_estimate_, integrated_estimate_pub_);

  // Publish transform between fixed frame and localization frame.
  geometry_msgs::TransformStamped tf;
  tf.transform = gr::ToRosTransform(integrated_estimate_);
  tf.header.stamp = stamp_;
  tf.header.frame_id = fixed_frame_id_;
  tf.child_frame_id = base_frame_id_;
  tfbr_.sendTransform(tf);

  return true;
}

void PointCloudLocalization::PublishPose(const gu::Transform3& pose,
                                         const ros::Publisher& pub) {
  // Check for subscribers before doing any work.
  if (pub.getNumSubscribers() == 0)
   return;

  // Convert from gu::Transform3 to ROS's PoseStamped type and publish.
  geometry_msgs::PoseStamped ros_pose;
  ros_pose.pose = gr::ToRosPose(pose);
  ros_pose.header.frame_id = base_frame_id_;
  ros_pose.header.stamp = stamp_;
  pub.publish(ros_pose);
}