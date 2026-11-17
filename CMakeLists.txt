cmake_minimum_required(VERSION 3.16)

set(COMPONENTS main st7789 encoder led_strip)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(led-controller)
