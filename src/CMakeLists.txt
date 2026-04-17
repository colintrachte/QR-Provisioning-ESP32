# Get all source files (Note: Explicitly listing files is safer, but GLOB works for small projects)
FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "."
    REQUIRES
        mdns
        joltwallet__littlefs    # This links the joltwallet/littlefs component
        QR-Code-generator      # This links the espressif/qrcode component
)
