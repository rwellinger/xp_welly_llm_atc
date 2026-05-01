# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/robertw/Workspace/x-plane/xp_welly_llm_atc/build-sanitize/espeak_ng/src/espeak_ng_external")
  file(MAKE_DIRECTORY "/Users/robertw/Workspace/x-plane/xp_welly_llm_atc/build-sanitize/espeak_ng/src/espeak_ng_external")
endif()
file(MAKE_DIRECTORY
  "/Users/robertw/Workspace/x-plane/xp_welly_llm_atc/build-sanitize/espeak_ng/src/espeak_ng_external-build"
  "/Users/robertw/Workspace/x-plane/xp_welly_llm_atc/build-sanitize/espeak_ng"
  "/Users/robertw/Workspace/x-plane/xp_welly_llm_atc/build-sanitize/espeak_ng/tmp"
  "/Users/robertw/Workspace/x-plane/xp_welly_llm_atc/build-sanitize/espeak_ng/src/espeak_ng_external-stamp"
  "/Users/robertw/Workspace/x-plane/xp_welly_llm_atc/build-sanitize/espeak_ng/src"
  "/Users/robertw/Workspace/x-plane/xp_welly_llm_atc/build-sanitize/espeak_ng/src/espeak_ng_external-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/robertw/Workspace/x-plane/xp_welly_llm_atc/build-sanitize/espeak_ng/src/espeak_ng_external-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/robertw/Workspace/x-plane/xp_welly_llm_atc/build-sanitize/espeak_ng/src/espeak_ng_external-stamp${cfgdir}") # cfgdir has leading slash
endif()
