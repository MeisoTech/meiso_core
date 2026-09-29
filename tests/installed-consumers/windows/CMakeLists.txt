cmake_minimum_required(VERSION 3.21)
project(MeisoWindowsInstalledConsumer LANGUAGES C CXX)
find_package(MeisoGlass 0.1 REQUIRED CONFIG COMPONENTS Windows)
add_executable(meiso_installed_windows_consumer main.c header_cpp17.cpp)
target_link_libraries(meiso_installed_windows_consumer PRIVATE
    Meiso::WindowsTcpAdapter Meiso::DisplayIngressMediaWindowsUdpAdapter)
set_target_properties(meiso_installed_windows_consumer PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)
if(MSVC)
    target_compile_options(meiso_installed_windows_consumer PRIVATE /W4 /WX)
endif()
enable_testing()
add_test(NAME meiso_installed_windows_consumer COMMAND meiso_installed_windows_consumer)
