cmake_minimum_required(VERSION 3.20)
project(My3DSProject)
enable_language(C CXX ASM)

set(ROMFS_DIR "${CMAKE_SOURCE_DIR}/resources/romfs")
set(SOURCES
        src/vorbis.c)

find_package(PkgConfig REQUIRED)
pkg_check_modules(TREMOR QUIET vorbisidec)
if(NOT TREMOR_FOUND OR TREMOR_INCLUDE_DIRS STREQUAL "")
    message(WARNING "pkg-config could not find vorbisidec, falling back to default Tremor include path")
    set(TREMOR_INCLUDE_DIRS "/opt/devkitpro/portlibs/3ds/include")
    set(TREMOR_LIBRARIES "vorbisidec;ogg")
    set(TREMOR_CFLAGS_OTHER "")
endif()

add_executable(${PROJECT_NAME} ${SOURCES})

message("${TREMOR_INCLUDE_DIRS} || ${TREMOR_CFLAGS_OTHER} || ${TREMOR_LIBRARIES}")

target_include_directories(${PROJECT_NAME} PRIVATE ${TREMOR_INCLUDE_DIRS})
target_compile_options(${PROJECT_NAME} PRIVATE ${TREMOR_CFLAGS_OTHER})
target_link_libraries(${PROJECT_NAME} ${TREMOR_LIBRARIES})

target_link_libraries(${PROJECT_NAME} ctru citro2d citro3d m)
ctr_create_3dsx(${PROJECT_NAME} ${PROJECT_NAME}.3dsx ROMFS ${ROMFS_DIR})