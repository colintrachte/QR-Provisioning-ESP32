# Get all source files (Note: Explicitly listing files is safer, but GLOB works for small projects)
FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "."
    EMBED_FILES
        "../data/index.html.gz"
        "../data/index.js.gz"
        "../data/index.css.gz"
        "../data/base.css.gz"
        "../data/settings.html.gz"
        "../data/settings.js.gz"
        "../data/settings.css.gz"
        "../data/favicon.svg.gz"
        "../data/setup.html.gz"
        "../data/setup.css.gz"
        "../data/setup.js.gz"
    REQUIRES
        mdns
        joltwallet__littlefs    # This links the joltwallet/littlefs component
        QR-Code-generator      # This links the espressif/qrcode component
)
