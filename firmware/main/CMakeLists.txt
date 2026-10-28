# Cada broker valida con su propia CA. Con AWS, el certificado y la clave
# del nodo no van en el binario: se leen de la partición certs de cada placa.
# La OTA con IoT Jobs también es solo de AWS.
if(CONFIG_AGRO_MQTT_BACKEND_AWS_IOT)
    set(embedded_certs "certs/aws_root_ca.pem")
    set(aws_srcs "aws_jobs.cpp" "ota_updater.cpp")
else()
    set(embedded_certs "certs/mqtt_ca.pem")
    set(aws_srcs "")
endif()

idf_component_register(SRCS "oled_display.cpp" "dht22_sensor.cpp" "main.cpp"
                            "device_id.cpp"
                            "mqtt_client.cpp"
                            "sensor_task.cpp"
                            "task_monitor.cpp"
                            "telemetry_task.cpp"
                            "wifi_manager.cpp"
                            ${aws_srcs}
                       INCLUDE_DIRS ""
                       EMBED_TXTFILES ${embedded_certs}
                       PRIV_REQUIRES app_update esp_app_format esp_driver_gpio esp_driver_i2c esp_event esp_http_client esp_https_ota esp_netif esp_partition esp_rom esp_timer esp_wifi mbedtls mqtt cjson nvs_flash)

# Estándar fijado acá y no heredado del default de ESP-IDF (gnu++26 en la
# 6.0), para que el código compile igual con cualquier versión instalada.
# C++20 es el mínimo que usa el código: inicializadores designados.
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20>)
