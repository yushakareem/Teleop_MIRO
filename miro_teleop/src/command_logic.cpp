/* Libraries */
#include "ros/ros.h"
#include "std_msgs/Bool.h"
#include "std_msgs/Float64.h"
#include "std_msgs/UInt8.h"
#include "geometry_msgs/Pose.h"
#include "geometry_msgs/PoseStamped.h"
#include "geometry_msgs/Pose2D.h"
#include "geometry_msgs/Vector3.h"
#include "miro_teleop/GestureProcessing.h"
#include "miro_teleop/SpatialReasoner.h"
#include "miro_teleop/PertinenceMapping.h"
#include "miro_teleop/MonteCarlo.h"
#include "miro_msgs/platform_control.h"
#include "rrtstar_msgs/rrtStarSRV.h"
#include "rrtstar_msgs/Region.h"
#include <iostream>
#include <cmath>
#include "miro_teleop/Path.h"
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>

/* Definitions */
#define RES 40 // Grid resolution
#define NZ 5 // Number of relations (north, south, west, east, distance-to)
#define HSIZE 400
#define VSIZE 400

/* Global variables */
std_msgs::UInt8 cmd; // Command tag received from interpreter
geometry_msgs::Pose2D obs, robot; // Obstacle and robot positions from mocap
geometry_msgs::Pose gesture; // Gesture information from mocap

/** 
 * Subscriber callback function.
 * Obtains command tag from Interpreter node. 
 */
void getCmd(const std_msgs::UInt8::ConstPtr& msg)
{
	cmd.data = msg->data;
	ROS_INFO("Command received from interpreter");
}

/** 
 * Subscriber callback function.
 * Obtains current robot pose from Motion Capture node. 
 */
void getRobotPose(const geometry_msgs::Pose2D::ConstPtr& groundpose)
{
	robot.x = 100*groundpose->x;
	robot.y = 100*groundpose->y;
	robot.theta = groundpose->theta;
}

/** 
 * Subscriber callback function.
 * Obtains current gesture pose from Motion Capture node. 
 */
void getGesture(const geometry_msgs::PoseStamped::ConstPtr& pose)
{
	gesture.orientation = pose->pose.orientation;
	gesture.position.x = 100*pose->pose.position.x;
	gesture.position.y = 100*pose->pose.position.y;
	gesture.position.z = 100*pose->pose.position.z;
}

/** 
 * Subscriber callback function.
 * Obtains obstacle pose from Motion Capture node. 
 */
void getObstaclePose(const geometry_msgs::Pose2D::ConstPtr& groundpose)
{
	obs.x = 100*groundpose->x;
	obs.y = 100*groundpose->y;
	obs.theta = groundpose->theta;
}

/** 
 * OpenCV Plot function.
 * Attaches matrix information to an img variable and displays it on screen. 
 */
void plot(const char* name, float matrix[][RES])
{
	cv::Mat img;
	cv::Mat map(RES, RES, CV_32F, matrix);
	map.convertTo(img, CV_8UC1);
	cv::namedWindow(name, cv::WINDOW_NORMAL);
	cv::imshow(name, img);
	cv::waitKey(0);
}

/**
 * Command Logic Node main function.
 * Calls services and set robot motion according to commands received.
 * 
 * In its initialization, the node calls the Spatial Reasoner service once to 
 * obtain all landscapes from the current workspace setting. Then, the execution
 * flow will depend on the command received from the Interpreter:
 *
 * If cmd = 1 (look), the following services are called in this order:
 * Gesture Processing - returns the target position
 * Pertinence Mapping - returns the mapped fuzzy landscape
 * Monte Carlo Simulation - computes the goal position
 * RRT* Path Planner - Generates optimal trajectory from robot position to goal
 * The trajectory obtained is published to the Robot Controller. 
 *
 * If cmd = 2 (go), the flag enable is set to 'true' and also sent to the 
 * Controller. In this moment, MIRO should move.
 *
 * If cmd = 3 (stop), the same flag is set to 'false' and sent to Controller.
 * The robot should stop.
 * 
 * If any other command tag is received, it is ignored.
 */
