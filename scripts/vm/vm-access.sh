#!/bin/bash
# VM 访问辅助脚本
# 用法: ./vm-access.sh [serial|ssh|status|stop]

VM_NAME="kvm-study"
VM_LOG="/tmp/vm-boot.log"

case "$1" in
  serial)
    echo "通过串口连接到 VM..."
    echo "提示: 使用 Ctrl-A X 退出"
    echo ""
    # 找到 QEMU 进程的 PID
    PID=$(pgrep -f "qemu.*$VM_NAME")
    if [ -z "$PID" ]; then
      echo "错误: VM 未运行"
      exit 1
    fi
    # 使用 socat 连接到 QEMU 的 monitor
    if command -v socat &> /dev/null; then
      socat -,raw,echo=0 UNIX-CONNECT:/tmp/qemu-$PID.monitor
    else
      echo "错误: 需要安装 socat (apt install socat)"
      exit 1
    fi
    ;;
  
  ssh)
    echo "通过 SSH 连接到 VM..."
    ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=10 -p 2222 root@localhost
    ;;
  
  status)
    echo "=== VM 状态 ==="
    PID=$(pgrep -f "qemu.*$VM_NAME")
    if [ -n "$PID" ]; then
      echo "✓ VM 正在运行 (PID: $PID)"
      ps -p $PID -o pid,pcpu,pmem,etime,cmd --no-headers
      echo ""
      echo "=== 端口监听 ==="
      ss -tlnp | grep $PID | awk '{print "  " $4 " (" $6 ")"}'
      echo ""
      echo "=== 最后 10 行日志 ==="
      tail -10 $VM_LOG 2>/dev/null || echo "  无日志文件"
    else
      echo "✗ VM 未运行"
    fi
    ;;
  
  stop)
    echo "停止 VM..."
    PID=$(pgrep -f "qemu.*$VM_NAME")
    if [ -n "$PID" ]; then
      kill $PID
      sleep 2
      if pgrep -f "qemu.*$VM_NAME" > /dev/null; then
        kill -9 $PID
      fi
      echo "✓ VM 已停止"
    else
      echo "VM 未运行"
    fi
    ;;
  
  *)
    echo "用法: $0 [serial|ssh|status|stop]"
    echo ""
    echo "命令:"
    echo "  serial  - 通过串口控制台连接 VM"
    echo "  ssh     - 通过 SSH 连接 VM (端口 2222)"
    echo "  status  - 显示 VM 状态"
    echo "  stop    - 停止 VM"
    exit 1
    ;;
esac
