if(NOT DEFINED OUTPUT)
    message(FATAL_ERROR "OUTPUT is required")
endif()
if(NOT DEFINED MAIN_LIBRARY)
    message(FATAL_ERROR "MAIN_LIBRARY is required")
endif()
if(DEFINED STATIC_LIBRARIES_FILE)
    file(STRINGS "${STATIC_LIBRARIES_FILE}" STATIC_LIBRARY_LIST)
elseif(DEFINED STATIC_LIBRARIES)
    string(REPLACE "|" ";" STATIC_LIBRARY_LIST "${STATIC_LIBRARIES}")
else()
    message(FATAL_ERROR "STATIC_LIBRARIES or STATIC_LIBRARIES_FILE is required")
endif()

set(BUNDLED_LIBRARY "${OUTPUT}.bundled")
file(REMOVE "${BUNDLED_LIBRARY}")

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    execute_process(
        COMMAND libtool -static -o "${BUNDLED_LIBRARY}" "${MAIN_LIBRARY}" ${STATIC_LIBRARY_LIST}
        RESULT_VARIABLE BUNDLE_RESULT
        COMMAND_ERROR_IS_FATAL ANY)
elseif(CMAKE_HOST_WIN32)
    execute_process(
        COMMAND "${AR_EXECUTABLE}" "/OUT:${BUNDLED_LIBRARY}" "${MAIN_LIBRARY}" ${STATIC_LIBRARY_LIST}
        RESULT_VARIABLE BUNDLE_RESULT
        COMMAND_ERROR_IS_FATAL ANY)
else()
    set(MRI_SCRIPT "${OUTPUT}.mri")
    file(WRITE "${MRI_SCRIPT}" "CREATE ${BUNDLED_LIBRARY}\n")
    file(APPEND "${MRI_SCRIPT}" "ADDLIB ${MAIN_LIBRARY}\n")
    foreach(STATIC_LIBRARY IN LISTS STATIC_LIBRARY_LIST)
        file(APPEND "${MRI_SCRIPT}" "ADDLIB ${STATIC_LIBRARY}\n")
    endforeach()
    file(APPEND "${MRI_SCRIPT}" "SAVE\nEND\n")
    execute_process(
        COMMAND "${AR_EXECUTABLE}" -M
        INPUT_FILE "${MRI_SCRIPT}"
        RESULT_VARIABLE BUNDLE_RESULT
        COMMAND_ERROR_IS_FATAL ANY)
    file(REMOVE "${MRI_SCRIPT}")

    if(LOCALIZE_SYMBOLS)
        # The archive bundles its own instantiations of libstdc++ templates
        # (std::format, std::string, ...) as COMDAT groups. A consumer built
        # with a different GCC has its own, and the linker keeps only one copy
        # per group, so code compiled against one libstdc++ layout ends up
        # calling the other (e.g. a segfault in Binder::bindCopyToClause when
        # the GCC 13 prebuilt is linked on a GCC 15 system). Give the archive
        # private copies of everything that isn't the Ladybug API:
        #  1. Partially link it into one object, turning COMDAT group members
        #     into ordinary sections so the final link can't discard them.
        #  2. Make every defined global local except `lbug::` (mangled
        #     `4lbug`) and the C API (`lbug_*`).
        #  3. Weaken the remaining STB_GNU_UNIQUE definitions (function-local
        #     statics, inline variables), which objcopy cannot localize and
        #     which would otherwise be duplicate symbols. Only defined symbols:
        #     a weakened undefined reference would resolve to 0.
        set(COMBINED_OBJECT "${OUTPUT}.combined.o")
        set(UNIQUE_SYMBOLS_FILE "${OUTPUT}.unique.txt")
        execute_process(
            COMMAND "${LD_EXECUTABLE}" -r --force-group-allocation
                    --whole-archive "${BUNDLED_LIBRARY}" -o "${COMBINED_OBJECT}"
            COMMAND_ERROR_IS_FATAL ANY)
        execute_process(
            COMMAND "${OBJCOPY_EXECUTABLE}" --wildcard
                    "--keep-global-symbol=*4lbug*"
                    "--keep-global-symbol=lbug_*"
                    "${COMBINED_OBJECT}"
            COMMAND_ERROR_IS_FATAL ANY)
        execute_process(
            COMMAND "${NM_EXECUTABLE}" -g --defined-only "${COMBINED_OBJECT}"
            OUTPUT_VARIABLE GLOBAL_SYMBOLS
            COMMAND_ERROR_IS_FATAL ANY)
        string(REGEX MATCHALL "[^\n]* u [^\n]*" UNIQUE_LINES "${GLOBAL_SYMBOLS}")
        list(TRANSFORM UNIQUE_LINES REPLACE "^.* u " "")
        list(JOIN UNIQUE_LINES "\n" UNIQUE_SYMBOLS)
        file(WRITE "${UNIQUE_SYMBOLS_FILE}" "${UNIQUE_SYMBOLS}\n")
        execute_process(
            COMMAND "${OBJCOPY_EXECUTABLE}" "--weaken-symbols=${UNIQUE_SYMBOLS_FILE}"
                    "${COMBINED_OBJECT}"
            COMMAND_ERROR_IS_FATAL ANY)
        file(REMOVE "${BUNDLED_LIBRARY}")
        execute_process(
            COMMAND "${AR_EXECUTABLE}" rcs "${BUNDLED_LIBRARY}" "${COMBINED_OBJECT}"
            COMMAND_ERROR_IS_FATAL ANY)
        file(REMOVE "${COMBINED_OBJECT}" "${UNIQUE_SYMBOLS_FILE}")
    endif()
endif()

file(RENAME "${BUNDLED_LIBRARY}" "${OUTPUT}")
if(DEFINED RANLIB_EXECUTABLE AND NOT CMAKE_HOST_WIN32)
    execute_process(COMMAND "${RANLIB_EXECUTABLE}" "${OUTPUT}" COMMAND_ERROR_IS_FATAL ANY)
endif()
