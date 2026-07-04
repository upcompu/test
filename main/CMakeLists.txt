idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "."
)

# Verze aplikace vidí Matter OTA a Basic Information cluster
set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
target_compile_options(${COMPONENT_LIB} PUBLIC "-DCHIP_HAVE_CONFIG_H")
