// Copyright 2022 Chen Jun
// Licensed under the MIT License.

#include <cv_bridge/cv_bridge.h>
#include <rmw/qos_profiles.h>

#include <ament_index_cpp/get_package_share_directory.hpp>
#include <image_transport/image_transport.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <rclcpp/duration.hpp>
#include <rclcpp/qos.hpp>

// STD
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "armor_detector/armor.hpp"
#include "armor_detector/detector_node.hpp"

namespace rm_auto_aim
{
BaseDetectorNode::BaseDetectorNode(
  const std::string & node_name, const rclcpp::NodeOptions & options)
: Node(node_name, options)
{
  RCLCPP_INFO(this->get_logger(), "Starting DetectorNode!");

  // Detector
  detector_ = initDetector();

  // Number classifier
  auto pkg_path = ament_index_cpp::get_package_share_directory("armor_detector");
  auto model_path = pkg_path + "/model/fc.onnx";
  auto label_path = pkg_path + "/model/label.txt";
  double threshold = this->declare_parameter("classifier.threshold", 0.7);
  classifier_ = std::make_unique<NumberClassifier>(model_path, label_path, threshold);

  // Subscriptions transport type
  transport_ = this->declare_parameter("subscribe_compressed", false) ? "compressed" : "raw";

  // Armors Publisher
  armors_pub_ = this->create_publisher<auto_aim_interfaces::msg::Armors>(
    "/detector/armors", rclcpp::SensorDataQoS());

  // Visualization Marker Publisher
  position_marker_.ns = "armors";
  position_marker_.type = visualization_msgs::msg::Marker::SPHERE_LIST;
  position_marker_.scale.x = position_marker_.scale.y = position_marker_.scale.z = 0.1;
  position_marker_.color.a = 1.0;
  position_marker_.color.r = 1.0;

  text_marker_.ns = "classification";
  text_marker_.action = visualization_msgs::msg::Marker::ADD;
  text_marker_.type = visualization_msgs::msg::Marker::TEXT_VIEW_FACING;
  text_marker_.scale.z = 0.1;
  text_marker_.color.a = 1.0;
  text_marker_.color.r = 1.0;
  text_marker_.color.g = 1.0;
  text_marker_.color.b = 1.0;
  text_marker_.lifetime = rclcpp::Duration::from_seconds(0.1);

  marker_pub_ =
    this->create_publisher<visualization_msgs::msg::MarkerArray>("/detector/marker", 10);

  // Debug Publishers
  debug_ = this->declare_parameter("debug", false);
  if (debug_) {
    createDebugPublishers();
  }

  // Debug param change moniter
  debug_param_sub_ = std::make_shared<rclcpp::ParameterEventHandler>(this);
  debug_cb_handle_ =
    debug_param_sub_->add_parameter_callback("debug", [this](const rclcpp::Parameter & p) {
      debug_ = p.as_bool();
      debug_ ? createDebugPublishers() : destroyDebugPublishers();
    });
}

std::unique_ptr<Detector> BaseDetectorNode::initDetector()
{
  rcl_interfaces::msg::ParameterDescriptor param_desc;
  param_desc.integer_range.resize(1);
  param_desc.integer_range[0].step = 1;
  param_desc.integer_range[0].from_value = 0;
  param_desc.integer_range[0].to_value = 255;
  int min_lightness = declare_parameter("min_lightness", 160, param_desc);

  param_desc.description = "0-RED, 1-BLUE";
  param_desc.integer_range[0].from_value = 0;
  param_desc.integer_range[0].to_value = 1;
  auto detect_color = declare_parameter("detect_color", RED, param_desc);

  Detector::LightParams l_params = {
    .min_ratio = declare_parameter("light.min_ratio", 0.1),
    .max_ratio = declare_parameter("light.max_ratio", 0.55),
    .max_angle = declare_parameter("light.max_angle", 40.0)};

  Detector::ArmorParams a_params = {
    .min_light_ratio = declare_parameter("armor.min_light_ratio", 0.6),
    .min_small_center_distance = declare_parameter("armor.min_small_center_distance", 0.8),
    .max_small_center_distance = declare_parameter("armor.max_small_center_distance", 2.8),
    .min_large_center_distance = declare_parameter("armor.min_large_center_distance", 3.2),
    .max_large_center_distance = declare_parameter("armor.max_large_center_distance", 4.3),
    .max_angle = declare_parameter("armor.max_angle", 35.0)};

  return std::make_unique<Detector>(min_lightness, detect_color, l_params, a_params);
}

std::vector<Armor> BaseDetectorNode::detectArmors(
  const sensor_msgs::msg::Image::ConstSharedPtr & img_msg)
{
  auto start_time = this->now();
  // Convert ROS img to cv::Mat
  auto img = cv_bridge::toCvShare(img_msg, "rgb8")->image;

  // Detect armors
  detector_->min_lightness = get_parameter("min_lightness").as_int();
  detector_->detect_color = get_parameter("detect_color").as_int();

  auto binary_img = detector_->preprocessImage(img);
  auto lights = detector_->findLights(img, binary_img);
  auto armors = detector_->matchLights(lights);

  // Extract numbers
  if (!armors.empty()) {
    classifier_->extractNumbers(img, armors);
    classifier_->threshold = get_parameter("classifier.threshold").as_double();
    classifier_->doClassify(armors);
  }

  // Publish debug info
  if (debug_) {
    auto final_time = this->now();
    auto latency = (final_time - start_time).seconds() * 1000;
    RCLCPP_INFO_STREAM(this->get_logger(), "detectArmors used: " << latency << "ms");
    cv::putText(
      img, "Latency: " + std::to_string(latency) + "ms", cv::Point(10, 30),
      cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0, 255, 0), 2);

    binary_img_pub_.publish(cv_bridge::CvImage(img_msg->header, "mono8", binary_img).toImageMsg());

    std::sort(
      detector_->debug_lights.data.begin(), detector_->debug_lights.data.end(),
      [](const auto & l1, const auto & l2) { return l1.center_x < l2.center_x; });
    std::sort(
      detector_->debug_armors.data.begin(), detector_->debug_armors.data.end(),
      [](const auto & a1, const auto & a2) { return a1.center_x < a2.center_x; });

    lights_data_pub_->publish(detector_->debug_lights);
    armors_data_pub_->publish(detector_->debug_armors);

    if (!armors.empty()) {
      // Combine all number images to one
      std::vector<cv::Mat> number_imgs;
      number_imgs.reserve(armors.size());
      for (auto & armor : armors) {
        cv::resize(armor.number_img, armor.number_img, cv::Size(20, 28));
        number_imgs.emplace_back(armor.number_img);
      }
      cv::Mat all_num_img;
      cv::vconcat(number_imgs, all_num_img);

      number_pub_->publish(*cv_bridge::CvImage(img_msg->header, "mono8", all_num_img).toImageMsg());
    }

    drawResults(img, lights, armors);
    final_img_pub_.publish(cv_bridge::CvImage(img_msg->header, "rgb8", img).toImageMsg());
  }

