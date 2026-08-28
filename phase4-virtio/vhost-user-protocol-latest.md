# Vhost-user 协议规范（最新版分析）

> 基于 QEMU 官方文档（2024/2025）
> 
> 来源: https://qemu-project.gitlab.io/qemu/interop/vhost-user.html

---

## 📋 概述

vhost-user 协议旨在补充 Linux 内核中用于控制 vhost 实现的 `ioctl` 接口。它实现了与同一主机上的用户空间进程共享 virtqueue 所需的控制平面。它使用 Unix 域套接字上的通信在消息的辅助数据中共享文件描述符。

协议定义了通信的 2 个端点：
- **前端 (front-end)**: 共享其 virtqueue 的应用程序（在我们的例子中是 QEMU）
- **后端 (back-end)**: virtqueue 的消费者

---

## 🔧 协议特性 (Protocol Features)

### 基础特性 (0-12)

```c
#define VHOST_USER_PROTOCOL_F_MQ                       0   // 多队列支持
#define VHOST_USER_PROTOCOL_F_LOG_SHMFD                1   // 日志共享内存文件描述符
#define VHOST_USER_PROTOCOL_F_RARP                     2   // RARP 支持
#define VHOST_USER_PROTOCOL_F_REPLY_ACK                3   // 回复确认
#define VHOST_USER_PROTOCOL_F_MTU                      4   // MTU 设置
#define VHOST_USER_PROTOCOL_F_BACKEND_REQ              5   // 后端请求
#define VHOST_USER_PROTOCOL_F_CROSS_ENDIAN             6   // 跨 endian 支持
#define VHOST_USER_PROTOCOL_F_CRYPTO_SESSION           7   // 加密会话
#define VHOST_USER_PROTOCOL_F_PAGEFAULT                8   // 缺页处理
#define VHOST_USER_PROTOCOL_F_CONFIG                   9   // 配置空间
#define VHOST_USER_PROTOCOL_F_BACKEND_SEND_FD         10   // 后端发送文件描述符
#define VHOST_USER_PROTOCOL_F_HOST_NOTIFIER           11   // 主机通知器
#define VHOST_USER_PROTOCOL_F_INFLIGHT_SHMFD          12   // 共享内存 inflight 追踪
```

### ✨ 新增特性 (13-22)

```c
#define VHOST_USER_PROTOCOL_F_RESET_DEVICE            13   // ✨ 设备重置
#define VHOST_USER_PROTOCOL_F_INBAND_NOTIFICATIONS    14   // ✨ 带内通知
#define VHOST_USER_PROTOCOL_F_CONFIGURE_MEM_SLOTS     15   // ✨ 配置内存插槽
#define VHOST_USER_PROTOCOL_F_STATUS                  16   // ✨ 设备状态查询
#define VHOST_USER_PROTOCOL_F_XEN_MMAP                17   // ✨ Xen 内存映射
#define VHOST_USER_PROTOCOL_F_SHARED_OBJECT           18   // ✨ 共享对象管理
#define VHOST_USER_PROTOCOL_F_DEVICE_STATE            19   // ✨ 设备状态管理
#define VHOST_USER_PROTOCOL_F_GET_VRING_BASE_INFLIGHT 20   // ✨ inflight 追踪的 vring base
#define VHOST_USER_PROTOCOL_F_GPA_ADDRESSES           21   // ✨ GPA 地址管理
#define VHOST_USER_PROTOCOL_F_SHMEM_MAP               22   // ✨ 共享内存映射
```

---

## 📨 前端消息类型 (Front-end Message Types)

### 基础控制消息

| 消息 | 说明 |
|------|------|
| `VHOST_USER_GET_FEATURES` | 获取支持的特性位掩码 |
| `VHOST_USER_SET_FEATURES` | 设置启用的特性 |
| `VHOST_USER_GET_PROTOCOL_FEATURES` | 获取协议特性 |
| `VHOST_USER_SET_PROTOCOL_FEATURES` | 设置协议特性 |
| `VHOST_USER_SET_OWNER` | 设置所有者 |
| `VHOST_USER_RESET_OWNER` | 重置所有者 |

### 内存管理消息

