cmake_minimum_required(VERSION 3.0)

project(ddgplayer)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

set(CMAKE_VERBOSE_MAKEFILE ON)
set(CMAKE_CXX_FLAGS
    "$ENV{CXXFLAGS} -rdynamic -O3 -fPIC -std=c++11 -Wall -Wno-deprecated -Werror -Wno-unused-function -Wno-builtin-macro-redefined -Wno-deprecated-declarations"
)
set(CMAKE_C_FLAGS
    "$ENV{CXXFLAGS} -rdynamic -O3 -fPIC -std=c11 -Wall -Wno-deprecated -Werror -Wno-unused-function -Wno-builtin-macro-redefined -Wno-deprecated-declarations"
)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -L${FFMPEG_DIR}/lib -I${FFMPEG_DIR}/include")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -L${FFMPEG_DIR}/lib -I${FFMPEG_DIR}/include")

include_directories(include)
aux_source_directory(src LIB_SRC)
add_library(${CMAKE_PROJECT_NAME} SHARED ${LIB_SRC})

target_link_libraries(${CMAKE_PROJECT_NAME} 
  -Wl,--start-group 
  avformat avcodec avfilter avutil swresample swscale 
  -Wl,--end-group)
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
