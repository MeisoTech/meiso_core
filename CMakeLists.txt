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

option(MEISO_BUILD_LEGACY_COMPOSITION
    "Build historical Composition applications, scheduler and developer shells" OFF)

include(cmake/MeisoBuild.cmake)
include(cmake/MeisoCore.cmake)
include(cmake/MeisoRuntime.cmake)
include(cmake/MeisoProfiles.cmake)
if(MEISO_BUILD_LEGACY_COMPOSITION)
    include(cmake/MeisoComposition.cmake)
endif()
include(cmake/MeisoAdapters.cmake)
if(MEISO_BUILD_LEGACY_COMPOSITION)
    include(cmake/MeisoControl.cmake)
endif()
if(MEISO_BUILD_LEGACY_COMPOSITION)
    include(cmake/MeisoClient.cmake)
endif()
include(cmake/MeisoHost.cmake)
if(MEISO_BUILD_LEGACY_COMPOSITION)
    include(cmake/MeisoSimulation.cmake)
endif()

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

# The Edge (Display Ingress service, K230D target, DRM research) builds from meiso_edge
# against this source; see meiso_edge/cmake/MeisoEdgeCore.cmake.
