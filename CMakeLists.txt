cmake_minimum_required(VERSION 3.20)
project(WiimoteMapper VERSION 1.1.x LANGUAGES CXX)
add_definitions(-DAPP_VERSION="${PROJECT_VERSION}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Source files
set(SOURCES
    main.cpp
    Connection.cpp
    InputNormalizer.cpp
    OutputRouter.cpp
    ConfigManager.cpp
)

# Headers
set(HEADERS
    Connection.h
    InputNormalizer.h
    OutputRouter.h
    ConfigManager.h
)

include(FetchContent)

# Fetch nlohmann/json
FetchContent_Declare(nlohmann_json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(nlohmann_json)

# Fetch Dear ImGui
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.90.4)
FetchContent_MakeAvailable(imgui)

set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)

add_executable(Wiimote WIN32 ${SOURCES} ${HEADERS} ${IMGUI_SOURCES} resource.rc)

target_include_directories(Wiimote PRIVATE 
    ${imgui_SOURCE_DIR} 
    ${imgui_SOURCE_DIR}/backends
)

# Link required Windows and DirectX libraries
target_link_libraries(Wiimote PRIVATE
    setupapi.lib
    hid.lib
    bthprops.lib
    user32.lib
    d3d11.lib
    d3dcompiler.lib
    nlohmann_json::nlohmann_json
)
