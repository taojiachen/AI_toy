#include "esp_camera.h"

camera_fb_t *take_picture(void);
esp_err_t init_camera(void);
void print_jpeg_as_base64(camera_fb_t *fb);