add_library(usermod_quickjs INTERFACE)

target_sources(usermod_quickjs INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/modquickjs.c
    ${CMAKE_CURRENT_LIST_DIR}/src/quickjs.c
    ${CMAKE_CURRENT_LIST_DIR}/src/libregexp.c
    ${CMAKE_CURRENT_LIST_DIR}/src/libunicode.c
    ${CMAKE_CURRENT_LIST_DIR}/src/dtoa.c
)

target_include_directories(usermod_quickjs INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/src
)

target_compile_definitions(usermod_quickjs INTERFACE
    -D_GNU_SOURCE
)

# Suppress strict GCC 14 warnings
target_compile_options(usermod_quickjs INTERFACE
    -Wno-error
    -Wno-incompatible-pointer-types
    -Wno-format
    -Wno-unused-function
    -Wno-maybe-uninitialized
    -Wno-sign-compare
    -Wno-unused-variable
    -Wno-unused-parameter
    -Wno-implicit-fallthrough
)

target_link_libraries(usermod INTERFACE usermod_quickjs)
