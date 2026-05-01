# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

if(EXISTS "/Users/robertw/Workspace/x-plane/xp_welly_llm_atc/build-sanitize/espeak_ng/src/espeak_ng_external-stamp/espeak_ng_external-gitclone-lastrun.txt" AND EXISTS "/Users/robertw/Workspace/x-plane/xp_welly_llm_atc/build-sanitize/espeak_ng/src/espeak_ng_external-stamp/espeak_ng_external-gitinfo.txt" AND
  "/Users/robertw/Workspace/x-plane/xp_welly_llm_atc/build-sanitize/espeak_ng/src/espeak_ng_external-stamp/espeak_ng_external-gitclone-lastrun.txt" IS_NEWER_THAN "/Users/robertw/Workspace/x-plane/xp_welly_llm_atc/build-sanitize/espeak_ng/src/espeak_ng_external-stamp/espeak_ng_external-gitinfo.txt")
  message(VERBOSE
    "Avoiding repeated git clone, stamp file is up to date: "
    "'/Users/robertw/Workspace/x-plane/xp_welly_llm_atc/build-sanitize/espeak_ng/src/espeak_ng_external-stamp/espeak_ng_external-gitclone-lastrun.txt'"
  )
  return()
endif()

# Even at VERBOSE level, we don't want to see the commands executed, but
# enabling them to be shown for DEBUG may be useful to help diagnose problems.
cmake_language(GET_MESSAGE_LOG_LEVEL active_log_level)
if(active_log_level MATCHES "DEBUG|TRACE")
  set(maybe_show_command COMMAND_ECHO STDOUT)
else()
  set(maybe_show_command "")
endif()

execute_process(
  COMMAND ${CMAKE_COMMAND} -E rm -rf "/Users/robertw/Workspace/x-plane/xp_welly_llm_atc/build-sanitize/espeak_ng/src/espeak_ng_external"
  RESULT_VARIABLE error_code
  ${maybe_show_command}
)
if(error_code)
  message(FATAL_ERROR "Failed to remove directory: '/Users/robertw/Workspace/x-plane/xp_welly_llm_atc/build-sanitize/espeak_ng/src/espeak_ng_external'")
endif()

# try the clone 3 times in case there is an odd git clone issue
set(error_code 1)
set(number_of_tries 0)
while(error_code AND number_of_tries LESS 3)
  execute_process(
    COMMAND "/usr/bin/git"
            clone --no-checkout --config "advice.detachedHead=false" "https://github.com/espeak-ng/espeak-ng.git" "espeak_ng_external"
    WORKING_DIRECTORY "/Users/robertw/Workspace/x-plane/xp_welly_llm_atc/build-sanitize/espeak_ng/src"
    RESULT_VARIABLE error_code
    ${maybe_show_command}
  )
  math(EXPR number_of_tries "${number_of_tries} + 1")
endwhile()
if(number_of_tries GREATER 1)
  message(NOTICE "Had to git clone more than once: ${number_of_tries} times.")
endif()
if(error_code)
  message(FATAL_ERROR "Failed to clone repository: 'https://github.com/espeak-ng/espeak-ng.git'")
endif()

execute_process(
  COMMAND "/usr/bin/git"
          checkout "212928b394a96e8fd2096616bfd54e17845c48f6" --
  WORKING_DIRECTORY "/Users/robertw/Workspace/x-plane/xp_welly_llm_atc/build-sanitize/espeak_ng/src/espeak_ng_external"
  RESULT_VARIABLE error_code
  ${maybe_show_command}
)
if(error_code)
  message(FATAL_ERROR "Failed to checkout tag: '212928b394a96e8fd2096616bfd54e17845c48f6'")
endif()

set(init_submodules TRUE)
if(init_submodules)
  execute_process(
    COMMAND "/usr/bin/git" 
            submodule update --recursive --init 
    WORKING_DIRECTORY "/Users/robertw/Workspace/x-plane/xp_welly_llm_atc/build-sanitize/espeak_ng/src/espeak_ng_external"
    RESULT_VARIABLE error_code
    ${maybe_show_command}
  )
endif()
if(error_code)
  message(FATAL_ERROR "Failed to update submodules in: '/Users/robertw/Workspace/x-plane/xp_welly_llm_atc/build-sanitize/espeak_ng/src/espeak_ng_external'")
endif()

# Complete success, update the script-last-run stamp file:
#
execute_process(
  COMMAND ${CMAKE_COMMAND} -E copy "/Users/robertw/Workspace/x-plane/xp_welly_llm_atc/build-sanitize/espeak_ng/src/espeak_ng_external-stamp/espeak_ng_external-gitinfo.txt" "/Users/robertw/Workspace/x-plane/xp_welly_llm_atc/build-sanitize/espeak_ng/src/espeak_ng_external-stamp/espeak_ng_external-gitclone-lastrun.txt"
  RESULT_VARIABLE error_code
  ${maybe_show_command}
)
if(error_code)
  message(FATAL_ERROR "Failed to copy script-last-run stamp file: '/Users/robertw/Workspace/x-plane/xp_welly_llm_atc/build-sanitize/espeak_ng/src/espeak_ng_external-stamp/espeak_ng_external-gitclone-lastrun.txt'")
endif()
