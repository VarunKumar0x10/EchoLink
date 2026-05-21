cmake_minimum_required(VERSION 3.10)
project(EchoLink)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Assuming you moved the submodule to the root 'third_party' folder
set(IMGUI_DIR "../third_party/imgui")

set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)

# Removed resources.rc since we are generating the icon programmatically
add_executable(EchoLink WIN32 main.cpp ${IMGUI_SOURCES})

target_include_directories(EchoLink PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_link_libraries(EchoLink PRIVATE 
    Ole32 
    Mmdevapi 
    d3d11 
    d3dcompiler 
    dxgi
    dwmapi
    shell32
)