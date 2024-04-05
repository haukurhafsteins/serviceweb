idf_component_register(SRCS "sysmon.cpp" "listdir.cpp" "upload.cpp" "serviceweb.cpp" "ota.cpp"
                    REQUIRES httpss cJSON esp_public_parameter app_update vfs nvs_flash spiffs esp_ethernet
                    INCLUDE_DIRS "include"
                    #EMBED_FILES
                    )
