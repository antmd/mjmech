// Copyright 2016 Josh Pieper, jjp@pobox.com.  All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "base/now.h"

#include <boost/test/auto_unit_test.hpp>

BOOST_AUTO_TEST_CASE(BasicNowTest) {
  boost::asio::io_service service;
  auto now = mjmech::base::Now(service);
  BOOST_CHECK(!now.is_not_a_date_time());
}
