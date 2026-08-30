cmake_minimum_required(VERSION 3.21)

project(MeisoGlassInstalledCoreConsumer LANGUAGES C)

find_package(MeisoGlass 0.1 REQUIRED CONFIG COMPONENTS Core)

if(NOT TARGET Meiso::Core OR
   NOT TARGET Meiso::CoreResources OR
   NOT TARGET Meiso::CoreSession)
    message(FATAL_ERROR "Core component did not define its complete target set")
endif()
if(TARGET Meiso::Runtime OR
   TARGET Meiso::RuntimeScheduler OR
   TARGET Meiso::CompositionProfile OR
   TARGET Meiso::CompositionHostRuntime)
    message(FATAL_ERROR "Core component leaked a higher-layer target")
endif()

add_executable(meiso_installed_core_consumer main.c)
target_link_libraries(meiso_installed_core_consumer PRIVATE Meiso::Core)
set_property(TARGET meiso_installed_core_consumer PROPERTY C_STANDARD 11)

enable_testing()
add_test(NAME meiso_installed_core_consumer COMMAND meiso_installed_core_consumer)
