idf_component_register(
    SRCS "app_main.cpp" "mqtt_link.cpp" "mqtt_proto.cpp"
    INCLUDE_DIRS "." "include"
    PRIV_INCLUDE_DIRS "." "include" "${ESP_MATTER_PATH}/examples/common/utils"
    REQUIRES app_update esp_event esp_http_client esp_http_server esp_https_ota esp_netif esp_wifi json led_strip mbedtls mqtt nvs_flash esp_matter
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
