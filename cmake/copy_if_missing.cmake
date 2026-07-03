# copy_if_missing.cmake
# Copies SRC to DST only when DST does not already exist.
# Invoked at build time via:
#   cmake -DSRC=... -DDST=... -P copy_if_missing.cmake
if(NOT EXISTS "${DST}")
    file(COPY_FILE "${SRC}" "${DST}")
    message(STATUS "Deployed: ${DST}")
else()
    message(STATUS "Skipped (already exists): ${DST}")
endif()
