cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(echoear_player)

# Creates the SPIFFS image from the data folder (pre-recorded MP3 files)
spiffs_create_partition_image(storage ${CMAKE_CURRENT_SOURCE_DIR}/data FLASH_IN_PROJECT)

# Creates the FATFS image from the model folder
fatfs_create_partition_image(model ${CMAKE_CURRENT_SOURCE_DIR}/model FLASH_IN_PROJECT)