  return armors;
}

void BaseDetectorNode::drawResults(
  cv::Mat & img, const std::vector<Light> & lights, const std::vector<Armor> & armors)
{
  // Draw Lights
  for (const auto & light : lights) {
    auto color = light.color == RED ? cv::Scalar(255, 255, 0) : cv::Scalar(255, 0, 255);
    cv::ellipse(img, light, color, 2);
  }

  // Draw armors
  for (const auto & armor : armors) {
    cv::line(img, armor.left_light.top, armor.right_light.bottom, cv::Scalar(0, 255, 0), 2);
    cv::line(img, armor.left_light.bottom, armor.right_light.top, cv::Scalar(0, 255, 0), 2);
  }

  // Show numbers and confidence
  for (const auto & armor : armors) {
    cv::putText(
      img, armor.classfication_result, armor.left_light.top, cv::FONT_HERSHEY_SIMPLEX, 0.8,
      cv::Scalar(0, 255, 255), 2);
  }

  // Draw camera center
  cv::circle(img, cam_center_, 5, cv::Scalar(255, 0, 0), 2);
}

void BaseDetectorNode::createDebugPublishers()
{
  lights_data_pub_ =
    this->create_publisher<auto_aim_interfaces::msg::DebugLights>("/debug/lights", 10);
  armors_data_pub_ =
    this->create_publisher<auto_aim_interfaces::msg::DebugArmors>("/debug/armors", 10);
  number_pub_ = this->create_publisher<sensor_msgs::msg::Image>("/number", 10);

  binary_img_pub_ = image_transport::create_publisher(this, "/binary_img");
  final_img_pub_ = image_transport::create_publisher(this, "/final_img");
}

void BaseDetectorNode::destroyDebugPublishers()
{
  lights_data_pub_.reset();
  armors_data_pub_.reset();
  number_pub_.reset();

  binary_img_pub_.shutdown();
  final_img_pub_.shutdown();
}

void BaseDetectorNode::publishMarkers()
{
  using Marker = visualization_msgs::msg::Marker;
  position_marker_.action = armors_msg_.armors.empty() ? Marker::DELETE : Marker::ADD;
  marker_array_.markers.emplace_back(position_marker_);
  marker_pub_->publish(marker_array_);
}

}  // namespace rm_auto_aim
