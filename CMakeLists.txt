cmake_minimum_required(VERSION 3.16)

project(FunctionQueue)

set(CMAKE_CXX_STANDARD 20)

add_executable(fq_test_call_only src/fq_test_call_only.cpp)
add_executable(fq_test_callNDelete src/fq_test_callNDelete.cpp)
add_executable(fq_test_1r_1w src/fq_test_1r_1w.cpp)
add_executable(fq_test_nr_nw src/fq_test_nr_nw.cpp)
add_executable(fq_test_nr_1w src/fq_test_nr_1w.cpp)
add_executable(fq_test_1r_nw src/fq_test_1r_nw.cpp)
add_executable(fq_test_sync_only src/fq_test_sync_only.cpp)
add_executable(fq_test_mr_mw src/fq_test_mr_mw.cpp)

if (WIN32)
    set(LIB_DIR "C:/Users/Aniket Bisht/Documents/Libraries/Windows")  # set the library directory for Windows ::::
endif (WIN32)

set(LIB_DIR ${LIB_DIR}/mingw)
set(LIBS ${LIB_DIR}/lib)
include_directories(${LIB_DIR}/include)

#[[set(CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS} -static)
set(CMAKE_EXE_LINKER_FLAGS ${CMAKE_EXE_LINKER_FLAGS} -static)]]

if (MINGW OR UNIX)
    target_link_libraries(fq_test_call_only PRIVATE pthread atomic)
    target_link_libraries(fq_test_callNDelete PRIVATE pthread atomic)
    target_link_libraries(fq_test_1r_1w PRIVATE pthread atomic)
    target_link_libraries(fq_test_nr_nw PRIVATE pthread atomic)
    target_link_libraries(fq_test_nr_1w PRIVATE pthread atomic)
    target_link_libraries(fq_test_1r_nw PRIVATE pthread atomic)
    target_link_libraries(fq_test_sync_only PRIVATE pthread atomic)
    target_link_libraries(fq_test_mr_mw PRIVATE pthread atomic)
endif ()
