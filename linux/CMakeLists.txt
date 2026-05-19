cmake_minimum_required(VERSION 3.10)
project(EchoLink LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(PULSE REQUIRED libpulse-simple libpulse)
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

set(IMGUI_SOURCES
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/backends/imgui_impl_glfw.cpp
    imgui/backends/imgui_impl_opengl3.cpp
)

add_executable(echolink main.cpp ${IMGUI_SOURCES})

target_include_directories(echolink PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends
    ${PULSE_INCLUDE_DIRS}
)

target_link_libraries(echolink PRIVATE
    glfw
    OpenGL::GL
    ${PULSE_LIBRARIES}
    Threads::Threads
)