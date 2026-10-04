# Compiles GLSL shaders to SPIR-V at build time.
#
# Prefers `glslc` (from shaderc / Vulkan SDK); falls back to `glslangValidator`.
#
# Usage:
#   pixelvr_compile_shaders(<target>
#       OUTPUT_DIR <dir>
#       SOURCES <shader1> <shader2> ...)
#
# Produces "<OUTPUT_DIR>/<name>.spv" for each source and a custom target that
# other targets can depend on.

find_program(PIXELVR_GLSLC glslc HINTS "$ENV{VULKAN_SDK}/bin")
find_program(PIXELVR_GLSLANG glslangValidator HINTS "$ENV{VULKAN_SDK}/bin")

if(NOT PIXELVR_GLSLC AND NOT PIXELVR_GLSLANG)
    message(FATAL_ERROR
        "No GLSL compiler found. Install 'glslc' (shaderc) or 'glslang-tools'.")
endif()

function(pixelvr_compile_shaders TARGET_NAME)
    cmake_parse_arguments(ARG "" "OUTPUT_DIR" "SOURCES" ${ARGN})

    if(NOT ARG_OUTPUT_DIR)
        message(FATAL_ERROR "pixelvr_compile_shaders: OUTPUT_DIR is required")
    endif()

    file(MAKE_DIRECTORY "${ARG_OUTPUT_DIR}")
    set(_spv_outputs "")

    foreach(_src ${ARG_SOURCES})
        get_filename_component(_name "${_src}" NAME)
        set(_out "${ARG_OUTPUT_DIR}/${_name}.spv")
        set(_abs "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")

        if(PIXELVR_GLSLC)
            add_custom_command(
                OUTPUT "${_out}"
                COMMAND "${PIXELVR_GLSLC}" -O --target-env=vulkan1.1
                        "${_abs}" -o "${_out}"
                DEPENDS "${_abs}"
                COMMENT "glslc ${_src}"
                VERBATIM)
        else()
            add_custom_command(
                OUTPUT "${_out}"
                COMMAND "${PIXELVR_GLSLANG}" -V --target-env vulkan1.1
                        "${_abs}" -o "${_out}"
                DEPENDS "${_abs}"
                COMMENT "glslangValidator ${_src}"
                VERBATIM)
        endif()

        list(APPEND _spv_outputs "${_out}")
    endforeach()

    add_custom_target(${TARGET_NAME} DEPENDS ${_spv_outputs})
endfunction()
