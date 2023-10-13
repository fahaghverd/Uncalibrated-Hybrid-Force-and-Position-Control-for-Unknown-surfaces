/*
 * wam_force_estimator.cpp
 *
 * 
 * 
 * In this script we use dynamic matrices drived for 4dof wam by Aritra Mitra et al., at this repo :
 * https://github.com/raj111samant/Model-based-control-of-4-DOF-Barrett-Wam/tree/master/WAM-C%2B%2B-Codes
 * 
 *  Created on: August, 2023
 *      Author: Faezeh
 */
#include <libconfig.h++>
#include <Dynamics.hpp>
#include <differentiator.hpp>
#include <force_estimator_4dof.hpp>
#include <wam_surface_Estimator.hpp>
#include <extended_ramp.hpp>
#include <get_tool_position_system.hpp>
#include <get_jacobian_system.hpp>
#include <robust_cartesian.h>
#include <extended_Tool_Orientation.hpp>
#include <unistd.h>
#include <iostream>
#include <string>
#include <barrett/units.h>
#include <barrett/systems.h>
#include <barrett/products/product_manager.h>
#include <barrett/detail/stl_utils.h>
#include <barrett/log.h>
#include <unistd.h>
#include <fcntl.h>
#include <barrett/standard_main_function.h>
#include <barrett/math.h>

#include <termios.h>
#include <unistd.h>
#include <fcntl.h>

#include <ros/ros.h>
#include <wam_force_estimation/RTCartForce.h>

// Function to generate waypoints along a cubic spline
std::vector<units::CartesianPosition::type> generateCubicSplineWaypoints(const units::CartesianPosition::type& initialPos, const units::CartesianPosition::type& finalPos, double waypointSpacing) {
    std::vector<units::CartesianPosition::type> waypoints;

    Eigen::Vector3d deltaPos = finalPos - initialPos;
    double totalDistance = deltaPos.norm();

    // Determine the number of waypoints
    int numWaypoints = static_cast<int>(totalDistance / waypointSpacing);
    if (numWaypoints < 2) {
        // Ensure at least two waypoints
        numWaypoints = 2;
        waypointSpacing = totalDistance / (numWaypoints - 1);
    }

    for (int i = 0; i < numWaypoints; ++i) {
        double t = static_cast<double>(i) / (numWaypoints - 1);
        Eigen::Vector3d waypoint = initialPos + t * deltaPos;
        waypoints.push_back(waypoint);
    }

    return waypoints;
}

