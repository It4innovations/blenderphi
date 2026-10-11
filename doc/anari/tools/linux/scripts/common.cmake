# Initial CMake cache shared by all components of the ANARI workspace on Linux
# (passed with `cmake -C`).

# Never read or write the user package registry in ~/.cmake/packages.
set(CMAKE_EXPORT_NO_PACKAGE_REGISTRY ON CACHE BOOL "")
set(CMAKE_EXPORT_PACKAGE_REGISTRY OFF CACHE BOOL "")
set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF CACHE BOOL "")
set(CMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY ON CACHE BOOL "")
set(CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY OFF CACHE BOOL "")

set(CMAKE_BUILD_TYPE Release CACHE STRING "")

# Libraries go to <prefix>/lib on every platform (GNUInstallDirs would pick lib64 on
# Rocky Linux), so that the layout is the one of the macOS workspace.
set(CMAKE_INSTALL_LIBDIR lib CACHE STRING "")

# CMake 4 rejects projects which ask for a CMake older than 3.5.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "")
