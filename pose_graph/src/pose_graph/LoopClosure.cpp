#include "pose_graph/LoopClosure.h"

#include <pcl/io/ply_io.h>
#include <pcl/point_types.h>
#include <pcl_conversions/pcl_conversions.h>
#include <ros/package.h>
#include <ros/ros.h>
#include <sensor_msgs/PointCloud2.h>
#include <std_srvs/Trigger.h>

#include <Eigen/SVD>
#include <algorithm>
#include <boost/filesystem.hpp>
#include <boost/thread.hpp>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "utils/Statistics.h"
#include "utils/UtilsOpenCV.h"

LoopClosure::LoopClosure(const Parameters& params)
    : nh_private_("~"),
      params_(std::make_shared<Parameters>(params)),
      pose_graph_(nullptr),
      camera_pose_visualizer_(nullptr),
      global_map_(nullptr),
      keyframe_tracking_queue_("keyframe_queue"),
      raw_image_buffer_(kBufferLengthNs) {
  frame_index_ = 0;
  sequence_ = 1;

  last_translation_ = Eigen::Vector3d(-100, -100, -100);

  setup();

  // pubSparseMap = nh_private_.advertise<sensor_msgs::PointCloud2>("sparse_pointcloud", 10);

  save_pointcloud_service_ = nh_private_.advertiseService("save_pointcloud", &LoopClosure::savePointCloud, this);

  consecutive_tracking_failures_ = 0;
  last_keyframe_time_ = 0;
  last_primitive_estmator_time_ = 0.0;
  tracking_status_ = TrackingStatus::NOT_INITIALIZED;

  init_t_w_prim_.setIdentity();
  init_t_w_svin_.setIdentity();

  switch_prim_pose_.setIdentity();
  switch_svin_pose_.setIdentity();
  switch_uber_pose_.setIdentity();

  last_t_w_prim_.setIdentity();
  last_scaled_prim_pose_.setZero();

  prim_estimator_keyframes_ = 0;
  vio_traj_length_ = 0.0;
  prim_traj_length_ = 0.0;
  scale_between_vio_prim_ = 0.0;
  shutdown_ = false;
}

void LoopClosure::setup() {
  global_map_ = std::unique_ptr<GlobalMap>(new GlobalMap());

  pose_graph_ = std::unique_ptr<PoseGraph>(new PoseGraph());
  pose_graph_->setPublishers(nh_private_);
  pose_graph_->set_svin_results_file(params_->svin_w_loop_path_);
  pose_graph_->set_fast_relocalization(params_->fast_relocalization_);
  pose_graph_->registerLoopClosureOptimizationCallback(
      std::bind(&GlobalMap::loopClosureOptimizationFinishCallback, global_map_.get(), std::placeholders::_1));
  pose_graph_->startOptimizationThread();

  camera_pose_visualizer_ = std::unique_ptr<CameraPoseVisualization>(new CameraPoseVisualization(1, 0, 0, 1));
  camera_pose_visualizer_->setScale(params_->camera_visual_size_);
  camera_pose_visualizer_->setLineWidth(params_->camera_visual_size_ / 10.0);

  // Loading vocabulary
  voc_ = new BriefVocabulary(params_->vocabulary_file_);
  BriefDatabase db;
  db.setVocabulary(*voc_, false, 0);
  pose_graph_->setBriefVocAndDB(voc_, db);

  publisher.setParameters(*params_);
  publisher.setPublishers();

  timer_ = nh_private_.createTimer(ros::Duration(3), &LoopClosure::updatePublishGlobalMap, this);

  if (params_->debug_image_) {
    setupOutputLogDirectories();
  }
}

