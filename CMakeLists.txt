idf_component_register(SRCS "api.cpp" "sysmon.cpp" "upload.cpp" "serviceweb.cpp" "ota.cpp"
                    REQUIRES httpss cJSON esp_public_parameter app_update vfs nvs_flash spiffs esp_ethernet multipart-parser-c
                    INCLUDE_DIRS "include"
                    EMBED_FILES "web/upload.html" "web/ota.html" "web/dir.html"
                    )
