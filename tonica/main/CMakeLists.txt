idf_component_register(
  SRCS main.cpp i2s_sink.cpp provisioning.cpp usb_audio_sink.cpp net_services.cpp status_led.cpp zeroconf_service.cpp takeover.cpp button.cpp wifi_setup.cpp event_log.cpp notify.cpp device_login.cpp sinfonica.cpp auto_update.cpp
  INCLUDE_DIRS .
  EMBED_TXTFILES setup_page.html ota_pubkey.pem
  REQUIRES esp_driver_gpio core esp_partition spi_flash usb usb_host_uac esp_ringbuf esp_http_server app_update espressif__mdns espressif__led_strip nvs_flash esp_wifi esp_netif esp_event esp_driver_i2s esp_driver_uart vfs json console lwip mbedtls esp_ghota esp_https_ota esp_http_client
)
