# spdlog's CMake insists on find_package(fmt CONFIG) when told to use an external fmt; the fmt::fmt
# target already exists here (added from the shadPS4 checkout), so just report it as found.
if(NOT TARGET fmt::fmt)
    message(FATAL_ERROR "fmt::fmt must be added before spdlog")
endif()
set(fmt_FOUND TRUE)
set(fmt_VERSION 12.1.0)
