cmake_minimum_required(VERSION 3.16)

project(MiniKV VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

function(minikv_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(minikv_core STATIC
    src/version.c
)

target_include_directories(minikv_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_definitions(minikv_core
    PRIVATE
        MINIKV_VERSION=\"${PROJECT_VERSION}\"
)

minikv_enable_warnings(minikv_core)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(MINIKV_SERVER_RUNTIME_SOURCE src/server_linux.c)
else()
    set(MINIKV_SERVER_RUNTIME_SOURCE src/server_unsupported.c)
endif()

add_library(minikv_server_runtime STATIC
    ${MINIKV_SERVER_RUNTIME_SOURCE}
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(minikv_server_runtime
        PRIVATE
            _GNU_SOURCE
    )
endif()

minikv_enable_warnings(minikv_server_runtime)

add_executable(minikv-server
    apps/minikv_server.c
)

target_include_directories(minikv-server
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(minikv-server
    PRIVATE
        minikv_core
        minikv_server_runtime
)

minikv_enable_warnings(minikv-server)

include(CTest)

if(BUILD_TESTING)
    add_executable(minikv_smoke
        tests/smoke_test.c
    )

    target_link_libraries(minikv_smoke
        PRIVATE
            minikv_core
    )

    target_compile_definitions(minikv_smoke
        PRIVATE
            MINIKV_EXPECTED_VERSION=\"${PROJECT_VERSION}\"
    )

    minikv_enable_warnings(minikv_smoke)

    add_test(
        NAME minikv.smoke
        COMMAND minikv_smoke
    )

    add_test(
        NAME minikv.server.help
        COMMAND minikv-server --help
    )

    set_tests_properties(minikv.server.help PROPERTIES
        PASS_REGULAR_EXPRESSION "Usage: minikv-server"
    )
endif()
