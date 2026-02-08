cmake_minimum_required(VERSION 3.30)

if (NOT DEFINED PROFRAW_DIR OR NOT DEFINED PROFDATA_FILE OR NOT DEFINED LLVM_PROFDATA_EXECUTABLE)
    message(FATAL_ERROR "merge_profraw.cmake requires PROFRAW_DIR, PROFDATA_FILE, and LLVM_PROFDATA_EXECUTABLE")
endif()

file(GLOB PROFRAW_FILES "${PROFRAW_DIR}/*.profraw")
if (PROFRAW_FILES STREQUAL "")
    message(FATAL_ERROR "No .profraw files found in ${PROFRAW_DIR}")
endif()

execute_process(
    COMMAND "${LLVM_PROFDATA_EXECUTABLE}" merge -sparse ${PROFRAW_FILES} -o "${PROFDATA_FILE}"
    RESULT_VARIABLE MERGE_RESULT
)
if (NOT MERGE_RESULT EQUAL 0)
    message(FATAL_ERROR "llvm-profdata merge failed with exit code ${MERGE_RESULT}")
endif()
