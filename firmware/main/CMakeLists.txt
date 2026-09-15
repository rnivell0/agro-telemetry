idf_component_register(SRCS "oled_display.cpp" "dht11_sensor.cpp" "main.cpp"
                            "mqtt_client.cpp"
                            "sensor_task.cpp"
                            "telemetry_task.cpp"
                            "wifi_manager.cpp"
                       INCLUDE_DIRS ""
                       EMBED_TXTFILES "certs/mqtt_ca.pem"
                       PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_event esp_netif esp_rom esp_timer esp_wifi mqtt nvs_flash)
