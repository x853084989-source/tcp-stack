#!/usr/bin/env bash
# 重启后恢复 DPDK 环境，等价于 dpdk-setup.sh 中的 43、45、46(512)、49。
set -euo pipefail

export RTE_SDK=/home/user/code/2.network/2.4/dpdk/dpdk-stable-19.08.2
export RTE_TARGET=x86_64-native-linux-gcc

DPDK_PCI=0000:0b:00.0
SSH_PCI=0000:03:00.0
HUGEPAGES=512
USTACK_DIR=/home/user/code/2.network/2.4/dpdk/ustack
DPDK_BIND="$RTE_SDK/usertools/dpdk-devbind.py"

die() {
    printf '错误：%s\n' "$*" >&2
    exit 1
}

usage() {
    printf '用法：%s [--setup-only]\n' "$0"
    printf '默认配置 DPDK 并运行 ustack；--setup-only 只配置环境。\n'
}

case "${1:-}" in
    '') ;;
    --setup-only) SETUP_ONLY=1 ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
esac
[[ $# -le 1 ]] || { usage >&2; exit 2; }

[[ -f "$RTE_SDK/$RTE_TARGET/.config" ]] || die "未找到已构建的 DPDK 目标；请检查 $RTE_TARGET"
[[ -f "$DPDK_BIND" ]] || die "未找到 dpdk-devbind.py"
[[ -x "$USTACK_DIR/build/ustack" ]] || die "未找到可执行文件 $USTACK_DIR/build/ustack"
[[ -d "/sys/bus/pci/devices/$DPDK_PCI" ]] || die "未找到业务网卡 $DPDK_PCI"
[[ -d "/sys/bus/pci/devices/$SSH_PCI" ]] || die "未找到 SSH 管理网卡 $SSH_PCI，PCI 布局可能已变化"
[[ "$(cat "/sys/bus/pci/devices/$DPDK_PCI/class")" == 0x020000 ]] || die "$DPDK_PCI 不是以太网设备"
[[ -d "/lib/modules/$(uname -r)/build" ]] || die "缺少当前内核 $(uname -r) 的头文件"

# 如果默认路由正好走业务网卡，停止绑定，避免断开远程连接。
default_iface=$(ip -4 route show default | awk '{for (i=1; i<NF; i++) if ($i=="dev") {print $(i+1); exit}}')
if [[ -n "$default_iface" && -e "/sys/class/net/$default_iface/device" ]]; then
    default_pci=$(basename "$(readlink -f "/sys/class/net/$default_iface/device")")
    [[ "$default_pci" != "$DPDK_PCI" ]] || die "$DPDK_PCI 正在承载默认路由，已停止绑定"
fi
for net_path in "/sys/bus/pci/devices/$DPDK_PCI/net/"*; do
    [[ -e "$net_path" ]] || continue
    net_iface=${net_path##*/}
    if ip -o addr show dev "$net_iface" | awk '$3 == "inet" || $3 == "inet6" { found=1 } END { exit !found }'; then
        die "$DPDK_PCI 的接口 $net_iface 配置了 IP 地址，已停止绑定"
    fi
done

# 内核更新后，仅重编对应的两个模块；不重新构建整个 DPDK（菜单 39）。
ensure_module_for_kernel() {
    local name=$1 source_dir=$2 module="$RTE_SDK/$RTE_TARGET/kmod/$1.ko"
    local built_for=''
    if [[ -f "$module" ]]; then
        built_for=$(modinfo -F vermagic "$module")
        built_for=${built_for%% *}
    fi
    if [[ "$built_for" != "$(uname -r)" ]]; then
        printf '重编 %s：%s -> %s\n' "$name" "${built_for:-未构建}" "$(uname -r)"
        make -C "$RTE_SDK" O="$RTE_SDK/$RTE_TARGET" RTE_MAKE_SUBTARGET=clean "${source_dir}_sub"
        make -C "$RTE_SDK" O="$RTE_SDK/$RTE_TARGET" "${source_dir}_sub"
        built_for=$(modinfo -F vermagic "$module")
        [[ "${built_for%% *}" == "$(uname -r)" ]] || die "$name 的内核版本仍不匹配"
    fi
}

ensure_module_for_kernel igb_uio kernel/linux/igb_uio
ensure_module_for_kernel rte_kni kernel/linux/kni

# sudo 会在需要时询问密码；脚本内部已设置 RTE_SDK 和 RTE_TARGET。
sudo -v

# 菜单 43：加载 IGB UIO。已经加载时不卸载，避免影响正在使用它的设备。
sudo modprobe uio
if [[ ! -d /sys/module/igb_uio ]]; then
    sudo insmod "$RTE_SDK/$RTE_TARGET/kmod/igb_uio.ko"
fi

# 菜单 45：加载 KNI。
if [[ ! -d /sys/module/rte_kni ]]; then
    sudo insmod "$RTE_SDK/$RTE_TARGET/kmod/rte_kni.ko"
fi

# 菜单 46：保留 512 个默认大小的 hugepages，并挂载 /mnt/huge。
page_size_kb=$(awk '/^Hugepagesize:/ {print $2}' /proc/meminfo)
pages_file="/sys/kernel/mm/hugepages/hugepages-${page_size_kb}kB/nr_hugepages"
[[ -f "$pages_file" ]] || die "找不到 hugepages 配置文件 $pages_file"
if [[ "$(cat "$pages_file")" != "$HUGEPAGES" ]]; then
    printf '%s\n' "$HUGEPAGES" | sudo tee "$pages_file" >/dev/null
fi
[[ "$(cat "$pages_file")" == "$HUGEPAGES" ]] || die "未能保留 $HUGEPAGES 个 hugepages"
sudo mkdir -p /mnt/huge
mount_type=$(findmnt -n -o FSTYPE --mountpoint /mnt/huge || true)
case "$mount_type" in
    '') sudo mount -t hugetlbfs nodev /mnt/huge ;;
    hugetlbfs) ;;
    *) die "/mnt/huge 已挂载为 $mount_type，无法用于 DPDK" ;;
esac

# 菜单 49：只绑定业务网卡；绝不把 SSH 网卡传给 devbind。
current_driver=''
if [[ -e "/sys/bus/pci/devices/$DPDK_PCI/driver" ]]; then
    current_driver=$(basename "$(readlink -f "/sys/bus/pci/devices/$DPDK_PCI/driver")")
fi
if [[ "$current_driver" != igb_uio ]]; then
    sudo python3 "$DPDK_BIND" --bind=igb_uio "$DPDK_PCI"
fi
[[ -e "/sys/bus/pci/devices/$DPDK_PCI/driver" ]] || die "$DPDK_PCI 未绑定到 igb_uio"
[[ "$(basename "$(readlink -f "/sys/bus/pci/devices/$DPDK_PCI/driver")")" == igb_uio ]] || die "$DPDK_PCI 未绑定到 igb_uio"

printf 'DPDK 环境已就绪：RTE_SDK=%s，RTE_TARGET=%s，业务网卡=%s\n' "$RTE_SDK" "$RTE_TARGET" "$DPDK_PCI"
if [[ "${SETUP_ONLY:-0}" == 1 ]]; then
    exit 0
fi

cd "$USTACK_DIR"
sudo env RTE_SDK="$RTE_SDK" RTE_TARGET="$RTE_TARGET" ./build/ustack
