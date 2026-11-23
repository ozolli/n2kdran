cmake_minimum_required(VERSION 3.18)
project(libn2k C)

# Bibliothèque partagée par le build Linux (tests, n2kdump) et le build Android (NDK).
add_library(n2k STATIC src/ydraw.c src/n2k.c src/decode.c src/polar.c src/derive.c src/ais.c)
target_include_directories(n2k PUBLIC include)
set_target_properties(n2k PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
                      POSITION_INDEPENDENT_CODE ON)
target_compile_options(n2k PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
target_link_libraries(n2k PUBLIC m)

if(NOT ANDROID)
    option(N2K_SANITIZE "ASan + UBSan pour les tests" ON)
    if(N2K_SANITIZE)
        target_compile_options(n2k PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer -g)
        target_link_options(n2k PUBLIC -fsanitize=address,undefined)
    endif()

    add_executable(n2kdump tools/n2kdump.c)
    target_link_libraries(n2kdump n2k)
    set_target_properties(n2kdump PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)

    add_executable(test_n2k tests/test_n2k.c)
    target_link_libraries(test_n2k n2k)
    set_target_properties(test_n2k PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)

    set(DATA ${CMAKE_CURRENT_SOURCE_DIR}/../tests/data)
    enable_testing()
    add_test(NAME unitaires COMMAND test_n2k ${DATA})
    add_test(NAME n2kdump_sim  COMMAND sh -c "$<TARGET_FILE:n2kdump> -r -i 0 < ${DATA}/sim-30s.ydraw > /dev/null")
    add_test(NAME n2kdump_reel COMMAND sh -c "$<TARGET_FILE:n2kdump> -r -i 0 < ${DATA}/reel-gps-ais-30s.ydraw > /dev/null")
endif()
