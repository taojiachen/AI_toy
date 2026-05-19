#ifndef WEBSOCKET_H
#define WEBSOCKET_H  // 添加头文件保护，避免重复包含

// 第一步：先包含FreeRTOS核心头文件（必须放在最前面，定义TickType_t等类型）
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// 第二步：包含ESP-IDF相关头文件
#include "esp_err.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_tls.h"

// 自签名服务器证书（直接复制你的证书内容，格式化后可用）
static const char *server_cert_pem = "-----BEGIN CERTIFICATE-----\n"
"MIIDazCCAlOgAwIBAgIUIPW2t3rXK0FBf62mnPbw7ZV9kgwwDQYJKoZIhvcNAQEL\n"
"BQAwRTELMAkGA1UEBhMCQVUxEzARBgNVBAgMClNvbWUtU3RhdGUxITAfBgNVBAoM\n"
"GEludGVybmV0IFdpZGdpdHMgUHR5IEx0ZDAeFw0yNTExMDYxMzM3MDVaFw0yNjEx\n"
"MDYxMzM3MDVaMEUxCzAJBgNVBAYTAkFVMRMwEQYDVQQIDApTb21lLVN0YXRlMSEw\n"
"HwYDVQQKDBhJbnRlcm5ldCBXaWRnaXRzIFB0eSBMdGQwggEiMA0GCSqGSIb3DQEB\n"
"AQUAA4IBDwAwggEKAoIBAQCQsbD9TuS8n8j80WfhSjDgRn0pDeas78SV2BdklSA3\n"
"CKdIx7Zv6K/UjEvQwp7oigbxO8Xaj4TdVMkQssmd2VpERf54X7R+ramDmJnHfU0S\n"
"mo0mDu2997gu2cqGbZbkX3Dh14bpQ7geRIZ/XwZTfHpwJGgA/qJPXdxbk4oTNg4A\n"
"6P0Jp2PfUzj5ZnOfDBaNGNVs3IgE0+1JRHciy3Yc3mNVUVaz76T+3vmxRtefQZ4b\n"
"UuWAM7pw2gC5gaeDpIJUjkB11D4SrmUnlMP2WHPKly4ARaKmCoNgqyFiRe/WYF+6\n"
"UqAUt5kLrzFfBpbEHJLSoh+KAA5q72/DzX3mEhIn3jAVAgMBAAGjUzBRMB0GA1Ud\n"
"DgQWBBRAlQUSDqOG0vO//0NvG0y2m5PZ9DAfBgNVHSMEGDAWgBRAlQUSDqOG0vO/\n"
"/0NvG0y2m5PZ9DAPBgNVHRMBAf8EBTADAQH/MA0GCSqGSIb3DQEBCwUAA4IBAQBj\n"
"uih+3dqFOdb0wV9RH/wY5kEaHFxxfWZA/2nJlI47PhLod65xdf6zUePENPtMtRo9\n"
"lJpKjKejqxJTKSHRA6v/ZNxGceJ1J0mK4aTYUUTA07qCvOrxuvFIHwnKcY9S0sm/\n"
"5mLYRDynAxIkScJccoGjSNFPxVFeBTs5uCIxgta9d6YoYPWYBEfvccRQtEGADdc+\n"
"dnv6IKm4ZXnxSMw5zbJlGSm2xuVN8EyITjUx0b0kOAk4XwMmqcvncebsdsS0qZSE\n"
"sjNu7iSKKEosB2QruxxhFllp/6ow9LOABnXLcmw/l4Abln5ViORS2vVuYMln+fSP\n"
"NR2sXGCvfl4h2ZEvhhsc\n"
"-----END CERTIFICATE-----\n";

// WebSocket连接状态枚举
typedef enum {
    WS_STATE_DISCONNECTED = 0,
    WS_STATE_CONNECTING,
    WS_STATE_CONNECTED,
    WS_STATE_RECONNECTING,
    WS_STATE_ERROR
} ws_state_t;

// 数据接收回调函数类型
typedef void (*ws_data_handler_t)(const char *data, size_t len);

// 状态变化回调函数类型
typedef void (*ws_state_handler_t)(ws_state_t old_state, ws_state_t new_state);

/**
 * @brief 简化版启动WebSocket连接（核心接口）
 * @param uri WebSocket服务器地址（如：wss://192.168.10.44:8765）
 * @return ESP_OK成功，其他失败
 */
esp_err_t ws_start(const char *uri);

/**
 * @brief 停止WebSocket连接
 */
void ws_stop(void);

/**
 * @brief 发送文本数据
 * @param data 文本数据
 * @param len 数据长度
 * @return ESP_OK成功，其他失败
 */
esp_err_t ws_send_text(const char *data, size_t len);

/**
 * @brief 发送二进制数据
 * @param data 二进制数据
 * @param len 数据长度
 * @return ESP_OK成功，其他失败
 */
esp_err_t ws_send_binary(const void *data, size_t len);

/**
 * @brief 检查连接状态
 * @return true已连接，false未连接
 */
bool ws_is_connected(void);

/**
 * @brief 注册数据接收回调（可选）
 * @param handler 回调函数
 */
void ws_register_data_handler(ws_data_handler_t handler);

/**
 * @brief 注册状态变化回调（可选）
 * @param handler 回调函数
 */
void ws_register_state_handler(ws_state_handler_t handler);

/**
 * @brief 反初始化WebSocket客户端
 */
void ws_deinit(void);

/**
 * @brief 发送jpeg数据
 */
esp_err_t ws_send_jpeg_binary(uint8_t *jpg_data, size_t jpg_len);

#endif // WEBSOCKET_H