| 消息 | 说明 | 新增 |
|------|------|------|
| `VHOST_USER_SET_MEM_TABLE` | 设置内存表 | |
| `VHOST_USER_GET_MAX_MEM_SLOTS` | 获取最大内存插槽数 | ✨ |
| `VHOST_USER_ADD_MEM_REG` | 添加内存区域 | ✨ |
| `VHOST_USER_REM_MEM_REG` | 移除内存区域 | ✨ |

### Vring 管理消息

| 消息 | 说明 |
|------|------|
| `VHOST_USER_SET_VRING_NUM` | 设置 vring 大小 |
| `VHOST_USER_SET_VRING_ADDR` | 设置 vring 地址 |
| `VHOST_USER_SET_VRING_BASE` | 设置 vring base |
| `VHOST_USER_GET_VRING_BASE` | 获取 vring base |
| `VHOST_USER_SET_VRING_KICK` | 设置 kick 文件描述符 |
| `VHOST_USER_SET_VRING_CALL` | 设置 call 文件描述符 |
| `VHOST_USER_SET_VRING_ERR` | 设置错误文件描述符 |
| `VHOST_USER_SET_VRING_ENABLE` | 启用/禁用 vring |
| `VHOST_USER_SET_VRING_ENDIAN` | 设置 vring endian |

### 设备状态消息

| 消息 | 说明 | 新增 |
|------|------|------|
| `VHOST_USER_GET_STATUS` | 获取设备状态 | ✨ |
| `VHOST_USER_SET_STATUS` | 设置设备状态 | ✨ |
| `VHOST_USER_CHECK_DEVICE_STATE` | 检查设备状态 | ✨ |
| `VHOST_USER_RESET_DEVICE` | 重置设备 | ✨ |

### 配置空间消息

| 消息 | 说明 |
|------|------|
| `VHOST_USER_GET_CONFIG` | 获取配置空间 |
| `VHOST_USER_SET_CONFIG` | 设置配置空间 |

### 网络特定消息

| 消息 | 说明 |
|------|------|
| `VHOST_USER_NET_SET_MTU` | 设置 MTU |
| `VHOST_USER_SEND_RARP` | 发送 RARP |

### 加密会话消息

| 消息 | 说明 |
|------|------|
| `VHOST_USER_CREATE_CRYPTO_SESSION` | 创建加密会话 |
| `VHOST_USER_CLOSE_CRYPTO_SESSION` | 关闭加密会话 |

### Postcopy 迁移消息

| 消息 | 说明 |
|------|------|
| `VHOST_USER_POSTCOPY_ADVISE` | Postcopy 建议 |
| `VHOST_USER_POSTCOPY_LISTEN` | Postcopy 监听 |
| `VHOST_USER_POSTCOPY_END` | Postcopy 结束 |

### 共享内存消息

| 消息 | 说明 | 新增 |
|------|------|------|
| `VHOST_USER_GET_SHMEM_CONFIG` | 获取共享内存配置 | ✨ |
| `VHOST_USER_GET_SHARED_OBJECT` | 获取共享对象 | ✨ |

### Inflight 追踪消息

| 消息 | 说明 | 新增 |
|------|------|------|
| `VHOST_USER_GET_INFLIGHT_FD` | 获取 inflight 文件描述符 | ✨ |
| `VHOST_USER_SET_INFLIGHT_FD` | 设置 inflight 文件描述符 | ✨ |
| `VHOST_USER_GET_VRING_BASE_INFLIGHT` | 获取 inflight 的 vring base | ✨ |

---

## 📨 后端消息类型 (Back-end Message Types)

> 注意：这些消息之前称为 "SLAVE" 消息，现已重命名为 "BACKEND"

