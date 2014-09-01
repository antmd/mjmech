// Copyright 2014 Josh Pieper, jjp@pobox.com.  All rights reserved.

#include <boost/test/auto_unit_test.hpp>

#include "quaternion.h"

namespace {
typedef imu::Quaternion<double> Quaternion;
typedef Quaternion::Vector3D Vector3D;
typedef Quaternion::Euler Euler;

void CheckVectorClose(const Quaternion::Vector3D& lhs,
                      double x, double y, double z) {
  BOOST_CHECK_SMALL(lhs.x() - x, 1e-2);
  BOOST_CHECK_SMALL(lhs.y() - y, 1e-2);
  BOOST_CHECK_SMALL(lhs.z() - z, 1e-2);
}
}

BOOST_AUTO_TEST_CASE(BasicQuaternion) {
  Vector3D v(10., 0., 0);

  v = Quaternion::FromEuler(0, 0, M_PI_2).Rotate(v);
  CheckVectorClose(v, 0, -10, 0);

  v = Quaternion::FromEuler(0, 0, -M_PI_2).Rotate(v);
  CheckVectorClose(v, 10, 0, 0);

  v = Quaternion::FromEuler(0, M_PI_2, 0).Rotate(v);
  CheckVectorClose(v, 10, 0, 0);

  v = Quaternion::FromEuler(M_PI_2, 0, 0).Rotate(v);
  CheckVectorClose(v, 0, 0, -10);

  v = Quaternion::FromEuler(0, 0, M_PI_2).Rotate(v);
  CheckVectorClose(v, 0, 0, -10);

  v = Quaternion::FromEuler(0, M_PI_2, 0).Rotate(v);
  CheckVectorClose(v, 0, 10, 0);

  v = Quaternion::FromEuler(M_PI_2, 0, 0).Rotate(v);
  CheckVectorClose(v, 0, 10, 0);

  v = Quaternion::FromEuler(0, 0, M_PI_2).Rotate(v);
  CheckVectorClose(v, 10, 0, 0);
}

namespace {
void CheckEuler(Euler euler,
                double roll_rad,
                double pitch_rad,
                double yaw_rad) {
  BOOST_CHECK_SMALL(euler.roll_rad - roll_rad, 1e-5);
  BOOST_CHECK_SMALL(euler.pitch_rad - pitch_rad, 1e-5);
  BOOST_CHECK_SMALL(euler.yaw_rad - yaw_rad, 1e-5);
}
}

BOOST_AUTO_TEST_CASE(QuaternionEulerAndBack) {
  struct TestCase {
    double roll_deg;
    double pitch_deg;
    double yaw_deg;
  };
  
  TestCase tests[] = {
    {45, 0, 0},
    {0, 45, 0},
    {0, 0, 45},
    {0, 90, 0},
    {0, 90, 20},
    {0, -90, 0},
    {0, -90, -10},
    {0, -90, 30},
    {10, 20, 30},
    {-30, 10, 20},
  };

  for (const auto& x: tests) {
    Euler result = Quaternion::FromEuler(
        x.roll_deg / 180 * M_PI,
        x.pitch_deg / 180 * M_PI,
        x.yaw_deg / 180 * M_PI).euler();
    CheckEuler(result,
               x.roll_deg / 180 * M_PI,
               x.pitch_deg / 180 *M_PI,
               x.yaw_deg / 180 * M_PI);
  }
}

BOOST_AUTO_TEST_CASE(QuaternionMultiply1) {
  Quaternion x90 = Quaternion::FromEuler(0, M_PI_2, 0);
  Quaternion xn90 = Quaternion::FromEuler(0, -M_PI_2, 0);
  Quaternion y90 = Quaternion::FromEuler(M_PI_2, 0, 0);

  Quaternion result = xn90 * y90 * x90;
  Vector3D vector(0, 1, 0);
  vector = result.Rotate(vector);
  CheckVectorClose(vector, 1, 0, 0);

  Quaternion initial = Quaternion::FromEuler(0, 0, 0.5 * M_PI_2);
  initial = Quaternion::FromEuler(0, 0, 0.5 * M_PI_2) * initial;
  CheckEuler(initial.euler(), 0, 0, M_PI_2);

  initial = Quaternion::FromEuler(0, 10 / 180. * M_PI, 0) * initial;
  vector = initial.Rotate(vector);
  CheckVectorClose(vector, 0, -0.9848078, -0.17364818);
  CheckEuler(initial.euler(), 10 / 180.0 * M_PI, 0, M_PI_2);
}
