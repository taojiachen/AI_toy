#include "buffer_manager.h"
#include <stdlib.h>
#include <string.h>

// 临界区宏定义（适配 ESP-IDF FreeRTOS）
#define PORT_ENTER_CRITICAL(mgr) portENTER_CRITICAL_SAFE(&(mgr)->mux)
#define PORT_EXIT_CRITICAL(mgr) portEXIT_CRITICAL_SAFE(&(mgr)->mux)

/**
 * 创建缓冲区实例
 */
BufferManager* buffer_manager_create(const BufferConfig* config) {
    // 参数校验
    if (config == NULL || config->buffer_count <= 0 || 
        config->buffer_size <= 0 || config->full_threshold <= 0) {
        return NULL;
    }

    // 分配管理器结构体内存
    BufferManager* mgr = (BufferManager*)malloc(sizeof(BufferManager));
    if (!mgr) {
        return NULL;
    }

    // 初始化管理器所有成员（先清0）
    memset(mgr, 0, sizeof(BufferManager));

    // 初始化核心参数
    mgr->buffer_count = config->buffer_count;
    mgr->buffer_size = config->buffer_size;
    mgr->full_threshold = config->full_threshold;
    mgr->current_pos = 0;
    mgr->fill_count = 0;
    mgr->read_pos = 0;
    mgr->is_full = false;
    mgr->is_empty = true;

    // 关键修复：手动初始化 ESP-IDF 自旋锁（替代宏赋值）
    // ESP-IDF 中 portMUX_TYPE 对应 spinlock_t，成员为 owner 和 count
    mgr->mux.owner = SPINLOCK_FREE;  // 初始化为未占用状态
    mgr->mux.count = 0;              // 锁计数置0

    // 分配缓冲区数组内存
    mgr->buffers = (uint8_t**)malloc(sizeof(uint8_t*) * mgr->buffer_count);
    if (!mgr->buffers) {
        free(mgr);
        return NULL;
    }

    // 为每个缓冲区分配内存
    for (int i = 0; i < mgr->buffer_count; i++) {
        mgr->buffers[i] = (uint8_t*)malloc(mgr->buffer_size);
        if (!mgr->buffers[i]) {
            // 释放已分配的缓冲区，避免内存泄漏
            for (int j = 0; j < i; j++) {
                free(mgr->buffers[j]);
            }
            free(mgr->buffers);
            free(mgr);
            return NULL;
        }
    }

    return mgr;
}

/**
 * 销毁缓冲区实例
 */
void buffer_manager_destroy(BufferManager* mgr) {
    if (!mgr) {
        return;
    }

    // 释放所有缓冲区内存
    if (mgr->buffers) {
        for (int i = 0; i < mgr->buffer_count; i++) {
            free(mgr->buffers[i]);
        }
        free(mgr->buffers);
    }

    // 释放管理器结构体
    free(mgr);
}

/**
 * 向缓冲区添加数据
 */
bool buffer_add(BufferManager* mgr, const uint8_t* data, size_t len) {
    if (!mgr || !data || len == 0 || len > mgr->buffer_size) {
        return false;
    }

    // 进入临界区（线程安全保护）
    PORT_ENTER_CRITICAL(mgr);

    // 复制数据到当前缓冲区
    memcpy(mgr->buffers[mgr->current_pos], data, len);

    // 计算下一个填充位置（循环）
    int next_current = (mgr->current_pos + 1) % mgr->buffer_count;

    // 更新填充计数
    if (mgr->fill_count < mgr->buffer_count) {
        mgr->fill_count++;
    } else {
        // 缓冲区满，移动读指针丢弃最旧数据
        mgr->read_pos = next_current;
    }

    // 更新当前填充位置
    mgr->current_pos = next_current;

    // 更新满阈值标志
    mgr->is_full = (mgr->fill_count >= mgr->full_threshold);
    mgr->is_empty = false;

    // 退出临界区
    PORT_EXIT_CRITICAL(mgr);
    return true;
}

/**
 * 从缓冲区读取数据
 */
size_t buffer_get(BufferManager* mgr, uint8_t* data, size_t len) {
    if (!mgr || !data || len == 0 || len > mgr->buffer_size) {
        return 0;
    }

    // 进入临界区
    PORT_ENTER_CRITICAL(mgr);

    // 无新数据可读（读指针追上写指针）
    if (mgr->fill_count == 0) {
        PORT_EXIT_CRITICAL(mgr);
        return 0;
    }

    // 复制数据到接收缓冲区
    size_t copy_len = (len > mgr->buffer_size) ? mgr->buffer_size : len;
    memcpy(data, mgr->buffers[mgr->read_pos], copy_len);

    // 更新读指针和填充计数
    mgr->read_pos = (mgr->read_pos + 1) % mgr->buffer_count;
    mgr->fill_count--;

    // 更新满阈值标志
    mgr->is_full = (mgr->fill_count >= mgr->full_threshold);
    mgr->is_empty = (mgr->fill_count == 0);

    // 退出临界区
    PORT_EXIT_CRITICAL(mgr);
    return copy_len;
}

/**
 * 获取缓冲区状态
 */
void buffer_get_status(BufferManager* mgr, int* current_pos, int* read_pos,
                      int* fill_count, bool* is_full, bool* is_empty) {
    if (!mgr) {
        return;
    }

    PORT_ENTER_CRITICAL(mgr);
    if (current_pos) *current_pos = mgr->current_pos;
    if (read_pos) *read_pos = mgr->read_pos;
    if (fill_count) *fill_count = mgr->fill_count;
    if (is_full) *is_full = mgr->is_full;
    if (is_empty) *is_empty = mgr->is_empty;
    PORT_EXIT_CRITICAL(mgr);
}