#include "esp_camera.h"

camera_fb_t *take_picture(void);
esp_err_t init_camera(void);
esp_err_t deinit_camera(void);
void send_camera_image(void);