# Cada broker valida con su propia CA. AWS además identifica al nodo por
# su certificado, que con su clave vive en certs/aws/ fuera de git.
if(CONFIG_AGRO_MQTT_BACKEND_AWS_IOT)
    set(embedded_certs "certs/aws_root_ca.pem" "certs/aws/device.crt" "certs/aws/device.key")
else()
    set(embedded_certs "certs/mqtt_ca.pem")
endif()

idf_component_register(SRCS "oled_display.cpp" "dht22_sensor.cpp" "main.cpp"
                            "device_id.cpp"
                            "mqtt_client.cpp"
                            "sensor_task.cpp"
                            "task_monitor.cpp"
                            "telemetry_task.cpp"
                            "wifi_manager.cpp"
                       INCLUDE_DIRS ""
                       EMBED_TXTFILES ${embedded_certs}
                       PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_event esp_netif esp_rom esp_timer esp_wifi mqtt nvs_flash)

# Estándar fijado acá y no heredado del default de ESP-IDF (gnu++26 en la
# 6.0), para que el código compile igual con cualquier versión instalada.
# C++20 es el mínimo que usa el código: inicializadores designados.
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20>)
