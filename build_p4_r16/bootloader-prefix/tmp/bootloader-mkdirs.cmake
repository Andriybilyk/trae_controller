# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "C:/esp/.espressif/v6.0/esp-idf/components/bootloader/subproject")
  file(MAKE_DIRECTORY "C:/esp/.espressif/v6.0/esp-idf/components/bootloader/subproject")
endif()
file(MAKE_DIRECTORY
  "C:/Users/andri/Documents/GitHub/trae_controller/build_p4_r16/bootloader"
  "C:/Users/andri/Documents/GitHub/trae_controller/build_p4_r16/bootloader-prefix"
  "C:/Users/andri/Documents/GitHub/trae_controller/build_p4_r16/bootloader-prefix/tmp"
  "C:/Users/andri/Documents/GitHub/trae_controller/build_p4_r16/bootloader-prefix/src/bootloader-stamp"
  "C:/Users/andri/Documents/GitHub/trae_controller/build_p4_r16/bootloader-prefix/src"
  "C:/Users/andri/Documents/GitHub/trae_controller/build_p4_r16/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "C:/Users/andri/Documents/GitHub/trae_controller/build_p4_r16/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "C:/Users/andri/Documents/GitHub/trae_controller/build_p4_r16/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
