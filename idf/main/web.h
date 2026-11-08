// The relay's web pages and endpoints (esp_http_server): status, setup, the contact's reset and reflash, OTA.
//   curl http://<ip>/                                         status
//   curl -X POST http://<ip>/contact/reset
//   curl --data-binary @contact.bin 'http://<ip>/contact/flash?mac=777B42DF&nv=wipe'   (tools/contact_image.py)
//   curl --data-binary @build/ows_relay.bin http://<ip>/ota   this firmware, then a restart
#pragma once

void web_start(void);
