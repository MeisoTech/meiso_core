cmake_minimum_required(VERSION 3.21)

project(MeisoGlass VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED YES)
set(CMAKE_C_EXTENSIONS NO)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

include(GNUInstallDirs)
include(CTest)

set(MEISO_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
file(MAKE_DIRECTORY "${MEISO_GENERATED_INCLUDE_DIR}/meiso")
configure_file(
    cmake/version.h.in
    "${MEISO_GENERATED_INCLUDE_DIR}/meiso/version.h"
    @ONLY
)

include(cmake/MeisoBuild.cmake)
include(cmake/MeisoCore.cmake)
include(cmake/MeisoRuntime.cmake)
include(cmake/MeisoProfiles.cmake)
include(cmake/MeisoComposition.cmake)
include(cmake/MeisoAdapters.cmake)
include(cmake/MeisoControl.cmake)
include(cmake/MeisoClient.cmake)
include(cmake/MeisoHost.cmake)
include(cmake/MeisoSimulation.cmake)
include(cmake/MeisoEdgeLinux.cmake)

option(
    MEISO_BUILD_APPLICATIONS
    "Build Meiso process entry points and evidence runners"
    ON
)
if(MEISO_BUILD_APPLICATIONS)
    include(cmake/MeisoApplications.cmake)
endif()

include(cmake/MeisoInstall.cmake)

if(BUILD_TESTING)
    include(cmake/MeisoTests.cmake)
endif()
