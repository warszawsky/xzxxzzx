cmake_minimum_required(VERSION 3.19)

project(geminik VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets Network)
set(CMAKE_AUTOMOC ON)

# Глобальные хоткеи (работают, даже когда фокус в другом приложении).
# Windows / macOS / X11. На Wayland не работают в принципе, см. README.
option(GEMINIK_GLOBAL_HOTKEYS "Глобальные хоткеи через QHotkey" ON)

if(GEMINIK_GLOBAL_HOTKEYS)
    include(FetchContent)
    set(QT_DEFAULT_MAJOR_VERSION 6 CACHE STRING "" FORCE)
    set(QHOTKEY_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(qhotkey
        GIT_REPOSITORY https://github.com/Skycoder42/QHotkey.git
        GIT_TAG        4e3a244d87f1f7e741e1395f2ffe825f3a8ada45)
    FetchContent_MakeAvailable(qhotkey)
endif()

qt_add_executable(geminik WIN32 MACOSX_BUNDLE
    src/main.cpp
    src/App.h               src/App.cpp
    src/Config.h            src/Config.cpp
    src/Persona.h
    src/GeminiClient.h      src/GeminiClient.cpp
    src/LocalTools.h        src/LocalTools.cpp
    src/HomeAssistant.h     src/HomeAssistant.cpp
    src/LocalBridge.h       src/LocalBridge.cpp
    src/ScreenCapture.h     src/ScreenCapture.cpp
    src/HotkeyService.h     src/HotkeyService.cpp
    src/PetWidget.h         src/PetWidget.cpp
    src/ChatWidget.h        src/ChatWidget.cpp
)

target_link_libraries(geminik PRIVATE Qt6::Widgets Qt6::Network)

if(GEMINIK_GLOBAL_HOTKEYS)
    target_link_libraries(geminik PRIVATE qhotkey)
    target_compile_definitions(geminik PRIVATE GEMINIK_HAVE_QHOTKEY)
endif()

# Исходники в UTF-8 (русские строки). Без этого MSVC превратит их в кракозябры.
if(MSVC)
    target_compile_options(geminik PRIVATE /utf-8 /W4)
else()
    target_compile_options(geminik PRIVATE -Wall -Wextra)
endif()

set_target_properties(geminik PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER dev.geminik.pet
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
