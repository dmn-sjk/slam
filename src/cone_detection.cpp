#include <ros/ros.h>
#include "sensor_msgs/PointCloud2.h"
#include <pcl_ros/point_cloud.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/filters/voxel_grid.h>
#include <math.h>
#include <pcl/search/kdtree.h>
#include <pcl/segmentation/extract_clusters.h>
#include <perception_handling/utils.hpp>
#include <perception_handling/color_classifier.hpp>
#include "cones_perception/ClassifyColorSrv.h"


class ConeDetector{
private:

	std::string frame_id = "cloud";
	// std::string input_cloud_topic = "/cloud";
	std::string input_cloud_topic = "/velodyne_points";
	std::string cones_topic = "/cones_cloud";
	std::string color_classifier_srv_name = "/color_classifier";

	float cone_width = 0.228;
	float cone_height = 0.325;
	float lidar_hor_res = 0.25; //degrees
	float lidar_ver_res = 7.5;

	// for our lidar
	// uint8_t distance_treshold_max = 12;
	// float distance_treshold_min = 0.7;
	// float level_threshold = -0.5;
	// uint8_t angle_threshold = 100;

	// uint8_t min_cluster_size = 3;
	// uint16_t max_cluster_size = 50;

	// for fsai rosbag
	uint8_t distance_treshold_max = 6;
	float distance_treshold_min = 1;
	float level_threshold = -0.09;
	uint8_t angle_threshold = 160;

	uint8_t min_cluster_size = 3;
	uint16_t max_cluster_size = 400;

	ros::NodeHandle nh;
    ros::Subscriber cloud_sub;
    ros::Publisher cones_pub;
	ros::ServiceClient color_srv_client;

	cones_perception::ClassifyColorSrv color_srv;

public:

	ConeDetector(): nh("~"){
		cloud_sub = nh.subscribe<sensor_msgs::PointCloud2>(input_cloud_topic, 2, &ConeDetector::cloud_handler, this);
		cones_pub = nh.advertise<sensor_msgs::PointCloud2>(cones_topic, 1);
		color_srv_client = nh.serviceClient<cones_perception::ClassifyColorSrv>(color_classifier_srv_name);
	
	}

	void cloud_handler(const sensor_msgs::PointCloud2ConstPtr &cloud_msg){

        pcl::PointCloud<pcl::PointXYZI>::Ptr input_cloud(new pcl::PointCloud <pcl::PointXYZI>);
		pcl::PointCloud<pcl::PointXYZI>::Ptr input_cloud_copy(new pcl::PointCloud <pcl::PointXYZI>);
        pcl::PointCloud<pcl::PointXYZI>::Ptr cloud_filtered(new pcl::PointCloud <pcl::PointXYZI>);

        pcl::fromROSMsg(*cloud_msg, *input_cloud);

		int cloud_size = input_cloud->points.size();
		
		pcl::copyPointCloud(*input_cloud, *input_cloud_copy);

		// TODO: make filter function return filtered cloud, so there will be no need to copy input
		// Deleting points too far away, too close and limiting horizontal FoV
		filter_points_position(input_cloud);
		
		//input_cloud->resize(cloud_size);

		cloud_filtered = downsample(input_cloud);

  		std::vector<pcl::PointIndices> cluster_indices;
		cluster_indices = euclidan_cluster(cloud_filtered);

	  	pcl::PointCloud<pcl::PointXYZI>::Ptr centroid_cloud (new pcl::PointCloud<pcl::PointXYZI>);
		
		centroid_cloud = get_centroid_cloud(input_cloud_copy, cloud_filtered, cluster_indices);

        sensor_msgs::PointCloud2 cones_cloud;
        
        pcl::toROSMsg(*centroid_cloud, cones_cloud);
		//pcl::toROSMsg(*input_cloud, cones_cloud);
        
		cones_cloud.header = cloud_msg->header;
		cones_cloud.fields = cloud_msg->fields;

        cones_pub.publish(cones_cloud);
	}

	void wait_till_color_classifier_ready() {
		color_srv_client.waitForExistence();
	}

	void filter_points_position(pcl::PointCloud<pcl::PointXYZI>::Ptr &cloud)
	{
		cloud->points.erase(std::remove_if(cloud->points.begin(), cloud->points.end(),
										   [&](pcl::PointXYZI p)
										   {
											   // level
											   return p.z < level_threshold or
													  // dist
													  perception_handling::euclidan_dist(p.x, p.y, p.z) > distance_treshold_max or
													  perception_handling::euclidan_dist(p.x, p.y, p.z) < distance_treshold_min or
													  // angle
													  -angle_threshold * M_PI / 180 >= atan2(p.y, p.x) or
													  atan2(p.y, p.x) >= angle_threshold * M_PI / 180;
										   }),
							cloud->points.end());
	}

