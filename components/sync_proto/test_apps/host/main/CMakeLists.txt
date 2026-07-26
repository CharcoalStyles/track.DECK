idf_component_register(
    SRCS
    "test_sync_snapshot.cpp"
    "test_tz_apply.cpp"
    "test_sync_backoff.cpp"
    INCLUDE_DIRS
    "."
    REQUIRES
    sync_proto
    WHOLE_ARCHIVE
)

# 'main' for IDF_TARGET=linux is normally defined in the freertos component;
# since we mock freertos out here, let Catch2 provide main() instead.
target_link_libraries(${COMPONENT_LIB} PRIVATE Catch2WithMain)
