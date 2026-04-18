/**
 * boards/board.h — Board selector.
 *
 * Include this header (not a specific board file) in config.h and anywhere
 * board constants are needed. The correct board file is pulled in based on
 * the -DBOARD_* flag set in platformio.ini build_flags.
 *
 * Adding a new board:
 *   1. Create boards/<board_name>.h with all required constants.
 *   2. Add an #elif branch here.
 *   3. Add a [env:<name>] in platformio.ini with -DBOARD_<NAME> in build_flags.
 */
#pragma once

#if defined(BOARD_HELTEC_LORA32_V3)
#  include "heltec_wifi_lora_32_v3.h"
#elif defined(BOARD_TTGO_LORA32_V1)
#  include "ttgo_lora32_v1.h"
#else
#  error "No board defined. Add -DBOARD_HELTEC_LORA32_V3 or -DBOARD_TTGO_LORA32_V1 \
to build_flags in platformio.ini, or create a new board header."
#endif
