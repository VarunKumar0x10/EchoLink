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

set(IMGUI_DIR "../third_party/imgui")

set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

add_executable(echolink main.cpp ${IMGUI_SOURCES})

target_include_directories(echolink PRIVATE 
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${PULSE_INCLUDE_DIRS}
)

target_link_libraries(echolink PRIVATE
    glfw
    OpenGL::GL
    ${PULSE_LIBRARIES}
    Threads::Threads
)