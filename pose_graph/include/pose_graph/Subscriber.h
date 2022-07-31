#pragma once

#include <image_transport/image_transport.h>
#include <image_transport/subscriber_filter.h>
#include <message_filters/subscriber.h>
#include <message_filters/sync_policies/exact_time.h>
#include <nav_msgs/Odometry.h>
#include <okvis_ros/SvinHealth.h>
#include <sensor_msgs/PointCloud.h>

#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include "common/Definitions.h"
#include "pose_graph/Parameters.h"

class Subscriber {
 public:
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW
  Subscriber(ros::NodeHandle& nh, const Parameters& params);  // NOLINT
  Subscriber() {}

  ~Subscriber() = default;

  void setNodeHandle(ros::NodeHandle& nh);                               // NOLINT
  void getSyncMeasurements(sensor_msgs::ImageConstPtr& kf_image_msg,     // NOLINT
                           nav_msgs::OdometryConstPtr& kf_odom,          // NOLINT
                           sensor_msgs::PointCloudConstPtr& kf_points,   // NOLINT
                           okvis_ros::SvinHealthConstPtr& svin_health);  // NOLINT

  nav_msgs::OdometryConstPtr getPrimitiveEstimatorPose(const uint64_t& ros_stamp);
  void getPrimitiveEstimatorPoses(const uint64_t& ros_stamp, std::vector<nav_msgs::OdometryConstPtr>& poses);  // NOLINT

  const cv::Mat getCorrespondingImage(const uint64_t& ros_stamp);
  const cv::Mat readRosImage(const sensor_msgs::ImageConstPtr& img_msg) const;

 private:
  std::mutex measurement_mutex_;

  ros::NodeHandle* nh_;  // The node handle of the node that will subscribe to the topic.
  std::unique_ptr<image_transport::ImageTransport> it_;  // The image transport.

  Parameters params_;  // The parameters of the node.

  typedef image_transport::SubscriberFilter ImageSubscriber;
  ImageSubscriber keyframe_image_subscriber_;
  // Subscriber to the keyframe pose.
  message_filters::Subscriber<nav_msgs::Odometry> keyframe_pose_subscriber_;
  // Subscriber to the keyframe points.
  message_filters::Subscriber<sensor_msgs::PointCloud> keyframe_points_subscriber_;
  message_filters::Subscriber<okvis_ros::SvinHealth> svin_health_subscriber_;

  typedef message_filters::sync_policies::
      ExactTime<sensor_msgs::Image, nav_msgs::Odometry, sensor_msgs::PointCloud, okvis_ros::SvinHealth>
          keyframe_sync_policy;

  std::unique_ptr<message_filters::Synchronizer<keyframe_sync_policy>> sync_keyframe_;

  // List of other subscribers.
  ros::Subscriber sub_svin_relocalization_odom_;  // Subscriber to the relocalization odometry.
  image_transport::Subscriber sub_orig_image_;    // Subscriber to the original image.
  ros::Subscriber sub_primitive_estimator_;       // Subscriber to the primitive estimator odometry.

  // Callback functions.
  // void keyframeImageCallback(const sensor_msgs::ImageConstPtr& msg);
  // void keyframePoseCallback(const nav_msgs::OdometryConstPtr& msg);
  // void keyframePointsCallback(const sensor_msgs::PointCloudConstPtr& msg);
  void svinRelocalizationOdomCallback(const nav_msgs::OdometryConstPtr& msg);
  // void svinHealthCallback(const okvis_ros::SvinHealthConstPtr& msg);
  void imageCallback(const sensor_msgs::ImageConstPtr& msg);
  void primitiveEstimatorCallback(const nav_msgs::OdometryConstPtr& msg);

  void keyframeCallback(const sensor_msgs::ImageConstPtr& kf_image_msg,
                        const nav_msgs::OdometryConstPtr& kf_odom,
                        const sensor_msgs::PointCloudConstPtr& kf_points,
                        const okvis_ros::SvinHealthConstPtr& svin_health);

  // Buffer queue for the measurements.
  // std::queue<sensor_msgs::ImageConstPtr> kf_image_buffer_;
  // std::queue<nav_msgs::OdometryConstPtr> kf_pose_buffer_;
  // std::queue<sensor_msgs::PointCloudConstPtr> kf_pcl_buffer_;
  // std::queue<okvis_ros::SvinHealthConstPtr> svin_health_buffer_;  // svin health buffer
  std::queue<sensor_msgs::ImageConstPtr> orig_image_buffer_;
  std::queue<nav_msgs::OdometryConstPtr> prim_estimator_odom_buffer_;

  // Topic names.
  std::string kf_image_topic_;             // The topic name of the keyframe image.
  std::string kf_pose_topic_;              // The topic name of the keyframe pose.
  std::string kf_points_topic_;            // The topic name of the keyframe points.
  std::string svin_reloc_odom_topic_;      // The topic name of the relocalization odometry.
  std::string svin_health_topic_;          // The topic name of the health of the SVIn.
  std::string primitive_estimator_topic_;  // The topic name of the primitive estimator odometry.

  double last_image_time_;                // The time of the last image.
  double last_primitive_estimator_time_;  // The time of the last primitive estimator odometry.

  KeyframeCallback keyframe_callback_;         // The callback function for the keyframe.
  PoseCallback primitive_estimator_callback_;  // The callback function for the pose.
  ImageCallback raw_image_callback_;           // The callback function for the original_image.

 public:
  inline double getLatestPrimitiveEstimatorTime() const { return last_primitive_estimator_time_; }
  inline void registerKeyframeCallback(const KeyframeCallback& callback) { keyframe_callback_ = callback; }
};
