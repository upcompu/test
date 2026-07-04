idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "${ESP_MATTER_PATH}/examples/common/utils"
)

# Verze aplikace vidí Matter OTA a Basic Information cluster
set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
target_compile_options(${COMPONENT_LIB} PUBLIC "-DCHIP_HAVE_CONFIG_H")
