cmake_minimum_required(VERSION 3.0)

project(ddgplayer)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

set(CMAKE_VERBOSE_MAKEFILE ON)
set(CMAKE_CXX_FLAGS
    "$ENV{CXXFLAGS} -O3 -fPIC -Wall -Wno-deprecated -Wno-unused-function -Wno-builtin-macro-redefined -Wno-deprecated-declarations"
)
set(CMAKE_C_FLAGS
  "$ENV{CXXFLAGS} -O3 -fPIC -Wall -Wno-deprecated -Wno-unused-function -Wno-builtin-macro-redefined -Wno-deprecated-declarations"
)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_C_STANDARD 11)

include_directories(include)
aux_source_directory(src LIB_SRC)
add_library(${CMAKE_PROJECT_NAME} SHARED ${LIB_SRC})

set(FFMPEG_LIBS avformat avcodec avfilter avutil swresample swscale avdevice)
include_directories(${FFMPEG_DIR}/include)
foreach(target_lib ${FFMPEG_LIBS})
  set(lib_path ${FFMPEG_DIR}/lib/lib${target_lib}.a)
  message(STATUS "set ${target_lib} to ${lib_path}")
  add_library(${target_lib} STATIC IMPORTED)
  set_target_properties(
    ${target_lib} 
   PROPERTIES IMPORTED_LOCATION
   ${lib_path}
)
endforeach()

set(fdk_aac_lib_path "${FDK_AAC_DIR}/lib/libfdk-aac.a")
message(STATUS "set fdk-aac to ${fdk_aac_lib_path}")
add_library(fdk-aac STATIC IMPORTED)
include_directories(${FDK_AAC_DIR}/include)
set_target_properties(
  fdk-aac
  PROPERTIES IMPORTED_LOCATION
  ${fdk_aac_lib_path}
)

set(x264_lib_path "${X264_DIR}/lib/libx264.a")
message(STATUS "set x264 to ${x264_lib_path}")
add_library(x264 STATIC IMPORTED)
include_directories(${X264_DIR}/include)
set_target_properties(
  x264 
  PROPERTIES IMPORTED_LOCATION
  ${x264_lib_path}
)

target_link_libraries(${CMAKE_PROJECT_NAME} 
  -landroid
  -lz
  -lcamera2ndk
  -lmediandk
  -Wl,--start-group 
  avformat avcodec avfilter avutil swresample swscale avdevice
  -Wl,--end-group
  fdk-aac
  x264
)

aux_source_directory(tests TEST_SRC)

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)
set(LIBRARY_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/lib)

foreach(filepath ${TEST_SRC})
  message(STATUS "building ${filepath} ...")
  get_filename_component(filename ${filepath} NAME_WLE)
  add_executable(${filename} ${filepath})
  add_dependencies(${filename} ${CMAKE_PROJECT_NAME})
  target_link_libraries(${filename} ${CMAKE_PROJECT_NAME} dl)
endforeach()
