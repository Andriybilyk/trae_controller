#include "config/board_profile.h"

#include "sdkconfig.h"

namespace board_profile {

BoardId current_board() {
#if CONFIG_TC_BOARD_NEW_P4
    return BoardId::NewP4;
#else
    return BoardId::LegacyS3;
#endif
}

const char *current_board_name() {
    switch (current_board()) {
        case BoardId::NewP4:
            return "new_p4";
        case BoardId::LegacyS3:
        default:
            return "legacy_s3";
    }
}

int display_width() {
    return CONFIG_TC_DISPLAY_WIDTH;
}

int display_height() {
    return CONFIG_TC_DISPLAY_HEIGHT;
}

} // namespace board_profile

