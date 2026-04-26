idf_component_register(SRC_DIRS "." "app" "app/wifi" "app/time" "app/aliyun" "app/websocket"  "app/audio" "app/health" "app/task_list" "app/event" "app/RFID"
                    INCLUDE_DIRS "." "app" "app/wifi" "app/time" "app/aliyun" "app/websocket"  "app/audio" "app/health" "app/task_list" "app/event" "app/RFID"
                    )
spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)