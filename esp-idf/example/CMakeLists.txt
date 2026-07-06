# glint benchmark for ESP32 / ESP32-S3 (ESP-IDF).
#
#   cd esp-idf/example
#   ln -s ../../esp-idf components/glint   # or copy
#   idf.py set-target esp32   # or esp32s3
#   idf.py build flash monitor
#
# Prints frames, bytes, FNV checksum and x-realtime percent. The classic
# ESP32 has a single-precision FPU (doubles are still soft); the integer
# -q speed paths avoid per-coefficient float either way.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(glint_bench)
