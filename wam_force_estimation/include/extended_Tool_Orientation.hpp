#pragma once

#include <iostream>

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <gsl/gsl_matrix.h>

#include <barrett/detail/ca_macro.h>
#include <barrett/units.h>
#include <barrett/systems/abstract/system.h>
#include <barrett/systems/abstract/single_io.h>
#include <barrett/systems/kinematics_base.h>


namespace barrett {
namespace systems {


// yields a quaternion representing the rotation from the world frame to the tool frame.
template<size_t DOF>
class ExtendedToolOrientation : public System, public KinematicsInput<DOF>,
						        public SingleOutput<math::Matrix<3,3>> {
public:
	ExtendedToolOrientation(const std::string& sysName = "ExtendedToolOrientation") :
		System(sysName), KinematicsInput<DOF>(this),
		SingleOutput<math::Matrix<3,3>>(this), rot() {}
	virtual ~ExtendedToolOrientation() { mandatoryCleanUp(); }

protected:
	virtual void operate() {
		rot.copyFrom(this->kinInput.getValue().impl->tool->rot_to_world);
		rot = rot.transpose(); // Transpose to get world-to-tool rotation

		this->outputValue->setData(&rot);
	}

	math::Matrix<3,3> rot;

private:
	DISALLOW_COPY_AND_ASSIGN(ExtendedToolOrientation);

public:
	EIGEN_MAKE_ALIGNED_OPERATOR_NEW
};


}
}
