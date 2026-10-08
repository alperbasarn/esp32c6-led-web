idf_component_register(
    SRCS "app_main.cpp" "mqtt_link.cpp" "mqtt_proto.cpp" "model/color_model.cpp" "model/net_model.cpp" "model/schedule_model.cpp" "model/http_encoding.cpp"
    INCLUDE_DIRS "." "include"
    PRIV_INCLUDE_DIRS "." "include" "${ESP_MATTER_PATH}/examples/common/utils"
    REQUIRES app_update esp_event esp_http_client esp_http_server esp_https_ota esp_netif esp_wifi json led_strip mbedtls mqtt nvs_flash esp_matter
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")

# ---------------------------------------------------------------------------
# The web UI, embedded gzip-compressed.
#
# The page used to be a 59,436-byte raw string literal in app_main.cpp. It is
# embedded ONLY compressed, never both ways: storing the uncompressed copy
# alongside would cost more flash than the literal it replaced, which is the
# whole reason for compressing it. That makes gzip negotiation load-bearing
# rather than an optimisation -- see main/model/http_encoding.cpp.
#
# Compression happens at build time from main/web/index.html, so the asset
# stays reviewable as HTML and the committed tree holds exactly one copy of it.
set(WEB_ASSET_SRC "${CMAKE_CURRENT_LIST_DIR}/web/index.html")
set(WEB_ASSET_GZ "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")

# ESP-IDF sets `python` to the venv interpreter the build is already using;
# fall back for any context that does not.
if(NOT DEFINED python)
    set(python "python3")
endif()

add_custom_command(
    OUTPUT "${WEB_ASSET_GZ}"
    COMMAND "${python}" "${CMAKE_CURRENT_LIST_DIR}/web/gzip_asset.py"
            "${WEB_ASSET_SRC}" "${WEB_ASSET_GZ}"
    DEPENDS "${WEB_ASSET_SRC}" "${CMAKE_CURRENT_LIST_DIR}/web/gzip_asset.py"
    COMMENT "Compressing web/index.html for embedding"
    VERBATIM
)
add_custom_target(web_asset_gz DEPENDS "${WEB_ASSET_GZ}")
add_dependencies(${COMPONENT_LIB} web_asset_gz)

# Yields _binary_index_html_gz_start / _end, declared in app_main.cpp.
target_add_binary_data(${COMPONENT_LIB} "${WEB_ASSET_GZ}" BINARY)
