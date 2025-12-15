idf_component_register(SRC_DIRS "." "app" "app/wifi" "app/time" "app/aliyun" "app/websocket" "app/esp-sr" "app/audio" "app/buffer_manager"
                    INCLUDE_DIRS "." "app" "app/wifi" "app/time" "app/aliyun" "app/websocket" "app/esp-sr" "app/audio" "app/buffer_manager"
                    )
spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)