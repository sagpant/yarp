# Copyright: (C) 2012 IITRBCS
# Authors: Paul Fitzpatrick
# CopyPolicy: Released under the terms of the LGPLv2.1 or later, see LGPL.TXT

macro(yarp_idl thrift)
  string(REGEX REPLACE "[^a-zA-Z0-9]" "_" thrift_name ${thrift})
  set(dir ${CMAKE_CURRENT_BINARY_DIR}/${thrift_name})

  set(INCLUDES)
  set(DEST_FILES)
  set(SRC_FILES)
  set(MISSING FALSE)
  set(RENAMING FALSE)

  set(CORE_ARGN)
  set(INCLUDING FALSE)
  foreach(arg ${ARGN})
    if (INCLUDING)
      list(APPEND INCLUDES ${arg})
    else ()
      if (arg STREQUAL "INCLUDE")
	set(INCLUDING TRUE)
      else ()
	list(APPEND CORE_ARGN ${arg})
      endif()
    endif()
  endforeach()

  foreach(arg ${CORE_ARGN})
    if (NOT arg STREQUAL "AS")
      get_filename_component(abs_name ${arg} ABSOLUTE)
      if (NOT RENAMING)
	list(APPEND DEST_FILES ${abs_name})
	if (NOT EXISTS ${abs_name})
	  set(MISSING TRUE)
	endif ()
      endif ()
      get_filename_component(ext ${arg} EXT)
      if (ext STREQUAL ".h")
	list(APPEND INCLUDES ${arg})
      endif()
      set(RENAMING FALSE)
    else ()
      set(RENAMING TRUE)
      list(LENGTH INCLUDES len)
      math(EXPR len "${len} - 1")
      list(REMOVE_AT INCLUDES ${len})
    endif()
  endforeach()
  option(ALLOW_IDL_GENERATION "Allow YARP to (re)build IDL files as needed" ${MISSING})
  if (ALLOW_IDL_GENERATION OR MISSING)
    find_package(YARP REQUIRED)
    find_program(YARPIDL_LOCATION yarpidl_thrift HINTS ${YARP_IDL_BINARY_HINT})
    if (NOT YARPIDL_LOCATION)
      message(STATUS "Hints for yarpidl_thrift location: ${YARP_IDL_BINARY_HINT}")
      message(FATAL_ERROR "Cannot find yarpidl_thrift program")
    endif ()
    set(RENAMING FALSE)
    foreach(arg ${CORE_ARGN})
      if (NOT arg STREQUAL "AS")
	get_filename_component(abs_name ${arg} ABSOLUTE)
	get_filename_component(name ${arg} NAME)
	if (NOT RENAMING)
	  list(APPEND SRC_FILES ${name})
	endif ()
	set(RENAMING FALSE)
      else ()
	set(RENAMING TRUE)
      endif()
    endforeach()
    set(SRC_DIR ${dir})
    get_filename_component(thrift_base ${thrift} NAME_WE)
    set(SRC_INDEX ${thrift_base}_index.txt)
    set(SRC_NAME ${thrift_base})
    configure_file(${YARP_MODULE_PATH}/template/YarpTweakIDL.cmake.in ${dir}/tweak.cmake @ONLY)

    get_filename_component(path_trift ${thrift} ABSOLUTE)
    if (NOT path_thrift) 
      set(path_thrift ${CMAKE_CURRENT_SOURCE_DIR})
    endif ()
    get_filename_component(name_thrift ${thrift} NAME)
    set(abs_thrift "${path_thrift}/${name_thrift}")
    set(ABS_ARGN)
    foreach(arg ${SRC_FILES})
      set(abs_arg "${dir}/${arg}")
      list(APPEND ABS_ARGN ${abs_arg})
    endforeach()
    add_custom_command(OUTPUT ${DEST_FILES}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${dir}
      COMMAND ${YARPIDL_LOCATION} -out ${dir} --gen yarp:cmake_supplies_headers ${abs_thrift}
      COMMAND ${CMAKE_COMMAND} -P ${dir}/tweak.cmake
      DEPENDS ${thrift} ${dir}/tweak.cmake)
  endif()
endmacro()

macro(yarp_idl_to_dir thrift_file output_dir)
  set(output_dir ${output_dir})
  #extract relevant folders / names 
  string(FIND ${thrift_file} "/" lastSlash REVERSE)

  if(lastSlash GREATER 0)
    string(SUBSTRING ${thrift_file} 0 ${lastSlash} include_prefix)
  else()
   set (include_prefix "")
  endif()
  get_filename_component(thriftName ${thrift_file} NAME_WE)
  #set output 
  set(dir ${CMAKE_CURRENT_BINARY_DIR}/${include_prefix}/${thriftName})
  string(REGEX REPLACE "[^a-zA-Z0-9]" "_" thrift_target_name ${thrift_file})
  option(ALLOW_IDL_GENERATION "Allow YARP to (re)build IDL files as needed" FALSE)
  mark_as_advanced(ALLOW_IDL_GENERATION)
  if (ALLOW_IDL_GENERATION)
    message(STATUS "Generating source files for Thrift file ${thrift_file}. Output directory: ${output_dir}.")
    # generate during cmake configuration, so we have all the names of generated files
    find_program(YARPIDL_LOCATION yarpidl_thrift HINTS ${YARP_IDL_BINARY_HINT})
    make_directory(${dir})
    configure_file(${YARP_MODULE_PATH}/template/placeGeneratedThriftFiles.cmake.in ${dir}/place${thriftName}.cmake @ONLY)
    execute_process(COMMAND ${YARPIDL_LOCATION} -out ${dir} --gen yarp:include_prefix -I ${CMAKE_CURRENT_SOURCE_DIR} ${thrift_file}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    execute_process(COMMAND ${CMAKE_COMMAND} -P ${dir}/place${thriftName}.cmake)

    include(${output_dir}/${thrift_target_name}.cmake)
    set(DEST_FILES)
    foreach(generatedFile ${headers})
      list(APPEND DEST_FILES ${output_dir}/${generatedFile})
    endforeach()
    foreach(generatedFile ${sources})
      list(APPEND DEST_FILES ${output_dir}/${generatedFile})
    endforeach()
    
    add_custom_command(OUTPUT ${output_dir}/${thrift_target_name}.cmake ${DEST_FILES}
      COMMAND ${YARPIDL_LOCATION} -out ${dir} --gen yarp:include_prefix -I ${CMAKE_CURRENT_SOURCE_DIR} ${thrift_file} 
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} 
      COMMAND ${CMAKE_COMMAND} -P ${dir}/place${thriftName}.cmake
      DEPENDS ${thrift_file} ${YARPIDL_LOCATION})
    add_custom_target(${thrift_target_name} DEPENDS ${output_dir}/${thrift_target_name}.cmake)

  endif()
endmacro()
