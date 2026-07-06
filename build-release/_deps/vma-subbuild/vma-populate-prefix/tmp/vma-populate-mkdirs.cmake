# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/Robinsong/Dev/MiniRaytrace/build-release/_deps/vma-src")
  file(MAKE_DIRECTORY "/Users/Robinsong/Dev/MiniRaytrace/build-release/_deps/vma-src")
endif()
file(MAKE_DIRECTORY
  "/Users/Robinsong/Dev/MiniRaytrace/build-release/_deps/vma-build"
  "/Users/Robinsong/Dev/MiniRaytrace/build-release/_deps/vma-subbuild/vma-populate-prefix"
  "/Users/Robinsong/Dev/MiniRaytrace/build-release/_deps/vma-subbuild/vma-populate-prefix/tmp"
  "/Users/Robinsong/Dev/MiniRaytrace/build-release/_deps/vma-subbuild/vma-populate-prefix/src/vma-populate-stamp"
  "/Users/Robinsong/Dev/MiniRaytrace/build-release/_deps/vma-subbuild/vma-populate-prefix/src"
  "/Users/Robinsong/Dev/MiniRaytrace/build-release/_deps/vma-subbuild/vma-populate-prefix/src/vma-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/Robinsong/Dev/MiniRaytrace/build-release/_deps/vma-subbuild/vma-populate-prefix/src/vma-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/Robinsong/Dev/MiniRaytrace/build-release/_deps/vma-subbuild/vma-populate-prefix/src/vma-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
