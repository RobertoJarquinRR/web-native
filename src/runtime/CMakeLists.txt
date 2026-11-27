cmake_minimum_required(VERSION 3.20)
project(webnative_r2 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(WN_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
get_filename_component(WN_WGPU_DIR "${WN_REPO_ROOT}/third_party/wgpu-native" ABSOLUTE)
set(WN_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated" CACHE PATH "directory with builtin_blob.h")

option(WN_WGPU "Build the wgpu-native renderer backend" ON)

if(WN_WGPU)
    set(WN_WGPU_DEFINE WN_WGPU=1)
else()
    set(WN_WGPU_DEFINE WN_WGPU=0)
endif()

set(WN_RUNTIME_SOURCES
    wn_common.c
    core/wn_core.c
    core/layout.c
    core/kind_ops.c
    core/paint.c
    core/input.c
    core/hit_test.c
    core/scroll.c
    document/wn_blob.c
    media/media_registry.c
    media/media_null.c
    media/media_core.c
    text/text_runtime.c
    text/text_atlas.c
    text/text_directwrite.cpp
    wn_view.c
    wn_app.c
    host_win32.c
    renderer_d3d11.c
)

if(WN_WGPU)
    list(APPEND WN_RUNTIME_SOURCES renderer_wgpu.c)
else()
    list(APPEND WN_RUNTIME_SOURCES renderer_wgpu_stub.c)
endif()

add_library(wn_runtime_obj OBJECT ${WN_RUNTIME_SOURCES})

target_include_directories(wn_runtime_obj PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${WN_REPO_ROOT}/include"
    "${WN_GENERATED_DIR}"
)

if(WN_WGPU)
    target_include_directories(wn_runtime_obj PUBLIC "${WN_WGPU_DIR}/include")
endif()

target_compile_definitions(wn_runtime_obj PUBLIC
    WN_STATIC
    ${WN_WGPU_DEFINE}
    $<$<BOOL:${MSVC}>:_CRT_SECURE_NO_WARNINGS>
)

if(MSVC)
    target_compile_options(wn_runtime_obj PRIVATE /W3)

    set_source_files_properties(renderer_d3d11.c PROPERTIES
        COMPILE_OPTIONS "/W4;/WX"
    )
endif()

set(WN_LINK_LIBS
    d3d11
    dxgi
    d3dcompiler
    windowscodecs
    ole32
    user32
    gdi32
    dwrite
    imm32
)

if(WN_WGPU)
    list(APPEND WN_LINK_LIBS "${WN_WGPU_DIR}/lib/wgpu_native.dll.lib")
endif()

add_library(webnative SHARED ${WN_RUNTIME_SOURCES})

target_include_directories(webnative PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${WN_REPO_ROOT}/include"
    "${WN_GENERATED_DIR}"
)

if(WN_WGPU)
    target_include_directories(webnative PRIVATE "${WN_WGPU_DIR}/include")
endif()

target_compile_definitions(webnative PRIVATE
    WN_BUILD_DLL
    ${WN_WGPU_DEFINE}
    $<$<BOOL:${MSVC}>:_CRT_SECURE_NO_WARNINGS>
)

if(MSVC)
    target_compile_options(webnative PRIVATE /W3)
endif()

set_target_properties(webnative PROPERTIES
    PREFIX ""
    OUTPUT_NAME "webnative"
)

target_link_libraries(webnative PRIVATE ${WN_LINK_LIBS})

add_executable(runtime_r2
    runtime_r2.c
)

add_executable(abi_smoke 
    examples/abi_smoke.c
)

add_executable(standalone
    examples/standalone.c
)

add_executable(embedded
    examples/embedded.c
)

add_executable(abi_header_check_c
    examples/abi_header_check.c
)

target_include_directories(abi_header_check_c PRIVATE "${WN_REPO_ROOT}/include")

target_compile_definitions(abi_header_check_c PRIVATE WN_STATIC)

add_executable(abi_header_check_cpp
    examples/abi_header_check.cpp
)

add_executable(wnui-dump
    tools/wn_dump.c
)

target_include_directories(wnui-dump PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${WN_REPO_ROOT}/include"
    "${WN_GENERATED_DIR}"
)

target_compile_definitions(wnui-dump PRIVATE WN_STATIC)

target_include_directories(abi_header_check_cpp PRIVATE "${WN_REPO_ROOT}/include")

target_compile_definitions(abi_header_check_cpp PRIVATE WN_STATIC)


target_link_libraries(runtime_r2 PRIVATE
    wn_runtime_obj
    ${WN_LINK_LIBS}
)

target_link_libraries(abi_smoke PRIVATE
    wn_runtime_obj
    ${WN_LINK_LIBS}
)

target_link_libraries(standalone PRIVATE
    wn_runtime_obj
    ${WN_LINK_LIBS}
)

target_link_libraries(embedded PRIVATE
    wn_runtime_obj
    ${WN_LINK_LIBS}
)

target_link_libraries(wnui-dump PRIVATE
    wn_runtime_obj
    ${WN_LINK_LIBS}
)

if(MSVC)
    foreach(target_name IN ITEMS runtime_r2 abi_smoke standalone embedded wnui-dump)
        target_compile_options(${target_name} PRIVATE /W3)
    endforeach()
endif()

if(MSVC)
    target_compile_options(abi_header_check_c PRIVATE /W4)
    target_compile_options(abi_header_check_cpp PRIVATE /W4)
    target_compile_options(wnui-dump PRIVATE /W4 /WX)
endif()


if(WN_WGPU)
    add_custom_target(copy_wgpu_dll ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WN_WGPU_DIR}/bin/wgpu_native.dll"
            "${CMAKE_CURRENT_BINARY_DIR}/wgpu_native.dll"
        COMMENT "copying wgpu_native.dll to build directory"
    )

    foreach(target_name IN ITEMS runtime_r2 abi_smoke standalone embedded webnative wnui-dump)
        add_dependencies(${target_name} copy_wgpu_dll)
    endforeach()
else()
    # Switching an existing build dir from ON to OFF must drop the copied DLL.
    add_custom_target(remove_wgpu_dll ALL
        COMMAND ${CMAKE_COMMAND} -E rm -f
            "${CMAKE_CURRENT_BINARY_DIR}/wgpu_native.dll"
        COMMENT "removing wgpu_native.dll (WN_WGPU=OFF)"
    )

    foreach(target_name IN ITEMS runtime_r2 abi_smoke standalone embedded webnative wnui-dump)
        add_dependencies(${target_name} remove_wgpu_dll)
    endforeach()
endif()

enable_testing()
add_test(NAME abi_header_check_c COMMAND abi_header_check_c)
add_test(NAME abi_header_check_cpp COMMAND abi_header_check_cpp)
add_test(NAME abi_smoke COMMAND abi_smoke)
add_test(NAME wnui_dump_help COMMAND wnui-dump --help)