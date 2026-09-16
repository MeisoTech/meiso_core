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
if(MEISO_BUILD_LEGACY_COMPOSITION)
    include(cmake/MeisoEdgeLinux.cmake)
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

option(MEISO_BUILD_DRM_SCANOUT_RESEARCH
    "Build historical K230D DRM policy research and injected-op tests" OFF)
if(MEISO_BUILD_DRM_SCANOUT_RESEARCH)
    if(NOT UNIX)
        message(FATAL_ERROR "DRM scanout research requires a POSIX build")
    endif()
    add_subdirectory(research/k230d-drm)
elseif(MEISO_BUILD_K230D_DRM_TOPOLOGY_ADMISSION_RUNNER OR
       MEISO_BUILD_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RUNNER OR
       MEISO_BUILD_K230D_DRM_SINGLE_PLANE_TEST_ONLY_SUPERVISOR)
    message(FATAL_ERROR "DRM probe runners require MEISO_BUILD_DRM_SCANOUT_RESEARCH=ON")
endif()
