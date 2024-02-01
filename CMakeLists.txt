idf_component_register(SRCS "serviceweb.cpp" "fileserver.c" "ota.cpp" "sysmon.c"
                    REQUIRES httpss cJSON esp_public_parameter app_update vfs nvs_flash spiffs esp_ethernet
                    INCLUDE_DIRS "include"
                    EMBED_FILES "fileserver.html")
