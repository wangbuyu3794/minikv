cmake_minimum_required(VERSION 3.16)

project(MiniKV VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

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

add_executable(minikv-server
    apps/minikv_server.c
)

target_link_libraries(minikv-server
    PRIVATE
        minikv_core
)

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