| 消息 | 旧名称 | 说明 |
|------|--------|------|
| `VHOST_USER_BACKEND_CONFIG_CHANGE_MSG` | `VHOST_USER_SLAVE_CONFIG_CHANGE_MSG` | 配置变更通知 |
| `VHOST_USER_BACKEND_IOTLB_MSG` | `VHOST_USER_SLAVE_IOTLB_MSG` | IOTLB 消息 |
| `VHOST_USER_BACKEND_VRING_CALL` | `VHOST_USER_SLAVE_VRING_CALL` | Vring call 通知 |
| `VHOST_USER_BACKEND_VRING_ERR` | `VHOST_USER_SLAVE_VRING_ERR` | Vring 错误通知 |
| `VHOST_USER_BACKEND_VRING_HOST_NOTIFIER_MSG` | `VHOST_USER_SLAVE_VRING_HOST_NOTIFIER_MSG` | 主机通知器消息 |
| `VHOST_USER_BACKEND_SHMEM_MAP` | - | 共享内存映射 | ✨ |
| `VHOST_USER_BACKEND_SHMEM_UNMAP` | - | 共享内存取消映射 | ✨ |
| `VHOST_USER_BACKEND_SHARED_OBJECT_ADD` | - | 添加共享对象 | ✨ |
| `VHOST_USER_BACKEND_SHARED_OBJECT_REMOVE` | - | 移除共享对象 | ✨ |
| `VHOST_USER_BACKEND_SHARED_OBJECT_LOOKUP` | - | 查找共享对象 | ✨ |

---

## ✨ 新增特性详细分析

### 1. 设备状态管理 (VHOST_USER_PROTOCOL_F_DEVICE_STATE)

**特性位**: 19

**功能**:
- `VHOST_USER_GET_STATUS`: 查询设备当前状态
- `VHOST_USER_SET_STATUS`: 设置设备状态
- `VHOST_USER_CHECK_DEVICE_STATE`: 检查设备状态是否就绪

**应用场景**:
- 设备状态迁移
- 设备健康检查
- 设备状态恢复

### 2. 带内通知 (VHOST_USER_PROTOCOL_F_INBAND_NOTIFICATIONS)

**特性位**: 14

**功能**:
- 允许通过主通信通道发送通知
- 减少对额外文件描述符的依赖
- 简化连接管理

**优势**:
- 减少文件描述符数量
- 简化后端实现
- 提高连接可靠性

### 3. 内存插槽配置 (VHOST_USER_PROTOCOL_F_CONFIGURE_MEM_SLOTS)

**特性位**: 15

**功能**:
- 动态配置内存插槽
- 支持热插拔内存
- 灵活的内存管理

**相关消息**:
- `VHOST_USER_GET_MAX_MEM_SLOTS`: 获取最大内存插槽数
- `VHOST_USER_ADD_MEM_REG`: 添加内存区域
- `VHOST_USER_REM_MEM_REG`: 移除内存区域

### 4. 共享对象管理 (VHOST_USER_PROTOCOL_F_SHARED_OBJECT)

**特性位**: 18

**功能**:
- 管理共享对象（如共享内存区域）
- 支持对象的添加、移除和查找

**相关消息**:
- `VHOST_USER_BACKEND_SHARED_OBJECT_ADD`: 添加共享对象
- `VHOST_USER_BACKEND_SHARED_OBJECT_REMOVE`: 移除共享对象
- `VHOST_USER_BACKEND_SHARED_OBJECT_LOOKUP`: 查找共享对象
- `VHOST_USER_GET_SHARED_OBJECT`: 获取共享对象信息

### 5. Inflight 追踪增强 (VHOST_USER_PROTOCOL_F_GET_VRING_BASE_INFLIGHT)

**特性位**: 20

**功能**:
- 支持 inflight I/O 追踪
- 支持实时迁移期间的 I/O 恢复
- 提高迁移可靠性

**相关消息**:
- `VHOST_USER_GET_INFLIGHT_FD`: 获取 inflight 追踪的文件描述符
- `VHOST_USER_SET_INFLIGHT_FD`: 设置 inflight 追踪的文件描述符
- `VHOST_USER_GET_VRING_BASE_INFLIGHT`: 获取 inflight 追踪的 vring base

### 6. GPA 地址管理 (VHOST_USER_PROTOCOL_F_GPA_ADDRESSES)

**特性位**: 21

**功能**:
- 管理 Guest 物理地址 (GPA)
- 支持 GPA 到 HVA 的映射
- 支持内存热插拔

### 7. 共享内存映射 (VHOST_USER_PROTOCOL_F_SHMEM_MAP)

**特性位**: 22

**功能**:
- 支持共享内存的映射和取消映射
- 支持动态内存管理

**相关消息**:
- `VHOST_USER_BACKEND_SHMEM_MAP`: 映射共享内存
- `VHOST_USER_BACKEND_SHMEM_UNMAP`: 取消映射共享内存

---

## 🔄 消息重命名历史