void LoopClosure::run() {
  while (!shutdown_) {
    std::unique_ptr<KeyframeInfo> keyframe_info = nullptr;
    bool queue_state = keyframe_tracking_queue_.popBlocking(keyframe_info);
    if (queue_state) {
      CHECK(keyframe_info);
      std::map<Keyframe*, int> KFcounter;

      for (size_t i = 0; i < keyframe_info->keyfame_points_.size(); ++i) {
        double quality = static_cast<double>(keyframe_info->tracking_info_.points_quality_[i]);
        for (auto observed_kf_index : keyframe_info->point_covisibilities_[i]) {
          if (kfMapper_.find(observed_kf_index) != kfMapper_.end()) {
            Keyframe* observed_kf =
                kfMapper_.find(observed_kf_index)->second;  // Keyframe where this point_3d has been observed
            KFcounter[observed_kf]++;
          }
        }
      }

      Keyframe* keyframe = new Keyframe(keyframe_info->timestamp_,
                                        keyframe_info->keypoint_ids_,
                                        keyframe_info->keyframe_index_,
                                        keyframe_info->translation_,
                                        keyframe_info->rotation_,
                                        keyframe_info->keyframe_image_,
                                        keyframe_info->keyfame_points_,
                                        keyframe_info->cv_keypoints_,
                                        KFcounter,
                                        sequence_,
                                        voc_,
                                        *params_);
      kfMapper_.insert(std::make_pair(keyframe_info->keyframe_index_, keyframe));
      pose_graph_->addKFToPoseGraph(keyframe, 1);

      cv::Mat original_color_image;
      if (!raw_image_buffer_.getNearestValueToTime(
              keyframe_info->timestamp_.toNSec(), 1000000, &original_color_image)) {
        LOG(WARNING) << "Could not color image for keyframe with timestamp " << keyframe_info->timestamp_.toNSec();
      } else {
        if (params_->resize_factor_ != 0) {
          cv::resize(original_color_image,
                     original_color_image,
                     cv::Size(params_->image_width_, params_->image_height_),
                     cv::INTER_LINEAR);
        }
        if (kfMapper_.find(keyframe_info->keyframe_index_) != kfMapper_.end()) {
          addPointsToGlobalMap(keyframe_info->keyframe_index_,
                               original_color_image,
                               keyframe_info->rotation_,
                               keyframe_info->translation_,
                               keyframe_info->keyfame_points_,
                               keyframe_info->tracking_info_.points_quality_,
                               keyframe_info->keypoint_ids_,

                               keyframe_info->cv_keypoints_);
        } else {
          LOG(WARNING) << "Keyframe not found";
        }
      }
    }
    frame_index_++;
  }
}

void LoopClosure::updatePublishGlobalMap(const ros::TimerEvent& event) {
  // only update the global map if the pose graph optimization is finished after loop closure

  if (global_map_->loop_closure_optimization_finished_) updateGlobalMap();

  pcl::PointCloud<pcl::PointXYZRGB>::Ptr global_map_pcl(new pcl::PointCloud<pcl::PointXYZRGB>);
  getGlobalPointCloud(global_map_pcl);
  sensor_msgs::PointCloud2 pcl_msg;
  pcl::toROSMsg(*global_map_pcl, pcl_msg);
  pcl_msg.header.frame_id = "world";
  pcl_msg.header.stamp = ros::Time::now();

  publisher.publishGlobalMap(pcl_msg);
}

void LoopClosure::getGlobalPointCloud(pcl::PointCloud<pcl::PointXYZRGB>::Ptr& pointcloud) {
  for (auto point_landmark_map : global_map_->getMapPoints()) {
    Landmark point_landmark = point_landmark_map.second;
    Eigen::Vector3d global_pos = point_landmark.point_;
    Eigen::Vector3d color = point_landmark.color_;
    double quality = point_landmark.quality_;

    if (quality > params_->min_landmark_quality_) {
      pcl::PointXYZRGB point;
      point.x = global_pos(0);
      point.y = global_pos(1);
      point.z = global_pos(2);
      point.r = static_cast<uint8_t>(color.x());
      point.g = static_cast<uint8_t>(color.y());
      point.b = static_cast<uint8_t>(color.z());
      pointcloud->push_back(point);
    }
  }
}

void LoopClosure::addPointsToGlobalMap(const int64_t keyframe_index,
                                       const cv::Mat& color_image,
                                       const Eigen::Matrix3d& camera_rotation,
                                       const Eigen::Vector3d& camera_translation,
                                       const std::vector<cv::Point3f>& keyframe_points,
                                       const std::vector<float>& point_qualities,
                                       const std::vector<Eigen::Vector3i>& point_ids,
                                       const std::vector<cv::KeyPoint>& cv_keypoints) {
  for (size_t i = 0; i < keyframe_points.size(); ++i) {
    float quality = point_qualities[i];
    if (quality < params_->min_landmark_quality_) continue;
    Eigen::Vector3d global_point_position(keyframe_points[i].x, keyframe_points[i].y, keyframe_points[i].z);
    Eigen::Vector3d point_cam_frame = camera_rotation.transpose() * (global_point_position - camera_translation);

    cv::KeyPoint image_point = cv_keypoints[i];
    cv::Vec3b color =
        color_image.at<cv::Vec3b>(static_cast<uint16_t>(image_point.pt.y), static_cast<uint16_t>(image_point.pt.x));
    // bgr to rgb
    Eigen::Vector3d color_eigen(
        static_cast<double>(color[2]), static_cast<double>(color[1]), static_cast<double>(color[0]));
    uint64_t landmark_id = static_cast<uint64_t>(point_ids[i].x());

    global_map_->addLandmark(global_point_position, landmark_id, quality, keyframe_index, point_cam_frame, color_eigen);
  }
}

