cmake_minimum_required(VERSION 3.16)
project(yocto_rootfs_updater_raylib CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# raygui.h is in the standard sysroot include path via its own recipe.
find_package(raylib REQUIRED CONFIG)

add_executable(yocto-rootfs-updater-raylib main.cpp)

target_link_libraries(yocto-rootfs-updater-raylib PRIVATE
    raylib
    pthread dl m drm
)

install(TARGETS yocto-rootfs-updater-raylib RUNTIME DESTINATION bin)
