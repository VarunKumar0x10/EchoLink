cmake_minimum_required(VERSION 3.10)
project(EchoLink)

set(CMAKE_CXX_STANDARD 17)

# Find ImGui from vcpkg
find_package(imgui CONFIG REQUIRED)

add_executable(EchoLink WIN32 main.cpp resources.rc)
target_include_directories(EchoLink PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Link Windows Audio Libraries and ImGui
target_link_libraries(EchoLink PRIVATE 
    imgui::imgui 
    Ole32 
    Mmdevapi 
    d3d11 
    d3dcompiler 
    dxgi
)