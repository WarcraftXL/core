# Compares a file a build made with the copy kept in the sources (cmake -DMADE=... -DKEPT=... -P),
# their first lines (which name where they were made from) aside: a warning, not an error, when
# they differ (another compiler's bytecode differs and is as good), saying how to refresh the copy.
file(STRINGS "${MADE}" made)
set(kept "")
if(EXISTS "${KEPT}")
    file(STRINGS "${KEPT}" kept)
endif()
list(LENGTH made made_count)
list(LENGTH kept kept_count)
if(made_count GREATER 0)
    list(REMOVE_AT made 0)
endif()
if(kept_count GREATER 0)
    list(REMOVE_AT kept 0)
endif()
if(NOT "${made}" STREQUAL "${kept}")
    message(WARNING "${KEPT} differs from what this build compiled (${MADE}); copy it over (its first line naming "
                    "src/gx/d3d9/shaders/draw.txt) when the programs changed.")
endif()
