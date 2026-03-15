cmake_minimum_required(VERSION 3.16)
project(XYScope LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/release)

set(SOURCES xyscope.mm)

if(APPLE)
    # macOS: compile as Objective-C++ with Apple frameworks
    enable_language(OBJCXX)
    add_executable(xyscope ${SOURCES})
    target_compile_options(xyscope PRIVATE -fobjc-arc -O3 -Wall)

    execute_process(COMMAND brew --prefix
                    OUTPUT_VARIABLE HOMEBREW_PREFIX
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    target_include_directories(xyscope PRIVATE ${HOMEBREW_PREFIX}/include)
    target_link_directories(xyscope PRIVATE ${HOMEBREW_PREFIX}/lib)
    target_link_libraries(xyscope
        SDL2 SDL2_ttf pthread
        "-framework OpenGL" "-framework Accelerate"
        "-framework Foundation" "-framework CoreAudio" "-framework AudioToolbox")

elseif(WIN32)
    # Windows: WASAPI loopback + FFTW3, compile .mm as C++
    add_executable(xyscope ${SOURCES})
    set_source_files_properties(${SOURCES} PROPERTIES LANGUAGE CXX)

    if(MSVC)
        target_compile_options(xyscope PRIVATE /O2 /W3)
    else()
        target_compile_options(xyscope PRIVATE -O3 -Wall)
    endif()

    # SDL2 and SDL2_ttf (install via vcpkg: vcpkg install sdl2 sdl2-ttf fftw3)
    find_package(SDL2 CONFIG REQUIRED)
    find_package(SDL2_ttf CONFIG REQUIRED)

    find_package(FFTW3 CONFIG QUIET)
    if(FFTW3_FOUND)
        target_link_libraries(xyscope PRIVATE FFTW3::fftw3)
    else()
        find_path(FFTW3_INCLUDE fftw3.h)
        find_library(FFTW3_LIB NAMES fftw3 libfftw3)
        target_include_directories(xyscope PRIVATE ${FFTW3_INCLUDE})
        target_link_libraries(xyscope PRIVATE ${FFTW3_LIB})
    endif()

    target_link_libraries(xyscope PRIVATE
        SDL2::SDL2 SDL2::SDL2main
        $<TARGET_NAME_IF_EXISTS:SDL2_ttf::SDL2_ttf>
        opengl32 ole32)

else()
    # Linux: Pipewire + FFTW3, compile .mm as C++
    add_executable(xyscope ${SOURCES})
    set_source_files_properties(${SOURCES} PROPERTIES LANGUAGE CXX)
    target_compile_options(xyscope PRIVATE -O3 -Wall)

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    pkg_check_modules(SDL2_TTF REQUIRED IMPORTED_TARGET SDL2_ttf)

    find_library(FFTW3_LIB fftw3)

    target_link_libraries(xyscope
        PkgConfig::SDL2 PkgConfig::SDL2_TTF
        GL PkgConfig::PIPEWIRE ${FFTW3_LIB} pthread)
endif()
