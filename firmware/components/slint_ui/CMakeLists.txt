idf_component_register(
    SRCS "slint_ui_dummy.c"
    INCLUDE_DIRS "include"
)

set(SLINT_UI_DIR "${CMAKE_CURRENT_LIST_DIR}/../../slint_ui")
set(SLINT_UI_TARGET "xtensa-esp32s3-espidf")
set(SLINT_UI_LIB "${SLINT_UI_DIR}/target/${SLINT_UI_TARGET}/release/libslint_ui.a")

# Ensure Rust UI is rebuilt when sources/UI change.
file(GLOB_RECURSE SLINT_UI_RS "${SLINT_UI_DIR}/src/*.rs")
file(GLOB_RECURSE SLINT_UI_Slint "${SLINT_UI_DIR}/ui/*.slint")
set(SLINT_UI_DEPS
    "${SLINT_UI_DIR}/Cargo.toml"
    "${SLINT_UI_DIR}/Cargo.lock"
    "${SLINT_UI_DIR}/build.rs"
    ${SLINT_UI_RS}
    ${SLINT_UI_Slint}
)
set(CARGO_EXE "$ENV{USERPROFILE}/.cargo/bin/cargo.exe")
if (NOT EXISTS "${CARGO_EXE}")
    set(CARGO_EXE "cargo")
endif()
set(ESPUP_EXPORT "$ENV{USERPROFILE}/export-esp.ps1")
if (NOT EXISTS "${ESPUP_EXPORT}")
    set(ESPUP_EXPORT "")
endif()

if (ESPUP_EXPORT STREQUAL "")
    add_custom_command(
        OUTPUT ${SLINT_UI_LIB}
        COMMAND ${CMAKE_COMMAND} -E env
            "RUSTFLAGS=-Cdebuginfo=0 -Clink-arg=/PDB:NONE"
            "CARGO_PROFILE_RELEASE_DEBUG=0"
            "CARGO_PROFILE_RELEASE_BUILD_OVERRIDE_DEBUG=0"
            ${CARGO_EXE} build --release --target ${SLINT_UI_TARGET}
        DEPENDS ${SLINT_UI_DEPS}
        WORKING_DIRECTORY ${SLINT_UI_DIR}
        VERBATIM
    )
else()
    add_custom_command(
        OUTPUT ${SLINT_UI_LIB}
        COMMAND powershell -ExecutionPolicy Bypass -NoProfile -Command "& ${ESPUP_EXPORT}; $env:RUSTUP_TOOLCHAIN='esp'; $env:RUSTFLAGS='-Cdebuginfo=0 -Clink-arg=/PDB:NONE'; $env:CARGO_PROFILE_RELEASE_DEBUG='0'; $env:CARGO_PROFILE_RELEASE_BUILD_OVERRIDE_DEBUG='0'; & ${CARGO_EXE} build --release --target ${SLINT_UI_TARGET} -Z build-std=core,alloc,std"
        DEPENDS ${SLINT_UI_DEPS}
        WORKING_DIRECTORY ${SLINT_UI_DIR}
        VERBATIM
    )
endif()

add_custom_target(slint_ui_build DEPENDS ${SLINT_UI_LIB})
add_dependencies(${COMPONENT_LIB} slint_ui_build)

target_link_libraries(${COMPONENT_LIB} INTERFACE ${SLINT_UI_LIB})
