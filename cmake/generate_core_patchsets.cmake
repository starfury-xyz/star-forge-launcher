set(CORE_INC "")
foreach(f IN LISTS CORE_PATCHSETS)
    get_filename_component(name "${f}" NAME)
    message(STATUS "file: ${f}")
    file(READ "${f}" text)
    string(APPEND CORE_INC "{\"core/${name}\", R\"sfcore(${text})sfcore\"},\n")
endforeach()

# Written via a temp file and copied only when different, so core.cpp only rebuilds on a real change.
file(WRITE "${OUTPUT_FILE}.tmp" "${CORE_INC}")
file(COPY_FILE "${OUTPUT_FILE}.tmp" "${OUTPUT_FILE}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT_FILE}.tmp")