为了提高清晰度，以下消息已重命名：

| 旧名称 | 新名称 | 原因 |
|--------|--------|------|
| `VHOST_USER_SLAVE_CONFIG_CHANGE_MSG` | `VHOST_USER_BACKEND_CONFIG_CHANGE_MSG` | 更准确的命名 |
| `VHOST_USER_SLAVE_IOTLB_MSG` | `VHOST_USER_BACKEND_IOTLB_MSG` | 统一术语 |
| `VHOST_USER_SLAVE_VRING_CALL` | `VHOST_USER_BACKEND_VRING_CALL` | 统一术语 |
| `VHOST_USER_SLAVE_VRING_ERR` | `VHOST_USER_BACKEND_VRING_ERR` | 统一术语 |
| `VHOST_USER_SLAVE_VRING_HOST_NOTIFIER_MSG` | `VHOST_USER_BACKEND_VRING_HOST_NOTIFIER_MSG` | 统一术语 |
| `VHOST_USER_SET_SLAVE_REQ_FD` | `VHOST_USER_SET_BACKEND_REQ_FD` | 统一术语 |

**原因**: "SLAVE" 术语已被 "BACKEND" 替代，以使用更现代和包容的术语。

---

## 📊 协议演进时间线

### 早期版本 (特性 0-12)
- 基础功能支持
- 多队列、日志、配置空间
- 加密会话、缺页处理

### 中期版本 (特性 13-16)
- **设备重置** (13): 支持设备完全重置
- **带内通知** (14): 简化连接管理
- **内存插槽配置** (15): 动态内存管理
- **设备状态** (16): 状态查询和管理

### 最新版本 (特性 17-22)
- **Xen 内存映射** (17): Xen  hypervisor 支持
- **共享对象管理** (18): 灵活的共享资源管理
- **设备状态管理** (19): 增强的状态管理
- **Inflight 追踪** (20): 实时迁移支持
- **GPA 地址管理** (21): 内存管理增强
- **共享内存映射** (22): 动态内存映射

---

## 🎯 关键改进总结

### 1. 实时迁移支持增强
- Inflight I/O 追踪
- 设备状态管理
- 共享内存动态管理

### 2. 内存管理灵活性
- 动态内存插槽配置
- 共享内存映射/取消映射
- GPA 地址管理

### 3. 连接简化
- 带内通知减少文件描述符
- 统一术语提高代码可读性

### 4. 设备管理增强
- 设备状态查询和设置
- 设备重置支持
- 健康检查机制

---

## 💡 实践建议

### 对于 KVM 开发者
1. **实现新特性**: 优先实现设备状态管理和 inflight 追踪
2. **内存管理**: 利用新的内存插槽配置特性
3. **迁移支持**: 利用 inflight 追踪提高迁移可靠性

### 对于 QEMU 开发者
1. **更新消息名称**: 使用新的 BACKEND 前缀
2. **实现新特性**: 支持共享对象管理和 GPA 地址管理
3. **测试新特性**: 重点测试实时迁移和设备状态管理

### 对于后端开发者
1. **支持新特性**: 实现设备状态管理和 inflight 追踪
2. **更新协议**: 使用新的消息名称
3. **测试兼容性**: 确保向后兼容

---

## 📚 参考资源

- [QEMU vhost-user 协议文档](https://qemu-project.gitlab.io/qemu/interop/vhost-user.html)
- [OASIS VIRTIO 规范](https://docs.oasis-open.org/virtio/virtio/v1.3/csd01/virtio-v1.3-csd01.html)
- [OASIS VIRTIO TC GitHub](https://github.com/oasis-tcs/virtio-spec)
- [SPDK vhost-user 文档](https://spdk.io/doc/vhost_processing.html)

---

## 🔍 总结

最新的 vhost-user 协议在以下方面有显著改进：

1. **实时迁移**: 通过 inflight 追踪和设备状态管理，大幅提高迁移可靠性
2. **内存管理**: 动态内存插槽和共享内存映射，提供更灵活的内存管理
3. **连接简化**: 带内通知减少文件描述符依赖
4. **术语现代化**: 从 SLAVE 到 BACKEND 的术语更新

这些改进使 vhost-user 协议更加现代化、灵活和可靠，特别适合云计算和虚拟化场景。