int main(int argc, char **argv)
{
	/* Definitions */
	geometry_msgs::Pose2D target, goal; // Target and goal positions
	miro_teleop::Path rrtPath; // Trajectory to be published
	std_msgs::Float64 matrices[NZ*RES*RES]; // From spatial reasoner
	std_msgs::Float64 landscape[RES*RES]; // From pertinence mapping
	std_msgs::Bool enable; // Controller enable flag
	miro_msgs::platform_control cmd_turn; // Turn command for "look"
	rrtstar_msgs::Region workspace, goal_reg, obs_reg; // For RRT* algorithm
	geometry_msgs::Vector3 init; // Initial position for the path planner
	double dtheta; // For turning command
	double pathsize; // Since RRT* trajectory size is variable
	int state = 0; // Control flag for the "look" command

	enable.data = false;

	/* Obstacle dimensions (predefined) */
	std_msgs::Float64 obsdim[2];
	obsdim[0].data = 80;
	obsdim[1].data = 80;

	/* Initialize and assign node handler */
	ros::init(argc, argv, "command_logic");
	ros::NodeHandle n;

	/* Initialize publishers and subscribers */
	// Publishers to robot controller
	ros::Publisher path_pub =
	n.advertise<miro_teleop::Path>("path", 1);
	ros::Publisher flag_pub =
	n.advertise<std_msgs::Bool>("enable", 1);
	// Publisher to miro
	ros::Publisher miro_pub = 
	n.advertise<miro_msgs::platform_control>
	("/miro/rob01/platform/control", 10);

	// Subscriber from command interpreter
	ros::Subscriber sub_cmd =
	n.subscribe("command", 3, getCmd);
	// Subscribers from motion capture (mocap)
	ros::Subscriber sub_robot =
	n.subscribe("Robot/ground_pose", 10, getRobotPose);
	ros::Subscriber sub_gesture =
	n.subscribe("Gesture/pose", 1, getGesture);
	ros::Subscriber sub_obs =
	n.subscribe("Obstacle/ground_pose", 1, getObstaclePose);

	/* Initialize service clients and handlers */
	ros::ServiceClient cli_spat =
	n.serviceClient<miro_teleop::SpatialReasoner>("spatial_reasoner");
	miro_teleop::SpatialReasoner srv_spat;

	ros::ServiceClient cli_gest =
	n.serviceClient<miro_teleop::GestureProcessing>("gesture_processing");
	miro_teleop::GestureProcessing srv_gest;

	ros::ServiceClient cli_pert =
	n.serviceClient<miro_teleop::PertinenceMapping>("pertinence_mapper");
	miro_teleop::PertinenceMapping srv_pert;

	ros::ServiceClient cli_mont =
	n.serviceClient<miro_teleop::MonteCarlo>("monte_carlo");
	miro_teleop::MonteCarlo srv_mont;

	ros::ServiceClient cli_rrts =
	n.serviceClient<rrtstar_msgs::rrtStarSRV>("rrtStarService");
	rrtstar_msgs::rrtStarSRV srv_rrts;

	/* Update rate (period) */
	ros::Rate loop_rate(10);

	/* Characterize workspace region (predefined) */
	workspace.center_x = 0;
	workspace.center_y = 0;
	workspace.center_z = 0;
	workspace.size_x = HSIZE;
	workspace.size_y = VSIZE;
	workspace.size_z = 0;

	srv_rrts.request.WS = workspace; // RRT* request member

	/* Assuming static objects, assign the obstacle region only once */
	obs_reg.center_x = obs.x;
	obs_reg.center_y = obs.y;
	obs_reg.center_z = 0;
	obs_reg.size_x = obsdim[0].data;
	obs_reg.size_y = obsdim[1].data;
	obs_reg.size_z = 0;

	srv_rrts.request.Obstacles.push_back(obs_reg); // RRT* request member

	/* Initialization */

	ROS_INFO("Command logic (master) node active");
	ROS_INFO("Initialization: calling spatial reasoner");

	// Set the matrices, by calling spatial reasoner
	srv_spat.request.center = obs;
	srv_spat.request.dimensions.push_back(obsdim[0]);
	srv_spat.request.dimensions.push_back(obsdim[1]);

	if (cli_spat.call(srv_spat))
	{
		// Matrices to be plotted by opencv
		float   spmat0[RES][RES], spmat1[RES][RES],
			spmat2[RES][RES], spmat3[RES][RES],
			spmat4[RES][RES];

		for (int i=0;i<NZ*RES*RES;i++){
			matrices[i].data = srv_spat.response.matrices[i].data;
			int depth=i/(RES*RES);
			int l_ind = i%(RES*RES);
			if(depth==0)
			spmat0[l_ind/RES][l_ind%RES]=matrices[i].data*255;
			if(depth==1)
			spmat1[l_ind/RES][l_ind%RES]=matrices[i].data*255;
			if(depth==2)
			spmat2[l_ind/RES][l_ind%RES]=matrices[i].data*255;
			if(depth==3)
			spmat3[l_ind/RES][l_ind%RES]=matrices[i].data*255;
			if(depth==4)
			spmat4[l_ind/RES][l_ind%RES]=matrices[i].data*255;
		}
		
		// Display landscapes (requires opencv package)
		plot("North", spmat0);
		plot("West" , spmat1);
		plot("South", spmat2);
		plot("East",  spmat3);
		plot("Distance", spmat4);

		ROS_INFO("Environment landscapes generated succesfully");
	}
	else
	{
		ROS_ERROR("Failed to call spatial reasoner");
		return 1;
	}

	/* Main loop */
	while(ros::ok())
	{
		/* Command: look */
		if(cmd.data==1)
		{
			state = 0;

			// First, call gesture processing service
			ROS_INFO("Calling Gesture Processing service");
			ROS_INFO("Gesture x: %f", gesture.position.x);
			srv_gest.request.gesture = gesture;
			
			if (cli_gest.call(srv_gest))
			{
				target = srv_gest.response.target;
				// Verify if target is valid number
				if(std::isfinite(target.x) &&
					std::isfinite(target.y))
				{
					ROS_INFO("Target obtained: (%f,%f)",
					target.x, target.y);
					state = 1;
				}
				else
				ROS_INFO("Invalid target: please try again");
				// Verify bound conditions
				if(target.x < -HSIZE/2 || target.x > HSIZE/2 ||
				   target.y < -VSIZE/2 || target.y > VSIZE/2)
				{
					ROS_INFO("Target out of the bounds");
					state = 0;
				}
			}
			else
			{
				ROS_ERROR("Failed to call Gesture Processing");
				return 1;
			}

			// Then, call pertinence mapping service
			if(state==1)
			{
			ROS_INFO("Calling Pertinence Mapping service");
			srv_pert.request.target = target;
			for (int i=0;i<RES*RES*NZ;i++)
			srv_pert.request.matrices.push_back(matrices[i]);

			if (cli_pert.call(srv_pert))
			{
				float pertmatrix[RES][RES];
				for (int i=0;i<RES*RES;i++) 
				{
					landscape[i].data = 
					srv_pert.response.landscape[i].data;
					//For OpenCV plot
					pertmatrix[i/RES][i%RES] =
					srv_pert.response.landscape[i].data*255;
				}

				// Verify whether the output is valid
				if(!std::isfinite(landscape[0].data))
				{
					state = 0;
					ROS_INFO("Invalid pertinence mapping");
				}
				else
				{
					state = 2;
					ROS_INFO("Landscapes mapped");
					// Plot using opencv
					plot("Mapped landscape", pertmatrix);	
				}
			}
			else
			{
				ROS_ERROR("Failed to call Pertinence Mapping");
				return 1;
			}
			srv_pert.request.matrices.clear();
			}

			// After, call monte carlo service
			if(state==2)
			{
			ROS_INFO("Calling Monte Carlo Simulation service");
			srv_mont.request.P = target;
			for (int i=0;i<RES*RES;i++)
			srv_mont.request.landscape.push_back(landscape[i]);

			if (cli_mont.call(srv_mont))
			{
				goal = srv_mont.response.goal;
				// Verify if goal returned is valid
				if(goal.x<-HSIZE/2 || goal.x>HSIZE/2
				|| goal.y<-VSIZE/2 || goal.y>VSIZE/2)
				{
					ROS_INFO("Invalid goal position");
					state = 0;
				}
				else
				{
					ROS_INFO("Goal obtained: (%f,%f)",
					goal.x, goal.y);
					state = 3;
				}
			}
			else
			{
				ROS_ERROR("Failed to call Monte Carlo service");
				return 1;
			}
			srv_mont.request.landscape.clear();
			}

			// Finally, call RRT* server and publish path
			if(state==3)
			{
			ROS_INFO("Calling RRT* Path Planner service");

			// Initial position is robot current one
			init.x = robot.x;
			init.y = robot.y;
			init.z = 0;

			// Define goal region
			goal_reg.center_x = goal.x; //TEST
			goal_reg.center_y = goal.y; //TEST
			goal_reg.center_z = 0;
			goal_reg.size_x = 20;
			goal_reg.size_y = 20;
			goal_reg.size_z = 0;

			// Note: workscape and object regions already defined
			srv_rrts.request.Goal = goal_reg;
			srv_rrts.request.Init = init;

			if(cli_rrts.call(srv_rrts))
			{
				ROS_INFO("Path found: Publishing...");
				pathsize = srv_rrts.response.path.size();
				// Obtain trajectory point-by-point
				geometry_msgs::Vector3 point;
				rrtPath.path.clear();
				for(int i=0; i<pathsize; i++)
				{
					point.x = srv_rrts.response.path[i].x;
					point.y = srv_rrts.response.path[i].y;
					point.z = srv_rrts.response.path[i].z;
					// Only x and y coordinates matter
					rrtPath.path.push_back(point);
					ROS_INFO("Point %d: (%f,%f)",
					i,point.x, point.y);
				}
				path_pub.publish(rrtPath);
				state = 4;
			}
			else
			{
				ROS_ERROR("Failed to call RRT* Path Planner");
				return 1;
			}
			}

			// If everything went well, miro turns itself to goal
			// (only if it is not moving)
			if(state==4)
			{
				ROS_INFO("Look, MiRo!");
				dtheta = atan2(goal.y-robot.y,goal.x-robot.x) 
								- robot.theta;
				dtheta = atan2(sin(dtheta),cos(dtheta));
				cmd_turn.body_move.theta = dtheta;
				miro_pub.publish(cmd_turn);
			}

			// Reset command
			cmd.data = 0;
		}

		/* Command: go */
		if(cmd.data==2)
		{
			// Enable robot control
			enable.data = true;
			flag_pub.publish(enable);

			// Reset command
			cmd.data = 0;
		}

		/* Command: stop */
		if(cmd.data==3)
		{
			// Disable robot control
			enable.data = false;
			flag_pub.publish(enable);

			// Reset command
			cmd.data = 0;
		}

		/* Spin and wait for next period */
		ros::spinOnce();
		loop_rate.sleep();

	}

	return 0;

}
