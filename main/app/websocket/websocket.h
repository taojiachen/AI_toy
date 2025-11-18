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

/**
 * @brief 启动WebSocket客户端并连接服务器
 * @return ESP_OK: 成功; 其他: 失败
 */
esp_err_t ws_start(const char *ws_uri);

/**
 * @brief 向服务器发送JSON文本数据（WebSocket文本帧）
 * @param json_data: 待发送的JSON字符串（如 "{\"type\":\"audio\"}"）
 * @param len: JSON数据长度（建议用strlen(json_data)，不含结束符）
 * @return ESP_OK: 成功; 其他: 失败
 */
esp_err_t ws_send_json(const char *json_data, size_t len);

/**
 * @brief 向服务器发送二进制数据（WebSocket二进制帧）
 * @param binary_data: 二进制数据缓冲区（如PCM音频、二进制文件内容）
 * @param len: 二进制数据长度（字节数）
 * @return ESP_OK: 成功; 其他: 失败
 */
esp_err_t ws_send_binary(const void *binary_data, size_t len);

/**
 * @brief 注册接收数据处理函数
 * @param handler: 自定义处理函数指针，格式：void func(const char *data, size_t len)
 * 说明：收到服务器数据时，会自动调用此函数，data为接收的字符串，len为数据长度
 */
void ws_register_recv_handler(void (*handler)(const char *data, size_t len));

/**
 * @brief 获取连接状态
 * @return ESP_OK: 已连接; ESP_FAIL: 未连接或其他错误
 */
esp_err_t is_ws_connected(void);

/**
 * @brief 停止WebSocket客户端并释放资源
 */
void ws_stop(void);

// 在文件末尾添加
/**
 * @brief 立即尝试重连服务器
 * @return ESP_OK: 成功; 其他: 失败
 */
esp_err_t ws_reconnect_now(void);

/**
 * @brief 获取当前重连计数
 * @return 当前重连次数
 */
int ws_get_reconnect_count(void);

/**
 * @brief 重置重连计数
 */
void ws_reset_reconnect_count(void);

#endif // WEBSOCKET_H