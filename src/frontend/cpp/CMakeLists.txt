cmake_minimum_required(VERSION 3.20)
project(webnative_frontend_cpp LANGUAGES C CXX)

# build.ps1 (cl.exe + vcvars64) is the canonical Windows build.  This file is
# provided for IDE/CMake users; it produces build-cmake/frontend-cpp.exe.
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB GUMBO_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/gumbo/src/*.c")
file(GLOB APP_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_executable(frontend-cpp ${APP_SOURCES} ${GUMBO_SOURCES})
target_include_directories(frontend-cpp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/gumbo/compat"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/gumbo/src")
target_compile_definitions(frontend-cpp PRIVATE NDEBUG)

if(MSVC)
    target_compile_options(frontend-cpp PRIVATE
        /O2 /MT
        $<$<COMPILE_LANGUAGE:CXX>:/std:c++20;/EHsc;/W4>
        $<$<COMPILE_LANGUAGE:C>:/W0>)
endif()

set_target_properties(frontend-cpp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
