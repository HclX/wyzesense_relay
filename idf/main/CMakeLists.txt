set(GATEWAY_BIN "${CMAKE_CURRENT_BINARY_DIR}/gateway_contact_v1.bin")
set(GATEWAY_SRC "$ENV{OWS_GATEWAY_IMAGE}")
if(NOT GATEWAY_SRC)
    set(GATEWAY_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../firmware/gateway-contact-v1-app.bin")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(
    OUTPUT "${GATEWAY_BIN}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../gateway_image.py" --bin "${GATEWAY_BIN}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../../gateway_image.py" "${GATEWAY_SRC}"
    VERBATIM
)

idf_component_register(
    SRCS "main.c" "wifi.c" "uart_port.c" "tcp_ws.c" "web.c" "rig_pins.c"
    INCLUDE_DIRS "."
    REQUIRES ows_core esp_wifi esp_event esp_netif esp_http_server esp_timer nvs_flash driver app_update
    EMBED_FILES "../../web/index.html" "${GATEWAY_BIN}"
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
