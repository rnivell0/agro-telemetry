idf_component_register(SRCS "oled_display.cpp" "dht22_sensor.cpp" "main.cpp"
                            "mqtt_client.cpp"
                            "sensor_task.cpp"
                            "task_monitor.cpp"
                            "telemetry_task.cpp"
                            "wifi_manager.cpp"
                       INCLUDE_DIRS ""
                       EMBED_TXTFILES "certs/mqtt_ca.pem"
                       PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_event esp_netif esp_rom esp_timer esp_wifi mqtt nvs_flash)

# Estándar fijado acá y no heredado del default de ESP-IDF (gnu++26 en la
# 6.0), para que el código compile igual con cualquier versión instalada.
# C++20 es el mínimo que usa el código: inicializadores designados.
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20>)
