cmake_minimum_required(VERSION 3.20)
project(lightency VERSION 1.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if (MSVC)
    add_compile_options(/W4 /Zc:strictStrings- /permissive- /utf-8 /bigobj /EHsc)
    add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    add_compile_options($<$<CONFIG:Release>:/O2> $<$<CONFIG:Release>:/GL> $<$<CONFIG:Release>:/Gy> $<$<CONFIG:Release>:/Gw>)
    add_link_options($<$<CONFIG:Release>:/LTCG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
endif()

add_library(lightency_hook SHARED
    src/hook/lightency_hook.def
    src/hook/lightency_hook.rc
    src/hook/dllmain.cpp
    src/hook/xaml_bridge.cpp
    src/hook/dock_animation.cpp
    src/hook/dock_animation.h
    src/common/composition.cpp
    src/common/composition.h
    src/common/types.h
)

target_include_directories(lightency_hook PRIVATE
    src/hook
)

target_link_libraries(lightency_hook PRIVATE
    user32
    gdi32
    dwmapi
    comctl32
    ole32
    oleaut32
    runtimeobject
    shlwapi
    windowsapp
    advapi32
    msimg32
    dcomp
    shcore
    ntdll
    d2d1
    dxguid
)

add_executable(lightency WIN32
    src/app/lightency.rc
    src/app/main.cpp
    src/app/modern_gui.cpp
    src/app/modern_gui.h
    src/app/injector.cpp
    src/app/injector.h
    src/app/icon_gen.cpp
    src/app/icon_gen.h
    src/app/config.cpp
    src/app/config.h
    src/app/tray.cpp
    src/app/tray.h
    src/app/payload_manager.cpp
    src/app/payload_manager.h
    src/app/update_manager.cpp
    src/app/update_manager.h
    src/common/composition.cpp
    src/common/composition.h
    src/common/types.h
)

add_dependencies(lightency lightency_hook)

target_link_libraries(lightency PRIVATE
    user32
    gdi32
    gdiplus
    dwmapi
    shell32
    comctl32
    shcore
    ole32
    psapi
    advapi32
    msimg32
    dcomp
    ntdll
    winhttp
)
