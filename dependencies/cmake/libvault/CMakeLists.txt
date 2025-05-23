include(FetchContent)

set(LIBVAULT_SRC_URL "https://github.com/abedra/libvault/archive/refs/tags/0.61.0.tar.gz")

find_package(PkgConfig REQUIRED)
pkg_check_modules(libcurl REQUIRED IMPORTED_TARGET libcurl)

if (libcurl_FOUND)
    message(STATUS "Found libcurl ${libcurl_VERSION} in system using pkg-config.")
else ()
    message(FATAL_ERROR "libcurl in system is not found using pkg-config.")
endif()

add_library(libcurl INTERFACE)
target_link_libraries(libcurl INTERFACE PkgConfig::libcurl)
target_include_directories(libcurl INTERFACE ${libcurl_INCLUDE_DIRS})

if (BUILD_SHARED_LIBS)
    set(LIBCURL_BUILD_PRODUCTS ${libcurl_LIBRARY_DIRS}/libcurl.so CACHE STRING "Path to libcurl library" FORCE)
else ()
    set(LIBCURL_BUILD_PRODUCTS ${libcurl_LIBRARY_DIRS}/libcurl.a CACHE STRING "Path to libcurl library" FORCE)
endif ()

message(STATUS "Using libcurl from system at ${LIBCURL_BUILD_PRODUCTS}.")

FetchContent_Declare(libvault
        URL ${LIBVAULT_SRC_URL}
        URL_HASH SHA256=9718ec91157daeb59a47b77447328900d4b9020d0a370d06a2ec012c7017ff78
        INACTIVITY_TIMEOUT 5
        )

set(ENABLE_TEST OFF)
FetchContent_MakeAvailable(libvault)