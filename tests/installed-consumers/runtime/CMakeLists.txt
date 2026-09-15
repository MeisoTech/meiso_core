cmake_minimum_required(VERSION 3.21)

project(MeisoGlassInstalledRuntimeConsumer LANGUAGES C)

find_package(MeisoGlass 0.1 REQUIRED CONFIG COMPONENTS Runtime)

if(NOT TARGET Meiso::Core OR
   NOT TARGET Meiso::Runtime)
    message(FATAL_ERROR "Runtime component did not load its Core dependency")
endif()
if(TARGET Meiso::WifiTelemetryProfile OR
   TARGET Meiso::CompositionProfile OR
   TARGET Meiso::CompositionHostRuntime OR
   TARGET Meiso::CompositionSdk)
    message(FATAL_ERROR "Runtime component leaked Profile or Host targets")
endif()

add_executable(meiso_installed_runtime_consumer main.c)
target_link_libraries(
    meiso_installed_runtime_consumer
    PRIVATE Meiso::Runtime
)
set_property(TARGET meiso_installed_runtime_consumer PROPERTY C_STANDARD 11)

enable_testing()
add_test(
    NAME meiso_installed_runtime_consumer
    COMMAND meiso_installed_runtime_consumer
)
