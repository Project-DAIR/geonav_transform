/*

Copyright (c) 2017, Brian Bingham
All rights reserved

This file is part of the geonav_transform package.

Geonav_transform is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Geonav_transform is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this package.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "geonav_transform/geonav_transform.h"
#include "geonav_transform/navsat_conversions.h"
#include "geonav_transform/geonav_utilities.h"

#include <tf2_geometry_msgs/tf2_geometry_msgs.h>

#include <XmlRpcException.h>

#include <string>

#include <memory>

namespace GeonavTransform
{
  GeonavTransform::GeonavTransform() : // Initialize attributes
                                       broadcast_utm2odom_transform_(true),
                                       broadcast_odom2base_transform_(true),
                                       nav_frame_id_(""),
                                       zero_altitude_(true),
                                       utm_frame_id_("utm"),
                                       odom_frame_id_("odom"),
                                       base_link_frame_id_("base_link"),
                                       utm_zone_(""),
                                       tf_listener_(tf_buffer_)
  {
    // Initialize transforms
    transform_odom2base_ = tf2::Transform(tf2::Transform::getIdentity());
    transform_odom2base_inverse_ = tf2::Transform(tf2::Transform::getIdentity());
    transform_utm2odom_ = tf2::Transform(tf2::Transform::getIdentity());
    transform_utm2odom_inverse_ = tf2::Transform(tf2::Transform::getIdentity());
  }

  GeonavTransform::~GeonavTransform()
  {
  }

  void GeonavTransform::run()
  {

    double frequency = 100.0;
    double delay = 0.0;

    ros::NodeHandle nh;
    ros::NodeHandle nh_priv("~");

    nav_update_time_ = ros::Time::now();

    // Load ROS parameters
    nh_priv.param("frequency", frequency, 10.0);
    nh_priv.param("orientation_ned", orientation_ned_, true);

    nh_priv.param("broadcast_utm2odom_transform", broadcast_utm2odom_transform_, true);
    nh_priv.param("broadcast_odom2base_transform", broadcast_odom2base_transform_, true);
    nh_priv.param("zero_altitude", zero_altitude_, false);
    nh_priv.param("altitude", altitude_, 0.0);
    nh_priv.param<std::string>("base_link_frame_id", base_link_frame_id_, "base_link");
    nh_priv.param<std::string>("odom_frame_id", odom_frame_id_, "odom");
    nh_priv.param<std::string>("utm_frame_id", utm_frame_id_, "utm");

    // Datum parameter - required
    double datum_lat;
    double datum_lon;
    double datum_yaw;
    tf2::Quaternion quat = tf2::Quaternion::getIdentity();
    tf2::Quaternion yaw_offset = tf2::Quaternion::getIdentity();
    yaw_offset.setRPY(0, 0, M_PI/2);

    // Setup transforms and messages
    nav_in_odom_.header.frame_id = odom_frame_id_;
    nav_in_odom_.child_frame_id = base_link_frame_id_;
    nav_in_odom_.header.seq = 0;
    nav_in_utm_.header.frame_id = utm_frame_id_;
    nav_in_utm_.child_frame_id = base_link_frame_id_;
    nav_in_utm_.header.seq = 0;
    transform_msg_utm2odom_.header.frame_id = utm_frame_id_;
    transform_msg_utm2odom_.child_frame_id = odom_frame_id_;
    transform_msg_utm2odom_.header.seq = 0;
    transform_msg_odom2base_.header.frame_id = odom_frame_id_;
    transform_msg_odom2base_.child_frame_id = base_link_frame_id_;
    transform_msg_odom2base_.header.seq = 0;

    // Publisher - Odometry relative to the odom frame
    odom_pub_ = nh.advertise<nav_msgs::Odometry>("geonav_odom", 10);
    utm_pub_ = nh.advertise<nav_msgs::Odometry>("geonav_utm", 10);

    // Publisher - Odometry in Geo frame
    geo_pub_ = nh.advertise<nav_msgs::Odometry>("geonav_geo", 10);

    // Subscriber - Odometry in GPS frame.
    // for converstion from geo. coord. to local nav. coord.
    ros::Subscriber geo_odom_sub = nh.subscribe("nav_odom", 10,
                                                &GeonavTransform::navOdomCallback,
                                                this);
    // Subscriber - Odometry in Nav. frame.
    // for conversion from local nav. coord. to geo. coord
    ros::Subscriber nav_odom_sub = nh.subscribe("geo_odom", 1,
                                                &GeonavTransform::geoOdomCallback,
                                                this);

    boost::shared_ptr<nav_msgs::Odometry const> odom_msg_ptr;

    odom_msg_ptr = ros::topic::waitForMessage<nav_msgs::Odometry>("nav_odom", ros::Duration(10));

    if (odom_msg_ptr == NULL)
      ROS_INFO("No odometry messages received");
    else
    {
      // Set datum - published static transform
      tf2::convert(odom_msg_ptr->pose.pose.orientation, quat);
      quat = yaw_offset * quat;
      quat.normalize();
      setDatum(odom_msg_ptr->pose.pose.position.y, odom_msg_ptr->pose.pose.position.x, odom_msg_ptr->pose.pose.position.z, quat);
    }

    // Loop
    ros::Rate rate(frequency);
    while (ros::ok())
    {
      ros::spinOnce();

      // Check for odometry
      if ((ros::Time::now().toSec() - nav_update_time_.toSec()) > 1.0)
      {
        ROS_WARN_STREAM("Haven't received Odometry on <"
                        << geo_odom_sub.getTopic() << "> for 1.0 seconds!"
                        << " Will not broadcast transform!");
      }
      else
      {
        // send transforms - particularly odom->base (utm->odom is static)
        broadcastTf();
      }
      rate.sleep();
    } // end of Loop
  }   // end of ::run()

  void GeonavTransform::broadcastTf(void)
  {
    transform_msg_odom2base_.header.stamp = ros::Time::now();
    transform_msg_odom2base_.header.seq++;
    transform_msg_odom2base_.transform = tf2::toMsg(transform_odom2base_);
    tf_broadcaster_.sendTransform(transform_msg_odom2base_);
  }
  bool GeonavTransform::setDatum(double lat, double lon, double alt,
                                 tf2::Quaternion q)
  {
    double utm_x = 0;
    double utm_y = 0;
    NavsatConversions::LLtoUTM(lat, lon, utm_y, utm_x, utm_zone_);

    ROS_INFO_STREAM("Datum (latitude, longitude, altitude) is ("
                    << std::fixed << lat << ", "
                    << lon << ", " << alt << ")");
    ROS_INFO_STREAM("Datum UTM Zone is: " << utm_zone_);
    ROS_INFO_STREAM("Datum UTM coordinate is ("
                    << std::fixed << utm_x << ", " << utm_y << ")");

    // Set the transform utm->odom
    transform_utm2odom_.setOrigin(tf2::Vector3(utm_x, utm_y, alt));
    transform_utm2odom_.setRotation(q);
    transform_utm2odom_inverse_ = transform_utm2odom_.inverse();
    // Convert quaternion to RPY - to double check and diplay
    tf2::Matrix3x3 mat(q);
    double roll, pitch, yaw;
    mat.getRPY(roll, pitch, yaw);
    ROS_INFO_STREAM("Datum orientation roll, pitch, yaw is ("
                    << roll << ", " << pitch << ", " << yaw << ")");

    // ROS_INFO_STREAM("Transform utm -> odom is: " << transform_utm2odom_);

    // Send out static UTM transform - frames are specified in ::run()
    transform_msg_utm2odom_.header.stamp = ros::Time::now();
    transform_msg_utm2odom_.header.seq++;
    transform_msg_utm2odom_.transform = tf2::toMsg(transform_utm2odom_);
    transform_msg_utm2odom_.transform.translation.z = (zero_altitude_ ? 0.0 : transform_msg_utm2odom_.transform.translation.z);
    utm_broadcaster_.sendTransform(transform_msg_utm2odom_);

    return true;
  } // end setDatum

  void GeonavTransform::navOdomCallback(const nav_msgs::OdometryConstPtr &msg)
  {
    nav_frame_id_ = msg->header.frame_id;
    if (nav_frame_id_.empty())
    {
      ROS_WARN_STREAM_ONCE("Odometry message has empty frame_id. "
                           "Will assume navsat device is mounted at "
                           "robot's origin.");
    }
    // Make sure the GPS data is usable - can't use NavSatStatus since we
    // are making due with an Odometry message
    bool good_gps = (!std::isnan(msg->pose.pose.position.x) &&
                     !std::isnan(msg->pose.pose.position.y) &&
                     !std::isnan(msg->pose.pose.position.z));
    if (!good_gps)
    {
      ROS_WARN_STREAM("Bad GPS!  Won't transfrom");
      return;
    }

    double utmX = 0;
    double utmY = 0;
    std::string utm_zone_tmp;
    nav_update_time_ = ros::Time::now();
    NavsatConversions::LLtoUTM(msg->pose.pose.position.y,
                               msg->pose.pose.position.x,
                               utmY, utmX, utm_zone_tmp);
    ROS_DEBUG_STREAM_THROTTLE(2.0, "Latest GPS (lat, lon, alt): "
                                       << msg->pose.pose.position.y << " , "
                                       << msg->pose.pose.position.x << " , "
                                       << msg->pose.pose.position.z);
    ROS_DEBUG_STREAM_THROTTLE(2.0, "UTM of latest GPS is (X,Y):"
                                       << utmX << " , " << utmY);

    // For now the 'nav' frame is that same as the 'base_link' frame
    transform_utm2nav_.setOrigin(tf2::Vector3(utmX, utmY,
                                              msg->pose.pose.position.z));
    transform_utm2nav_.setRotation(tf2::Quaternion(msg->pose.pose.orientation.x,
                                                   msg->pose.pose.orientation.y,
                                                   msg->pose.pose.orientation.z,
                                                   msg->pose.pose.orientation.w));
    transform_utm2nav_inverse_ = transform_utm2nav_.inverse();

    // Publish Nav/Base Odometry in UTM frame - note frames are set in ::run()
    nav_in_utm_.header.stamp = nav_update_time_;
    nav_in_utm_.header.seq++;
    // Create position information using transform.
    // Convert from transform to pose message
    // tf2::toMsg(transform_utm2nav_, nav_in_utm_.pose.pose);
    tf2::Vector3 tmp;
    tmp = transform_utm2nav_.getOrigin();
    nav_in_utm_.pose.pose.position.x = tmp[0];
    nav_in_utm_.pose.pose.position.y = tmp[1];
    nav_in_utm_.pose.pose.position.z = tmp[2];

    nav_in_utm_.pose.pose.position.z = (zero_altitude_ ? 0.0 : nav_in_utm_.pose.pose.position.z);
    // Create orientation information directy from incoming orientation
    nav_in_utm_.pose.pose.orientation = msg->pose.pose.orientation;
    nav_in_utm_.pose.covariance = msg->pose.covariance;
    // For twist - ignore the rotation since both are in the base_link/nav frame
    nav_in_utm_.twist.twist.linear = msg->twist.twist.linear;
    nav_in_utm_.twist.twist.angular = msg->twist.twist.angular;
    nav_in_utm_.twist.covariance = msg->twist.covariance;
    // Publish
    utm_pub_.publish(nav_in_utm_);

    // Calculate Nav in odom frame
    // Note the 'base' and 'nav' frames are the same for now
    // odom2base = odom2nav = odom2utm * utm2nav
    transform_odom2base_.mult(transform_utm2odom_inverse_, transform_utm2nav_);
    tf2::Transform transform_to_NED;
    tf2::Quaternion rotation_NED_quat = tf2::Quaternion::getIdentity();
    rotation_NED_quat.setRPY(M_PI, 0, M_PI/2);
    transform_to_NED.setRotation(rotation_NED_quat);
    transform_odom2base_ = transform_odom2base_ * transform_to_NED;


    ROS_DEBUG_STREAM_THROTTLE(2.0, "utm2nav X:"
                                       << transform_utm2nav_.getOrigin()[0]
                                       << "Y:" << transform_utm2nav_.getOrigin()[1]);
    ROS_DEBUG_STREAM_THROTTLE(2.0, "utm2odom X:"
                                       << transform_utm2odom_.getOrigin()[0]
                                       << "Y:" << transform_utm2odom_.getOrigin()[1]);
    ROS_DEBUG_STREAM_THROTTLE(2.0, "utm2odom_inverse X:"
                                       << transform_utm2odom_inverse_.getOrigin()[0]
                                       << "Y:"
                                       << transform_utm2odom_inverse_.getOrigin()[1]);
    ROS_DEBUG_STREAM_THROTTLE(2.0, "odom2base X:"
                                       << transform_odom2base_.getOrigin()[0]
                                       << "Y:" << transform_odom2base_.getOrigin()[1]);

    // Publish Nav odometry in odom frame - note frames are set in ::run()
    nav_in_odom_.header.stamp = nav_update_time_;
    nav_in_odom_.header.seq++;
    // Position from transform
    tf2::toMsg(transform_odom2base_, nav_in_odom_.pose.pose);
    nav_in_odom_.pose.pose.position.z = (zero_altitude_ ? 0.0 : nav_in_odom_.pose.pose.position.z);
    // Orientation and twist are uneffected
    // nav_in_odom_.pose.pose.orientation = msg->pose.pose.orientation;
    // nav_in_odom_.pose.covariance = msg->pose.covariance;
    // nav_in_odom_.twist.twist.linear = msg->twist.twist.linear;
    // nav_in_odom_.twist.twist.angular = msg->twist.twist.angular;
    // nav_in_odom_.twist.covariance = msg->twist.covariance;
    odom_pub_.publish(nav_in_odom_);
  } // navOdomCallback

  void GeonavTransform::geoOdomCallback(const nav_msgs::OdometryConstPtr &msg)
  {
    // Convert position from odometry frame to UTM
    // nav and base are same for now
    // utm2base = utm2nav = utm2odom * odom2nav
    transform_odom2nav_.setOrigin(tf2::Vector3(msg->pose.pose.position.x,
                                               msg->pose.pose.position.y,
                                               msg->pose.pose.position.z));
    transform_odom2nav_.setRotation(tf2::Quaternion(msg->pose.pose.orientation.x,
                                                    msg->pose.pose.orientation.y,
                                                    msg->pose.pose.orientation.z,
                                                    msg->pose.pose.orientation.w));
    transform_odom2nav_inverse_ = transform_odom2nav_.inverse();
    transform_utm2nav_.mult(transform_utm2odom_, transform_odom2nav_);

    // Convert from UTM to LL
    double lat;
    double lon;
    tf2::Vector3 tmp;
    tmp = transform_utm2nav_.getOrigin();
    NavsatConversions::UTMtoLL(tmp[1], tmp[0], utm_zone_,
                               lat, lon);

    nav_in_geo_.header.stamp = ros::Time::now();
    nav_in_geo_.pose.pose.position.x = lon;
    nav_in_geo_.pose.pose.position.y = lat;
    nav_in_geo_.pose.pose.position.z = 0.0;
    // Create orientation information directy from incoming orientation
    nav_in_geo_.pose.pose.orientation = msg->pose.pose.orientation;
    nav_in_geo_.pose.covariance = msg->pose.covariance;
    // For twist - ignore the rotation since both are in the base_link/nav frame
    nav_in_geo_.twist.twist.linear = msg->twist.twist.linear;
    nav_in_geo_.twist.twist.angular = msg->twist.twist.angular;
    nav_in_geo_.twist.covariance = msg->twist.covariance;
    // Publish
    geo_pub_.publish(nav_in_geo_);
  } // geoOdomCallback

} // namespace GeonavTransform
