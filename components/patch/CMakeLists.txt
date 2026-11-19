idf_component_register(
    SRCS "patch.c" "patch_json.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES json
)
