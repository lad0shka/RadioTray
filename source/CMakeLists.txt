cmake_minimum_required(VERSION 3.16)
cmake_policy(SET CMP0091 NEW)
project(RadioTray CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8 /EHsc)
    add_compile_options($<$<CONFIG:Release>:/O1>)
    add_compile_options($<$<CONFIG:Release>:/Gy>)
    add_compile_options($<$<CONFIG:Release>:/Gw>)
    add_compile_options($<$<CONFIG:Release>:/GL>)
    add_link_options($<$<CONFIG:Release>:/LTCG>)
    add_link_options($<$<CONFIG:Release>:/OPT:REF>)
    add_link_options($<$<CONFIG:Release>:/OPT:ICF>)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(RadioTray WIN32
    main.cpp
    embedded_bass.cpp
    stream_metadata.cpp
    app.rc
    app.manifest
    app.ico
    resource.h
    version.h
    vendor/bass/bass.h
    embedded_bass.h
    stream_metadata.h
)

# Make sure the resource compiler re-runs (and the EXE relinks) when the
# embedded bass.dll or the icon changes.
set_property(SOURCE app.rc APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/bass/bass.dll"
    "${CMAKE_CURRENT_SOURCE_DIR}/app.ico")

# Lets rc.exe resolve "vendor/bass/bass.dll" from the RCDATA directive.
target_include_directories(RadioTray PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

set_target_properties(RadioTray PROPERTIES OUTPUT_NAME "RadioTray")

target_link_libraries(RadioTray PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/bass/bass.lib"
    ole32 oleaut32 shell32 user32 gdi32 advapi32 winhttp shlwapi
    delayimp bcrypt uuid
)

if(MSVC)
    # BASS is delay-loaded from the embedded cache at runtime via
    # __pfnDliNotifyHook2 (no /DELAY:UNLOAD, library stays loaded for life).
    target_link_options(RadioTray PRIVATE "/DELAYLOAD:bass.dll")
endif()

# No post-build bass.dll copy: the DLL ships inside the EXE as RCDATA 101.