void LoopClosure::updateGlobalMap() {
  for (auto point_landmark_map : global_map_->getMapPoints()) {
    uint64_t landmark_id = point_landmark_map.first;
    Landmark point_landmark = point_landmark_map.second;

    Eigen::Vector3d point_3d = Eigen::Vector3d::Zero();
    Eigen::Vector3d color = Eigen::Vector3d::Zero();
    double quality = 0.0;
    uint64_t total_observations = 0;
    for (auto kf_observation : point_landmark.keyframe_observations_) {
      uint64_t kf_id = kf_observation.first;
      Observation obs = kf_observation.second;
      Eigen::Vector3d local_pos = obs.local_pos_;
      Eigen::Vector3d local_color = obs.color_;
      double kf_quality = obs.quality_;

      // Converting to global coordinates
      if (kfMapper_.find(kf_id) == kfMapper_.end()) {
        std::cout << "Keyframe not found" << std::endl;
        continue;
      }

      Keyframe* kf = kfMapper_.find(kf_id)->second;
      Eigen::Matrix3d R_kf_w;
      Eigen::Vector3d T_kf_w;
      kf->getPose(T_kf_w, R_kf_w);

      Eigen::Vector3d global_pos = R_kf_w * local_pos + T_kf_w;
      point_3d = point_3d + global_pos * kf_quality;
      color = color + local_color * kf_quality;
      quality = quality + kf_quality;
      total_observations += 1;
    }

    point_3d = point_3d / quality;
    color = color / quality;
    quality = quality / total_observations;

    global_map_->updateLandmark(landmark_id, point_3d, quality, color);
  }
  global_map_->loop_closure_optimization_finished_ = false;
}

bool LoopClosure::savePointCloud(std_srvs::TriggerRequest& request, std_srvs::TriggerResponse& response) {
  ROS_INFO_STREAM("!! Saving Point Cloud !!");
  pcl::PointCloud<pcl::PointXYZRGB>::Ptr pointcloud(new pcl::PointCloud<pcl::PointXYZRGB>);
  getGlobalPointCloud(pointcloud);

  std::string pkg_path = ros::package::getPath("pose_graph");
  std::string pointcloud_file = pkg_path + "/reconstruction_results/pointcloud.ply";

  pcl::io::savePLYFileBinary(pointcloud_file, *pointcloud);
  response.success = true;
  response.message = "Saving Point Cloud ";
  return true;
}

void LoopClosure::updatePrimiteEstimatorTrajectory(const nav_msgs::OdometryConstPtr& pose_msg) {
  geometry_msgs::PoseStamped pose_stamped;
  pose_stamped.header = pose_msg->header;
  pose_stamped.header.seq = primitive_estimator_poses_.size() + 1;
  pose_stamped.pose = Utility::matrixToRosPose(init_t_w_svin_ * init_t_w_prim_.inverse() *
                                               Utility::rosPoseToMatrix(pose_msg->pose.pose) * params_->T_body_imu_ *
                                               params_->T_imu_cam0_);
  primitive_estimator_poses_.push_back(pose_stamped);
}

bool LoopClosure::healthCheck(const okvis_ros::SvinHealthConstPtr& health_msg, std::string& error_msg) {
  // ROS_INFO_STREAM(Utility::healthMsgToString(health_msg));
  std::stringstream ss;
  std::setprecision(5);

  HealthParams health_params = params_->health_params_;
  uint32_t total_triangulated_keypoints = health_msg->numTrackedKps;

  if (total_triangulated_keypoints < health_params.min_tracked_keypoints) {
    ss << "Not enough triangulated keypoints: " << total_triangulated_keypoints << std::endl;
    error_msg = ss.str();
    return false;
  }

  std::vector<int> keypoints_per_quadrant = health_msg->kpsPerQuadrant;

  bool quadrant_check = std::all_of(keypoints_per_quadrant.begin(), keypoints_per_quadrant.end(), [&](int kp_count) {
    return kp_count >= health_params.kps_per_quadrant;
  });

  if (!quadrant_check && *std::max_element(keypoints_per_quadrant.begin(), keypoints_per_quadrant.end()) <=
                             10.0 * health_params.kps_per_quadrant) {
    ss << "Not enough keypoints per quadrant:  [" << keypoints_per_quadrant[0] << ", " << keypoints_per_quadrant[1]
       << ", " << keypoints_per_quadrant[2] << ", " << keypoints_per_quadrant[3] << "]" << std::endl;
    error_msg = ss.str();
    return false;
  }

  uint32_t new_detected_keypoints_kf = health_msg->newKps;
  float new_detected_keypoints_ratio =
      static_cast<float>(new_detected_keypoints_kf) / static_cast<float>(total_triangulated_keypoints);

  if (new_detected_keypoints_ratio >= 0.75) {
    ss << "Too many new keypoints: " << new_detected_keypoints_ratio << std::endl;
    error_msg = ss.str();
    return false;
  }

  double average_response =
      std::accumulate(health_msg->responseStrengths.begin(), health_msg->responseStrengths.end(), double(0.0)) /
      static_cast<double>(health_msg->responseStrengths.size());
  float fraction_with_low_detector_response =
      std::count_if(health_msg->responseStrengths.begin(),
                    health_msg->responseStrengths.end(),
                    [&](double response) { return response < average_response; }) /
      static_cast<float>(health_msg->responseStrengths.size());

  if (fraction_with_low_detector_response >= 0.85) {
    ss << "Too many detectors with low response: " << fraction_with_low_detector_response << std::endl;
    error_msg = ss.str();
    return false;
  }

  return true;
}

