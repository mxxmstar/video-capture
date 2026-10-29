#!/usr/bin/env python3
"""Run on NanoPC-T4: export target headers, startup files and core libraries."""
import argparse
import pathlib
import subprocess
import tarfile

# 唯一的参数：导出包的输出路径（例如 /tmp/nanopc-t4-sysroot.tar.gz）
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("output", help="Output .tar.gz path, e.g. /tmp/nanopc-t4-sysroot.tar.gz")
args = parser.parse_args()

# 平台自检：必须在板子上运行。主机的 Ubuntu 22.04 是 glibc 2.35 + GCC 12，
# 而板子是 20.04 的 glibc 2.31 + GCC 10；用主机的运行库去链接板上的程序
# 会产生符号版本不匹配（如依赖 GLIBC_2.34），程序在板上无法启动。
if subprocess.check_output(["uname", "-m"], text=True).strip() != "aarch64":
    parser.error("Run this exporter on the AArch64 target, not the x86_64 VM")

# 目标机的 C 运行库、启动文件和动态链接器所在目录
library_root = pathlib.Path("/usr/lib/aarch64-linux-gnu")

# 需要收集的库文件名前缀，按用途分组：
#   crt/Scrt/Mcrt/rcrt -> crt1.o、Scrt1.o(PIE)、crti.o、crtn.o：程序入口与 .init/.fini 段
#   libc./libc-/libc_  -> libc.so.6、libc-2.31.so、libc_nonshared.a：C 运行库本体
#   libm./libm-/libmvec -> 数学库及其向量化版本
#   libdl/libpthread/librt./librt-/libutil/libresolv/libanl -> 动态加载、线程、定时器、
#                                                              终端、DNS 解析、异步 DNS
#   ld-               -> ld-linux-aarch64.so.1：动态链接器，交叉链接时必须存在
#   libstdc++         -> C++ 标准库运行库
#   libgcc_s/libatomic -> GCC 运行时（异常展开、128 位原子操作）
#   librockchip_mpp/librga -> 板子特有的硬件编解码与 2D 加速库
prefixes = (
    "crt", "Scrt", "Mcrt", "rcrt", "libc.", "libc-", "libc_",
    "libm.", "libm-", "libmvec", "libdl", "libpthread", "librt.",
    "librt-", "libutil", "libresolv", "libanl", "ld-", "libstdc++",
    "libgcc_s", "libatomic", "librockchip_mpp", "librga",
)

# 必需路径：/usr/include 提供 glibc 与 libstdc++ 头文件；
# /usr/lib/gcc/.../10 提供 GCC 内建头（stddef.h、stdint-gcc.h、arm_neon.h 等）
# 以及 libgcc.a / libgcc_s.so，这些都不在 /usr/include 里。
paths = [pathlib.Path("/usr/include"), pathlib.Path("/usr/lib/gcc/aarch64-linux-gnu/10")]

# 只取库目录下的顶层条目（不递归），按前缀筛选。
# 同时接受符号链接：libc.so.6 等通常是链接，只保留链接本身即可满足链接器。
paths += sorted(p for p in library_root.iterdir()
                if (p.is_file() or p.is_symlink()) and p.name.startswith(prefixes))

# 完整性校验：任一必需路径缺失就立即失败，避免产出残缺的 sysroot。
# 用 is_symlink() 兜底，处理“链接指向的文件位于其他目录”的情况。
for path in paths:
    if not path.exists() and not path.is_symlink():
        raise SystemExit(f"Missing target development files: {path}")

# 打包为 tar.gz。arcname 去掉开头的 "/"，让解压后直接得到以 sysroot 为根的目录树：
#   usr/include/...、usr/lib/aarch64-linux-gnu/...、usr/lib/gcc/.../10/...
# tarfile 默认不跟随符号链接（dereference=False），保留链接结构符合 sysroot 的预期。
with tarfile.open(args.output, "w:gz") as archive:
    for path in paths:
        archive.add(str(path), arcname=str(path).lstrip("/"))

# 打印产物大小，便于确认导出是否合理（通常几十到上百 MB）
print(f"Exported {args.output}: {pathlib.Path(args.output).stat().st_size} bytes")