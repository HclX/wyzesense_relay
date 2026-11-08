idf_component_register(SRCS "main.c" "settings.c" "net.c" "contact.c" "relay_task.c" "web.c" "rig_task.c" "cjtag_task.c"
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES "../../web/index.html"  # the page shared with the ESP8266 build
                       REQUIRES ows_core nvs_flash esp_wifi esp_netif esp_event esp_http_server app_update
                                esp_driver_uart esp_driver_gpio esp_timer esp_adc)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)  # as the rest of our C

# the gateway images the relay carries for its contact (#76, gateway_image.py: OWS and stock v1.8),
# embedded as _binary_gateway_contact_v1_app_bin_* and _binary_gateway_stock_v1_8_app_bin_*; checked on every build
set(gw_bin "${CMAKE_BINARY_DIR}/gateway-contact-v1-app.bin")
add_custom_target(gateway_image
    COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/../../gateway_image.py" "${gw_bin}"
    BYPRODUCTS "${gw_bin}" VERBATIM)
target_add_binary_data(${COMPONENT_LIB} "${gw_bin}" BINARY DEPENDS gateway_image)

set(gw_stock_bin "${CMAKE_BINARY_DIR}/gateway-stock-v1-8-app.bin")
add_custom_target(gateway_stock_image
    COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/../../gateway_image.py" --stock "${gw_stock_bin}"
    BYPRODUCTS "${gw_stock_bin}" VERBATIM)
target_add_binary_data(${COMPONENT_LIB} "${gw_stock_bin}" BINARY DEPENDS gateway_stock_image)

# the relay's version for its status page and for the web page to find in an image ("OWS-RELAY-VERSION:")
execute_process(COMMAND git describe --always --dirty --tags WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
                OUTPUT_VARIABLE relay_version OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(relay_version STREQUAL "")
    set(relay_version unknown)
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE OWS_RELAY_VERSION="${relay_version}")
