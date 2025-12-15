idf_component_register(SRC_DIRS "." "app" "app/wifi" "app/time" "app/aliyun" "app/websocket"  "app/audio" 
                    INCLUDE_DIRS "." "app" "app/wifi" "app/time" "app/aliyun" "app/websocket"  "app/audio" 
                    )
spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)