file(GLOB_RECURSE U8G2_SOURCES "u8g2/csrc/*.c")

idf_component_register(
    SRCS ${U8G2_SOURCES}
         "hal/u8g2_esp32_hal.c"
    INCLUDE_DIRS "u8g2/csrc"
                 "hal"
    REQUIRES driver
)
