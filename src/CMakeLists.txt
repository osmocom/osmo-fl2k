# Copyright 2018 Osmocom Project
#
# This file is part of osmo-fl2k
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

include(GNUInstallDirs)

MACRO(LIBFL2K_APPEND_SRCS)
    LIST(APPEND libosmo-fl2k_srcs ${ARGV})
ENDMACRO(LIBFL2K_APPEND_SRCS)

LIBFL2K_APPEND_SRCS(
    libosmo-fl2k.c
)

########################################################################
# Set up Windows DLL resource files
########################################################################
IF(MSVC)
    include(${CMAKE_SOURCE_DIR}/cmake/Modules/Version.cmake)

    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/libosmo-fl2k.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/libosmo-fl2k.rc
    @ONLY)

    LIBFL2K_APPEND_SRCS(${CMAKE_CURRENT_BINARY_DIR}/libosmo-fl2k.rc)
ENDIF(MSVC)

########################################################################
# Setup shared library variant
########################################################################
add_library(libosmo-fl2k_shared SHARED ${libosmo-fl2k_srcs})
target_link_libraries(libosmo-fl2k_shared ${LIBUSB_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})
set_target_properties(libosmo-fl2k_shared PROPERTIES DEFINE_SYMBOL "libosmofl2k_EXPORTS")
set_target_properties(libosmo-fl2k_shared PROPERTIES OUTPUT_NAME osmo-fl2k)
set_target_properties(libosmo-fl2k_shared PROPERTIES SOVERSION ${MAJOR_VERSION})
set_target_properties(libosmo-fl2k_shared PROPERTIES VERSION ${LIBVER})

########################################################################
# Setup static library variant
########################################################################
add_library(libosmo-fl2k_static STATIC ${libosmo-fl2k_srcs})
target_link_libraries(libosmo-fl2k_static ${LIBUSB_LIBRARIES})
set_property(TARGET libosmo-fl2k_static APPEND PROPERTY COMPILE_DEFINITIONS "libosmofl2k_STATIC" )
if(NOT WIN32)
# Force same library filename for static and shared variants of the library
set_target_properties(libosmo-fl2k_static PROPERTIES OUTPUT_NAME osmo-fl2k)
endif()

########################################################################
# Setup libraries used in executables
########################################################################
if(WIN32 AND NOT MINGW)
add_library(libgetopt_static STATIC
    getopt/getopt.c
)
target_link_libraries(
    libosmo-fl2k_shared
)
endif()

########################################################################
# Build utility
########################################################################
add_executable(fl2k_file fl2k_file.c)
add_executable(fl2k_tcp fl2k_tcp.c)
add_executable(fl2k_test fl2k_test.c)
add_executable(fl2k_fm fl2k_fm.c rds_waveforms.c rds_mod.c)
set(INSTALL_TARGETS libosmo-fl2k_shared libosmo-fl2k_static fl2k_file fl2k_tcp fl2k_test fl2k_fm)

target_link_libraries(fl2k_file libosmo-fl2k_shared 
    ${LIBUSB_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)

target_link_libraries(fl2k_tcp libosmo-fl2k_shared 
    ${LIBUSB_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)

target_link_libraries(fl2k_test libosmo-fl2k_shared 
    ${LIBUSB_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)

target_link_libraries(fl2k_fm libosmo-fl2k_shared 
    ${LIBUSB_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)


if(UNIX)
target_link_libraries(fl2k_test m)
target_link_libraries(fl2k_fm m)
endif()

if(WIN32 AND NOT MINGW)
target_link_libraries(fl2k_file libgetopt_static)
target_link_libraries(fl2k_tcp ws2_32 libgetopt_static)
target_link_libraries(fl2k_test libgetopt_static)
target_link_libraries(fl2k_fm libgetopt_static)
set_property(TARGET fl2k_file APPEND PROPERTY COMPILE_DEFINITIONS "libosmo-fl2k_STATIC" )
set_property(TARGET fl2k_tcp APPEND PROPERTY COMPILE_DEFINITIONS "libosmo-fl2k_STATIC" )
set_property(TARGET fl2k_test APPEND PROPERTY COMPILE_DEFINITIONS "libosmo-fl2k_STATIC" )
set_property(TARGET fl2k_fm APPEND PROPERTY COMPILE_DEFINITIONS "libosmo-fl2k_STATIC" )
endif()

if(MINGW)
target_link_libraries(fl2k_tcp ws2_32)
endif()

########################################################################
# Install built library files & utilities
########################################################################
install(TARGETS ${INSTALL_TARGETS}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} # .so/.dylib file
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} # .lib file
    RUNTIME DESTINATION bin              # .dll file
)
