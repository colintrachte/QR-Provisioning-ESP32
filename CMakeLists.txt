cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(QR-Provisioning-ESP32)
# Add the boards directory to the component path
set(EXTRA_COMPONENT_DIRS "boards")
