cmake_minimum_required(VERSION 3.21)
project(capsasr-fcitx VERSION 0.7.0 LANGUAGES CXX)

find_package(Fcitx5Core REQUIRED)
include("${FCITX_INSTALL_CMAKECONFIG_DIR}/Fcitx5Utils/Fcitx5CompilerSettings.cmake")

add_library(capsasrcommit SHARED capsasrcommit.cpp)
target_link_libraries(capsasrcommit PRIVATE Fcitx5::Core)
set_target_properties(capsasrcommit PROPERTIES PREFIX "")

configure_file(capsasrcommit.conf.in capsasrcommit.conf)
install(TARGETS capsasrcommit DESTINATION "${FCITX_INSTALL_LIBDIR}/fcitx5")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/capsasrcommit.conf"
        DESTINATION "${FCITX_INSTALL_PKGDATADIR}/addon")
