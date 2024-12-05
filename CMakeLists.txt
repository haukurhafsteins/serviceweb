idf_component_register(SRCS "api_nvs.cpp" "api.cpp" "api_upload.cpp" "api_download.cpp" "api_nvs.cpp" "sysmon.cpp" "serviceweb.cpp" "ota.cpp"
                    REQUIRES httpss cJSON esp_public_parameter app_update vfs nvs_flash spiffs esp_ethernet
                    INCLUDE_DIRS "include"
                    EMBED_FILES
                    )
