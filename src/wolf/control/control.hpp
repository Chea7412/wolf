#pragma once

#include <state/data-structures.hpp>
#include <thread>
#include <helpers/logger.hpp>
#include <range/v3/view.hpp>

namespace control {

void run_control(immer::box<state::ControlSession> control_sess, int timeout_millis = 1000);

bool init();

} // namespace control