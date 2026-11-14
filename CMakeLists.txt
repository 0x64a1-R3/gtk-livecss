cmake_minimum_required(VERSION 3.25)
project(gtk_livecss
        VERSION 1.0.0
        DESCRIPTION "Universal LD_PRELOAD CSS hot-reloader for GTK3, GTK4, and Libadwaita"
        LANGUAGES CXX
)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Zero link-time GTK/GLib dependencies; symbols are resolved lazily via RTLD_NOLOAD.
add_library(gtk_livecss SHARED src/main.cpp)

target_compile_options(gtk_livecss PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -fvisibility=hidden
)

target_link_libraries(gtk_livecss PRIVATE ${CMAKE_DL_LIBS})

set_target_properties(gtk_livecss PROPERTIES
        PREFIX "lib"
        SUFFIX ".so"
)

install(TARGETS gtk_livecss
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)