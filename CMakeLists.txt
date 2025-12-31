cmake_minimum_required(VERSION 3.20)
project(My3DSProject)
enable_language(C CXX ASM)

set(ROMFS_DIR "${CMAKE_SOURCE_DIR}/resources/romfs")
set(SOURCES
        src/main.c)

add_executable(${PROJECT_NAME} ${SOURCES})

target_link_libraries(${PROJECT_NAME} ctru citro2d citro3d m)
ctr_create_3dsx(${PROJECT_NAME} ${PROJECT_NAME}.3dsx ROMFS ${ROMFS_DIR})