# CTest `render.shader_blocks`, run as `cmake -DSHADER_DIR=<src/render/shaders> -P ShaderBlocks.cmake`: every uniform
# block is declared the same, member for member, in each shader that declares it. OpenGL links the stages of a program
# only when the blocks they share match (Vulkan, D3D and Metal do not check), so a vertex shader that declared only the
# prefix it reads left the preview black wherever Wabcut falls back to OpenGL (Linux without a Vulkan driver).

cmake_minimum_required(VERSION 3.28...4.3)

if(NOT IS_DIRECTORY "${SHADER_DIR}")
    message(FATAL_ERROR "ShaderBlocks.cmake: SHADER_DIR is not a directory: '${SHADER_DIR}'")
endif()

file(GLOB shaders "${SHADER_DIR}/*.vert" "${SHADER_DIR}/*.frag")
set(names "")
foreach(shader IN LISTS shaders)
    file(READ "${shader}" source)
    string(REPLACE ";" "," source "${source}") # a CMake list separator
    string(REGEX REPLACE "//[^\n]*" "" source "${source}")
    string(REGEX MATCHALL "uniform[ \t\r\n]+[A-Za-z0-9_]+[ \t\r\n]*{[^}]*}" blocks "${source}")
    get_filename_component(file_name "${shader}" NAME)
    foreach(block IN LISTS blocks)
        string(REGEX REPLACE "^uniform[ \t\r\n]+([A-Za-z0-9_]+).*" "\\1" name "${block}")
        string(REGEX REPLACE "^[^{]*{(.*)}$" "\\1" members "${block}")
        string(REGEX REPLACE "[ \t\r\n]+" " " members "${members}")
        string(STRIP "${members}" members)
        if(DEFINED block_${name})
            if(NOT block_${name} STREQUAL members)
                message(FATAL_ERROR "uniform block ${name} differs between ${first_${name}} and ${file_name}:\n"
                                    "  ${first_${name}}: ${block_${name}}\n  ${file_name}: ${members}")
            endif()
        else()
            set(block_${name} "${members}")
            set(first_${name} "${file_name}")
            list(APPEND names ${name})
        endif()
    endforeach()
endforeach()
list(LENGTH names count)
message(STATUS "${count} uniform blocks, each declared the same in every shader")
