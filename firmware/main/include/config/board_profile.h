#pragma once

#include <stdint.h>

namespace board_profile {

enum class BoardId : uint8_t {
    LegacyS3 = 0,
    NewP4 = 1,
};

BoardId current_board();
const char *current_board_name();
int display_width();
int display_height();

} // namespace board_profile