	std::vector<pcl::PointIndices> euclidan_cluster(const pcl::PointCloud<pcl::PointXYZI>::ConstPtr &cloud){
		pcl::search::KdTree<pcl::PointXYZI>::Ptr kdtree (new pcl::search::KdTree<pcl::PointXYZI>);
  		kdtree->setInputCloud (cloud);

        std::vector<pcl::PointIndices> cluster_indices;
	  	pcl::EuclideanClusterExtraction<pcl::PointXYZI> ec;
		ec.setClusterTolerance(sqrt(pow(this->cone_height, 2) + pow(this->cone_width, 2)));
		ec.setMinClusterSize(min_cluster_size);
		ec.setMaxClusterSize(max_cluster_size);
		ec.setSearchMethod(kdtree);
		ec.setInputCloud(cloud);
		ec.extract(cluster_indices);

		return cluster_indices;
    }

	pcl::PointCloud<pcl::PointXYZI>::Ptr get_reconstructed_cone(const pcl::PointXYZI cone_center, const pcl::PointCloud<pcl::PointXYZI>::ConstPtr &cloud){
        pcl::PointCloud<pcl::PointXYZI>::Ptr reconstructed_cone(new pcl::PointCloud <pcl::PointXYZI>);
		pcl::PointXYZI p;

		for (std::vector<pcl::PointXYZI, Eigen::aligned_allocator<pcl::PointXYZI>>::const_iterator it = cloud->points.begin(); it != cloud->points.end(); it++) {
			if ((cone_center.x + (cone_width / 1.5) >= it->x && cone_center.x - (cone_width / 1.5) <= it->x) &&
				(cone_center.y + (cone_width / 1.5) >= it->y && cone_center.y - (cone_width / 1.5) <= it->y)) {
					p.x = it->x;
					p.y = it->y;
					p.z = it->z;
					p.intensity = it->intensity;
					reconstructed_cone->push_back(p);
			}
		}

        return reconstructed_cone;
    }

	pcl::PointCloud<pcl::PointXYZI>::Ptr downsample(const pcl::PointCloud<pcl::PointXYZI>::ConstPtr &cloud){
        pcl::PointCloud<pcl::PointXYZI>::Ptr cloud_filtered(new pcl::PointCloud <pcl::PointXYZI>);
        pcl::VoxelGrid<pcl::PointXYZI> vg;
        vg.setInputCloud(cloud);
        vg.setLeafSize(0.04f, 075.04f, 0.05f);
        vg.filter(*cloud_filtered);

        return cloud_filtered;
    }

	pcl::PointCloud<pcl::PointXYZI>::Ptr get_centroid_cloud(const pcl::PointCloud<pcl::PointXYZI>::ConstPtr &whole_cloud,
															const pcl::PointCloud<pcl::PointXYZI>::ConstPtr &filtered_cloud,
															std::vector<pcl::PointIndices> cluster_indices) {
		pcl::PointCloud<pcl::PointXYZI>::Ptr centroid_cloud (new pcl::PointCloud<pcl::PointXYZI>);
		//pcl::PointCloud<pcl::PointXYZI>::Ptr single_cone_cloud_filtered (new pcl::PointCloud<pcl::PointXYZI>);
		pcl::PointCloud<pcl::PointXYZI>::Ptr single_cone_cloud_reconstruct (new pcl::PointCloud<pcl::PointXYZI>);

        for (std::vector<pcl::PointIndices>::const_iterator it = cluster_indices.begin(); it != cluster_indices.end(); ++it)
		{
			pcl::PointXYZI p;
			int j = 0;
			float x, y = 0.0;
			for (const auto& idx : it->indices){
				//single_cone_cloud_filtered->push_back((*filtered_cloud)[idx]);
				x += (*filtered_cloud)[idx].x;
				y += (*filtered_cloud)[idx].y;
				j++;
			}

			p.x = x / j;
			p.y = y / j;
			p.z = 0.0;

			/* color classification */
			single_cone_cloud_reconstruct = get_reconstructed_cone(p, whole_cloud);
			p.intensity = perception_handling::colors_to_intensities[get_color(single_cone_cloud_reconstruct)];
			single_cone_cloud_reconstruct->clear();

			centroid_cloud->push_back(p);

			j = 0;
			x = 0.0;
			y = 0.0;
		}

		return centroid_cloud;
	}

	perception_handling::Color get_color(pcl::PointCloud<pcl::PointXYZI>::Ptr &single_cone_cloud) {
		perception_handling::Color color;
		sensor_msgs::PointCloud2 single_cone_msg;

        pcl::toROSMsg(*single_cone_cloud, single_cone_msg);

		single_cone_msg.header.frame_id = frame_id;

		color_srv.request.single_cone_cloud = single_cone_msg;
		
		if (color_srv_client.call(color_srv)) {
			color = static_cast<perception_handling::Color>(color_srv.response.color);
		} else {
			// unknown
			color = static_cast<perception_handling::Color>(0);
			ROS_ERROR("Failed to call service");
		}

		return color;
	}
};


int main(int argc, char* argv[])
{
	ros::init(argc, argv, "cone_detector");
  	ConeDetector detector;
	detector.wait_till_color_classifier_ready();
	ROS_INFO("Ready to detect cones.");
    ros::spin();

    return 0;
}