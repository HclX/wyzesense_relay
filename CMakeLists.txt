cmake_minimum_required(VERSION 3.20)
project(wyzesense-relay C)
enable_testing()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The ESP32 relay (issue #22): its portable core (relay/), built for the host here: ows-relay (the core on a PC, the
# C counterpart of tools/ws_bridge.py) and its native tests. The ESP-IDF firmware (idf/) builds with idf.py.
add_library(ows_relay STATIC relay/relay.c relay/sock_posix.c relay/buffer.c common/host_proto.c)
target_include_directories(ows_relay PUBLIC relay common)
target_compile_options(ows_relay PRIVATE -Wall -Wextra -Werror)

add_executable(ows-relay host/ows_relay.c)
target_link_libraries(ows-relay PRIVATE ows_relay)
target_compile_options(ows-relay PRIVATE -Wall -Wextra -Werror)

add_executable(test_relay host/test_relay.c)
target_link_libraries(test_relay PRIVATE ows_relay)
target_compile_options(test_relay PRIVATE -Wall -Wextra -Werror -fsanitize=address,undefined)
target_link_options(test_relay PRIVATE -fsanitize=address,undefined)
add_test(NAME relay COMMAND test_relay)

# the contact's ROM bootloader, cJTAG probe, and runtime NV client (contact/): ows-sbl runs it on a PC (tests/test_ows_sbl.py)
add_library(ows_contact STATIC contact/sbl.c contact/sblprobe.c contact/gwsync.c contact/gwnv.c contact/cjtag.c)
target_include_directories(ows_contact PUBLIC contact common)
target_link_libraries(ows_contact PUBLIC ows_relay)  # host_proto.c
target_compile_options(ows_contact PRIVATE -Wall -Wextra -Werror)

add_executable(test_cjtag host/test_cjtag.c)
target_link_libraries(test_cjtag PRIVATE ows_contact)
target_compile_options(test_cjtag PRIVATE -Wall -Wextra -Werror -fsanitize=address,undefined)
target_link_options(test_cjtag PRIVATE -fsanitize=address,undefined)
add_test(NAME cjtag COMMAND test_cjtag)

add_executable(test_sblprobe host/test_sblprobe.c)
target_link_libraries(test_sblprobe PRIVATE ows_contact)
target_compile_options(test_sblprobe PRIVATE -Wall -Wextra -Werror -fsanitize=address,undefined)
target_link_options(test_sblprobe PRIVATE -fsanitize=address,undefined)
add_test(NAME sblprobe COMMAND test_sblprobe)

add_executable(test_gwnv host/test_gwnv.c)
target_link_libraries(test_gwnv PRIVATE ows_contact)
target_compile_options(test_gwnv PRIVATE -Wall -Wextra -Werror -fsanitize=address,undefined)
target_link_options(test_gwnv PRIVATE -fsanitize=address,undefined)
add_test(NAME gwnv COMMAND test_gwnv)

add_executable(ows-sbl host/ows_sbl.c)
target_link_libraries(ows-sbl PRIVATE ows_contact)
target_compile_options(ows-sbl PRIVATE -Wall -Wextra -Werror -fsanitize=address,undefined)
target_link_options(ows-sbl PRIVATE -fsanitize=address,undefined)

# the start-up gateway check and update (contact/gwsync.c): tests/test_gwsync.py
add_executable(ows-gwsync host/ows_gwsync.c)
target_link_libraries(ows-gwsync PRIVATE ows_contact)
target_compile_options(ows-gwsync PRIVATE -Wall -Wextra -Werror -fsanitize=address,undefined)
target_link_options(ows-gwsync PRIVATE -fsanitize=address,undefined)

add_executable(test_buffer host/test_buffer.c)
target_link_libraries(test_buffer PRIVATE ows_relay)
target_compile_options(test_buffer PRIVATE -Wall -Wextra -Werror -fsanitize=address,undefined)
target_link_options(test_buffer PRIVATE -fsanitize=address,undefined)
add_test(NAME relay_buffer COMMAND test_buffer)

# the relays' web upload parsing, HTTP auth, and mDNS responder (web/, shared with their one page index.html)
add_library(ows_web STATIC web/multipart.c web/http_auth.c web/mdns_pkt.c)
target_include_directories(ows_web PUBLIC web)
target_compile_options(ows_web PRIVATE -Wall -Wextra -Werror)

add_executable(test_multipart host/test_multipart.c)
target_link_libraries(test_multipart PRIVATE ows_web)
target_compile_options(test_multipart PRIVATE -Wall -Wextra -Werror -fsanitize=address,undefined)
target_link_options(test_multipart PRIVATE -fsanitize=address,undefined)
add_test(NAME multipart COMMAND test_multipart)

add_executable(test_http_auth host/test_http_auth.c)
target_link_libraries(test_http_auth PRIVATE ows_web)
target_compile_options(test_http_auth PRIVATE -Wall -Wextra -Werror -fsanitize=address,undefined)
target_link_options(test_http_auth PRIVATE -fsanitize=address,undefined)
add_test(NAME http_auth COMMAND test_http_auth)

add_executable(test_mdns host/test_mdns.c)
target_link_libraries(test_mdns PRIVATE ows_web)
target_compile_options(test_mdns PRIVATE -Wall -Wextra -Werror -fsanitize=address,undefined)
target_link_options(test_mdns PRIVATE -fsanitize=address,undefined)
add_test(NAME mdns COMMAND test_mdns)

# the battery rig's core (rig/, #95): measuring a sensor's current from its supply's voltage alone
add_library(ows_rig STATIC rig/rig.c)
target_include_directories(ows_rig PUBLIC rig)
target_compile_options(ows_rig PRIVATE -Wall -Wextra -Werror)

add_executable(test_rig host/test_rig.c)
target_link_libraries(test_rig PRIVATE ows_rig m)
target_compile_options(test_rig PRIVATE -Wall -Wextra -Werror -O2 -fsanitize=address,undefined)
target_link_options(test_rig PRIVATE -fsanitize=address,undefined)
add_test(NAME rig COMMAND test_rig)