void LoopClosure::setupOutputLogDirectories() {
  std::string pacakge_path = ros::package::getPath("pose_graph");

  std::string output_dir = pacakge_path + "/output_logs/loop_candidates/";
  if (!boost::filesystem::is_directory(output_dir) || !boost::filesystem::exists(output_dir)) {
    boost::filesystem::create_directory(output_dir);
  }
  for (const auto& entry : boost::filesystem::directory_iterator(output_dir)) {
    boost::filesystem::remove_all(entry.path());
  }

  output_dir = pacakge_path + "/output_logs/descriptor_matched/";
  if (!boost::filesystem::is_directory(output_dir) || !boost::filesystem::exists(output_dir)) {
    boost::filesystem::create_directory(output_dir);
  }
  for (const auto& entry : boost::filesystem::directory_iterator(output_dir)) {
    boost::filesystem::remove_all(entry.path());
  }

  output_dir = pacakge_path + "/output_logs/pnp_verified/";
  if (!boost::filesystem::is_directory(output_dir) || !boost::filesystem::exists(output_dir)) {
    boost::filesystem::create_directory(output_dir);
  }
  for (const auto& entry : boost::filesystem::directory_iterator(output_dir)) {
    boost::filesystem::remove_all(entry.path());
  }

  output_dir = pacakge_path + "/output_logs/loop_closure/";
  if (!boost::filesystem::is_directory(output_dir) || !boost::filesystem::exists(output_dir)) {
    boost::filesystem::create_directory(output_dir);
  }
  for (const auto& entry : boost::filesystem::directory_iterator(output_dir)) {
    boost::filesystem::remove_all(entry.path());
  }

  output_dir = pacakge_path + "/output_logs/geometric_verification/";
  if (!boost::filesystem::is_directory(output_dir) || !boost::filesystem::exists(output_dir)) {
    boost::filesystem::create_directory(output_dir);
  }
  for (const auto& entry : boost::filesystem::directory_iterator(output_dir)) {
    boost::filesystem::remove_all(entry.path());
  }

  std::string loop_closure_file = pacakge_path + "/output_logs/loop_closure.txt";
  if (boost::filesystem::exists(loop_closure_file)) {
    boost::filesystem::remove(loop_closure_file);
  }
  std::ofstream loop_path_file(loop_closure_file, std::ios::out);
  loop_path_file << "cur_kf_id"
                 << " "
                 << "cur_kf_ts"
                 << " "
                 << "matched_kf_id"
                 << " "
                 << "matched_kf_ts"
                 << " "
                 << "relative_tx"
                 << " "
                 << "relative_ty"
                 << " "
                 << "relative_tz"
                 << " "
                 << "relative_qx"
                 << " "
                 << "relative_qy"
                 << " "
                 << "relative_qz"
                 << " "
                 << "relative_qw" << std::endl;
  loop_path_file.close();

  std::string switch_info_file = pacakge_path + "/output_logs/switch_info.txt";
  if (boost::filesystem::exists(switch_info_file)) {
    boost::filesystem::remove(switch_info_file);
  }
  std::ofstream switch_info_file_stream(switch_info_file, std::ios::out);
  switch_info_file_stream << "type"
                          << " "
                          << "vio_stamp"
                          << " "
                          << "prim_stamp"
                          << " "
                          << "uber_stamp" << std::endl;
  switch_info_file_stream.close();
}

void LoopClosure::shutdown() {
  LOG_IF(ERROR, shutdown_) << "Shutdown requested, but PoseGraph modile was already shutdown.";
  LOG(INFO) << "Shutting down PoseGraph module.";
  keyframe_tracking_queue_.shutdown();
  shutdown_ = true;
}