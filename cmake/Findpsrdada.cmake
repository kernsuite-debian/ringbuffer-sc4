include(FindPackageHandleStandardArgs)

find_library(PSRDADA_LIBRARY psrdada HINTS ENV LD_LIBRARY_PATH)
find_path(PSRDADA_INCLUDE_DIR "dada_hdu.h" HINTS ENV PSRDADA_INCLUDE_DIR)

# handle the QUIETLY and REQUIRED arguments and set PSRDADA_FOUND to TRUE
# if all listed variables are TRUE
find_package_handle_standard_args(psrdada DEFAULT_MSG PSRDADA_LIBRARY PSRDADA_INCLUDE_DIR)

mark_as_advanced(PSRDADA_LIBRARY)
include_directories(${PSRDADA_INCLUDE_DIR})

set(PSRDADA_LIBRARIES ${PSRDADA_LIBRARY} )
