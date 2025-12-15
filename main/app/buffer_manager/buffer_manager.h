#ifndef BUFFER_MANAGER_H
#define BUFFER_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// 缓冲区配置（可在创建实例时自定义）
typedef struct
{
    int buffer_count;   // 缓冲区数量（如8）
    size_t buffer_size; // 单个缓冲区大小（如960字节）
    int full_threshold; // 触发is_full的阈值（如5）
} BufferConfig;

// 内部结构体定义
struct BufferManager {
    uint8_t** buffers;      // 缓冲区数组（动态分配）
    int buffer_count;       // 缓冲区数量
    size_t buffer_size;     // 单个缓冲区大小
    int full_threshold;     // 满阈值
    volatile int current_pos; // 当前填充位置
    volatile int fill_count;  // 已填充数量
    volatile int read_pos;    // 当前读取位置
    volatile bool is_full;    // 是否达到阈值
    volatile bool is_empty;   // 是否为空
    portMUX_TYPE mux;         // ESP-IDF 临界区锁（自旋锁）
};

// 缓冲区管理器结构体（对外仅声明，隐藏内部实现）
typedef struct BufferManager BufferManager;

/**
 * @brief 创建缓冲区管理器实例
 * @param config 缓冲区配置（包含数量、大小、阈值）
 * @return 成功返回实例指针，失败返回NULL
 */
BufferManager *buffer_manager_create(const BufferConfig *config);

/**
 * @brief 销毁缓冲区管理器实例
 * @param mgr 要销毁的实例指针
 */
void buffer_manager_destroy(BufferManager *mgr);

/**
 * @brief 向指定缓冲区添加数据
 * @param mgr 缓冲区实例指针
 * @param data 要添加的数据指针（非空）
 * @param len 数据长度（需 ≤ 单个缓冲区大小，且 > 0）
 * @return true：添加成功；false：参数无效或数据过长
 */
bool buffer_add(BufferManager *mgr, const uint8_t *data, size_t len);

/**
 * @brief 从指定缓冲区取出数据
 * @param mgr 缓冲区实例指针
 * @param data 接收数据的缓冲区指针（非空）
 * @param len 期望读取的长度（需 ≤ 单个缓冲区大小，且 > 0）
 * @return 实际读取的字节数（0表示无新数据）
 */
size_t buffer_get(BufferManager *mgr, uint8_t *data, size_t len);

/**
 * @brief 获取指定缓冲区的当前状态
 * @param mgr 缓冲区实例指针
 * @param current_pos 输出当前填充位置（可选，传NULL忽略）
 * @param read_pos 输出当前读取位置（可选，传NULL忽略）
 * @param fill_count 输出已填充缓冲区数量（可选，传NULL忽略）
 * @param is_full 输出是否达到阈值（可选，传NULL忽略）
 * @param is_empty 输出是否为空（可选，传NULL忽略）
 */
void buffer_get_status(BufferManager *mgr, int *current_pos, int *read_pos,
                       int *fill_count, bool *is_full, bool *is_empty);

#endif // BUFFER_MANAGER_H