template<size_t DOF>
int wam_main(int argc, char** argv, ProductManager& pm,	systems::Wam<DOF>& wam) {
    BARRETT_UNITS_TEMPLATE_TYPEDEFS(DOF);

	//Initialising ROS node and publishers
	ros::init(argc, argv, "force_estimator_node");
    ros::NodeHandle nh;
	ros::Publisher force_publisher = nh.advertise<wam_force_estimation::RTCartForce>("force_topic", 1);
	ros::Publisher force_avg_publisher = nh.advertise<wam_force_estimation::RTCartForce>("force_avg_topic", 1);

	//Setting up real-time command timeouts and initial values
	ros::Duration rt_msg_timeout;
    rt_msg_timeout.fromSec(0.2); //rt_status will be determined false if rt message is not received in specified time

	//temp file for logger
	char tmpFile[] = "/tmp/btXXXXXX";
	if (mkstemp(tmpFile) == -1) {
		printf("ERROR: Couldn't create temporary file!\n");
		return 1;
	}
	
	//Moving to start pose
	jp_type POS_READY;
	POS_READY << 0.002227924477643431, -0.1490540623980915, -0.04214558734519736, 1.6803055108189549;
	//POS_READY<< 0.0,1.05,0.0, 1.7;
	//wam.moveTo(POS_READY);

	//Adding gravity term and unholding joints
	wam.gravityCompensate();
	usleep(1500);

	// Set the differentiator mode indicating how many data points it uses
	int mode = 5; 
	ja_type zero_acc; 

	// Load configuration settings
	//libconfig::Config config;
	//config.readFile("inverse_dynamics_test.conf");		
	v_type drive_inertias;
	drive_inertias[0] = 11631e-8;
	drive_inertias[1] = 11831e-8;
	drive_inertias[2] = 11831e-8;
	drive_inertias[3] = 10686e-8; 
	libconfig::Setting& setting = pm.getConfig().lookup(pm.getWamDefaultConfigPath());

	//Instantiating systems
	typedef boost::tuple<double, cp_type, cf_type, cp_type> tuple_type;
	systems::GravityCompensator<DOF> gravityTerm(setting["gravity_compensation"]);
	getJacobian<DOF> getWAMJacobian;
	ForceEstimator<DOF> forceEstimator(true);
	Dynamics<DOF> wam4dofDynamics;
	systems::Constant<ja_type> zero(zero_acc);
	differentiator<DOF, jv_type, ja_type> diff(mode);
	ExtendedRamp time(pm.getExecutionManager(), 1.0);
	const LowLevelWam<DOF>& llw = wam.getLowLevelWam();
	systems::Gain<ja_type, sqm_type, jt_type> driveInertias(llw.getJointToMotorPositionTransform().transpose() * drive_inertias.asDiagonal() * llw.getJointToMotorPositionTransform());
	systems::TupleGrouper<double, cp_type, cf_type, cp_type> tg;
	systems::PrintToStream<cf_type> print(pm.getExecutionManager());


	//Adding surface estimator parts
	SurfaceEstimator<DOF> surface_estimator;
	ExtendedToolOrientation<DOF> rot;
	getToolPosition<DOF> cp;

	//Firstorder filter instead of diffrentiator
	double omega_p = 130.0;
	systems::FirstOrderFilter<jv_type> hp;
	hp.setHighPass(jv_type(omega_p), jv_type(omega_p));
	pm.getExecutionManager()->startManaging(hp);
	systems::Gain<jv_type, double, ja_type> changeUnits(1.0);

	//real-time data logger
	const size_t PERIOD_MULTIPLIER = 1;
	systems::PeriodicDataLogger<tuple_type> logger(
			pm.getExecutionManager(),
			new log::RealTimeWriter<tuple_type>(tmpFile, PERIOD_MULTIPLIER * pm.getExecutionManager()->getPeriod()),
			PERIOD_MULTIPLIER);

	
	//Connecting system potrs
	systems::connect(time.output, diff.time);
	systems::connect(time.output, tg.template getInput<0>());

	//systems::connect(wam.jvOutput, hp.input);
	//systems::connect(hp.output, changeUnits.input);
	//systems::connect(changeUnits.output, forceEstimator.jaInput);
	//systems::connect(changeUnits.output, driveInertias.input);	

	//systems::connect(wam.jvOutput, diff.inputSignal);
	//systems::connect(diff.outputSignal, forceEstimator.jaInput);
	//systems::connect(diff.outputSignal, driveInertias.input);

	systems::connect(zero.output, forceEstimator.jaInput);
	systems::connect(zero.output, driveInertias.input);

	systems::connect(wam.kinematicsBase.kinOutput, getWAMJacobian.kinInput);
	systems::connect(wam.kinematicsBase.kinOutput, gravityTerm.kinInput);

	systems::connect(getWAMJacobian.output, forceEstimator.Jacobian);
	systems::connect(driveInertias.output, forceEstimator.rotorInertiaEffect);
	systems::connect(gravityTerm.output, forceEstimator.g);
	systems::connect(wam.jtSum.output, forceEstimator.jtInput);
	
	systems::connect(wam.jpOutput, wam4dofDynamics.jpInputDynamics);
	systems::connect(wam.jvOutput, wam4dofDynamics.jvInputDynamics);
	systems::connect(wam4dofDynamics.MassMAtrixOutput, forceEstimator.M);
	systems::connect(wam4dofDynamics.CVectorOutput, forceEstimator.C);

	//systems::connect(forceEstimator.cartesianForceOutput, tg.template getInput<1>());	

	//Connecting input and outputs for surface estimator
	systems::connect(wam.kinematicsBase.kinOutput, rot.kinInput);
	systems::connect(wam.kinematicsBase.kinOutput, cp.kinInput);
	systems::connect(rot.output, surface_estimator.rotInput); //this does not work!
	systems::connect(cp.output, surface_estimator.cpInput);
	systems::connect(forceEstimator.cartesianForceOutput, surface_estimator.cfInput);
	systems::connect(surface_estimator.P1, tg.template getInput<1>());
	systems::connect(surface_estimator.P2, tg.template getInput<2>());
	systems::connect(surface_estimator.P3, tg.template getInput<3>());
	systems::connect(tg.output, logger.input);
	//Initialization Move when starting 
    //jp_type wam_init = wam.getHomePosition();
    //wam_init[3] -= .35;
    //wam.moveTo(wam_init); // Adjust the elbow, moving the end-effector out of the haptic boundary and hold position for haptic force initialization.

	// Change decrease our tool position controller gains slightly
    cp_type cp_kp, cp_kd;
    for (size_t i = 0; i < 3; i++)
    {
      cp_kp[i] = 1500;
      cp_kd[i] = 5.0;
    }
    wam.tpController.setKp(cp_kp);
    wam.tpController.setKd(cp_kd);
	
	//making spline from current cp to start cp
	cp_type start_pose;
	start_pose[0] = 0.554666;
	start_pose[1] =  0.019945;
	start_pose[2] =  -0.268530;
	std::vector<cp_type> waypoints = generateCubicSplineWaypoints(wam.getToolPosition(), start_pose,0.05);
	for (const cp_type& waypoint : waypoints) {
		//std::cout << "Waypoint: " << waypoint.transpose() << std::endl;
		//wam.moveTo(waypoint, true, 0.05);
	}
	


	// Reset and start the time counter
	{BARRETT_SCOPED_LOCK(pm.getExecutionManager()->getMutex());
	time.reset();
	time.start();}
	
	printf("Logging started.\n");

	// Set terminal input to non-blocking mode
    struct termios oldSettings, newSettings;
    tcgetattr(STDIN_FILENO, &oldSettings);
    newSettings = oldSettings;
    newSettings.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newSettings);
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);

	bool inputReceived = false;
    std::string lineInput2;
    std::cout << "Press [Enter] to stop." << std::endl;

	ros::Rate pub_rate(500);

	cp_type next_pose;
	next_pose<< 0.6560848991017444,0.019945,-0.208530522254321;
	std::vector<cp_type> waypoints2 = generateCubicSplineWaypoints(wam.getToolPosition(), next_pose,0.05);
	for (const cp_type& waypoint : waypoints2) {
		//std::cout << "Waypoint: " << waypoint.transpose() << std::endl;
		//wam.moveTo(waypoint, true, 0.05);
	}
	usleep(10);
	std::cout << "Estimated F:" <<forceEstimator.computedF << std::endl;
	std::cout<< surface_estimator.rotInput.getValue() << std::endl;

	int i = 0;
	cf_type cf_avg;	
	wam_force_estimation::RTCartForce force_msg;
	wam_force_estimation::RTCartForce force_avg_msg;
	btsleep(1);
	while (ros::ok() && pm.getSafetyModule()->getMode() == SafetyModule::ACTIVE){
		// Process pending ROS events and execute callbacks
		ros::spinOnce();

		char c;		
		if (read(STDIN_FILENO, &c, 1) > 0) {
            if (c == '\n') {
                inputReceived = true;
                break;
            }
        }
		
		//std::cout<< cf_avg << std::endl;

		i++;
		cf_avg = cf_avg + forceEstimator.computedF;
		//std::cout << "Estimated F:" <<forceEstimator.computedF << std::endl;
		force_msg.force[0] = forceEstimator.computedF[0];
		force_msg.force[1] = forceEstimator.computedF[1];
		force_msg.force[2] = forceEstimator.computedF[2];
		force_msg.force_norm = forceEstimator.computedF.norm()/9.81;
		//force_msg.force_dir = forceEstimator.computedF.normalize();
		force_msg.time = time.getYValue();
		force_publisher.publish(force_msg);
		
		if(i == 20){
			cf_avg = cf_avg/i;
			force_avg_msg.force[0] = cf_avg[0];
			force_avg_msg.force[1] = cf_avg[1];
			force_avg_msg.force[2] = cf_avg[2];
			force_avg_msg.time= time.getYValue();
			force_avg_msg.force_norm = cf_avg.norm()/9.81;
			//force_avg_msg.force_dir = cf_avg.normalize();
			force_avg_publisher.publish(force_avg_msg);
			i = 0;
			cf_avg<< 0.0, 0.0, 0.0;
			}
		
		pub_rate.sleep();
	}

		 
			
	//Restore terminal settings
    tcsetattr(STDIN_FILENO, TCSANOW, &oldSettings);
    fcntl(STDIN_FILENO, F_SETFL, 0);
	
	//Stop the time counter and disconnect controllers
	time.stop();
	wam.idle();
	
	logger.closeLog();
	printf("Logging stopped.\n");


	log::Reader<tuple_type> lr(tmpFile);
	lr.exportCSV(argv[1]);
	printf("Output written to %s.\n", argv[1]);
	std::remove(tmpFile);

	// Wait for user input before moving home
	barrett::detail::waitForEnter();
	wam.moveHome();
	return 0;
}