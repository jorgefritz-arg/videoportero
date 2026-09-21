# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# include($ENV{ADF_PATH}/CMakeLists.txt)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

list(APPEND EXTRA_COMPONENT_DIRS "../common")

# GCC 14 (IDF 5.4) trata warnings nuevos de componentes externos como errores.
# Los degradamos a warnings para que el build no se corte por pedanteria del
# compilador en codigo de terceros (esp_video, gmf_core, etc.)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error" APPEND)

project(videopuerto)
