# hasher – 高性能多算法哈希计算器

[![许可证](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![OpenSSL](https://img.shields.io/badge/OpenSSL-3.x-green.svg)](https://www.openssl.org/)
[![构建状态](https://github.com/GavinGe021/hasher/actions/workflows/build.yml/badge.svg)](https://github.com/GavinGe021/hasher/actions)

[English](README.md) | [英文文档](README.md)

`hasher` 是一款轻量级、高性能的命令行哈希计算工具。基于 OpenSSL 3.x EVP 接口，支持 20 余种哈希算法，具备异步 I/O、多文件并行处理和智能进度条等功能。

## 特性

- 多算法支持：MD4, MD5, SHA-1, SHA-2 系列, SHA-3, BLAKE2, SM3, RIPEMD-160, Whirlpool, Keccak 等。
- 高性能：采用双缓冲 + 异步 I/O 架构，充分发挥 CPU 与磁盘性能。
- 并行处理：通过 `--threads` 支持多文件并行计算。
- 智能进度条：单文件超过 100MB 时自动显示（可通过 `--progress`/`--no-progress` 控制）。
- SHAKE 可定制长度：通过 `--digest-bits` 灵活指定 SHAKE 输出比特数。
- 跨平台：Windows 与 Linux 均使用原生 I/O 接口，无性能损失。
- 详细输出：`--verbose` 显示速度、耗时，并为 `md5-sha1` 拆分显示两个哈希值。

## 安装方式

### 从源码编译

克隆仓库并编译：

    git clone https://github.com/GavinGe021/hasher.git
    cd hasher
    make

或手动编译：

    g++ -std=c++17 -O3 -pthread -o hasher hasher.cpp -lcrypto -lssl

**依赖项**：OpenSSL 3.x 开发库（`libssl-dev` / `openssl-devel`）。

### 下载预编译二进制（推荐）

从 [Releases 发布页](https://github.com/GavinGe021/hasher/releases) 下载对应平台的静态编译版本，解压即可使用。

### 包管理器安装（由社区维护）

- Windows (winget)：`winget install hasher`（待提交）
- Windows (Chocolatey)：`choco install hasher`
- Linux (APT)：后续将通过 PPA 提供
- macOS (Homebrew)：`brew install hasher`

## 使用说明

    hasher [选项] <输入> [算法] [<输入2> ...]

### 基础示例

    # 计算字符串 "hello" 的 SHA-256（默认算法）
    hasher hello

    # 计算文件的 SHA-256
    hasher --file myfile.iso

    # 自动检测（若存在同名文件则视为文件，否则视为字符串）
    hasher myfile

    # 指定算法（例如 SHA-512）
    hasher hello sha512

    # 并行计算多个文件（使用 4 个线程）
    hasher --threads=4 file1.iso file2.iso file3.iso

    # 自定义 SHAKE256 输出长度为 512 位
    hasher --digest-bits=512 --file data.bin shake256

    # 显示详细速度和耗时信息
    hasher --verbose largefile.bin

### 选项参数

| 选项 | 说明 |
| :--- | :--- |
| `--help, -h` | 显示帮助信息 |
| `--version, -V` | 显示版本号和 OpenSSL 版本信息 |
| `--file` | 强制将所有输入视为文件路径 |
| `--string` | 强制将所有输入视为文字字符串 |
| `--progress, -p` | 强制显示进度条（仅单文件模式） |
| `--no-progress` | 禁用进度条 |
| `--buffer-size=<MB>` | I/O 缓冲区大小（MB，默认 64） |
| `--threads=<N>` | 并行处理文件的数量（默认 1） |
| `--verbose, -v` | 打印速度、耗时等详细信息 |
| `--digest-bits=<N>` | SHAKE 算法的输出比特数 |

### 支持的全部算法

`md4`, `md5`, `md5-sha1`, `sha1`, `sha224`, `sha256`, `sha384`, `sha512`, `sha512-224`, `sha512-256`, `sha3-224`, `sha3-256`, `sha3-384`, `sha3-512`, `shake128`, `shake256`, `blake2b512`, `blake2s256`, `ripemd160`, `sm3`, `whirlpool`, `mdc2`, `keccak224`, `keccak256`, `keccak384`, `keccak512`

> 安全提示：MD4、MD5、SHA-1 等算法已被证实存在安全风险，使用时会自动显示警告。

## 性能表现

在测试环境（NVMe SSD + Intel i7）下，计算 10GB 文件的 SHA-256 速度可达 ~600 MB/s，多文件并行时可充分利用多核 CPU。

## 安全说明

- 本工具仅用于哈希计算，不涉及加密或身份验证。
- 使用弱哈希算法（如 MD5）时，用户需自行承担安全风险（工具会显示警告）。

## 参与贡献

欢迎提交 Issue 和 Pull Request。提交代码时请确保兼容 C++17，并遵循 OpenSSL 的使用规范。

## 许可证

本项目采用 MIT 许可证，使用 OpenSSL 3.x（Apache 2.0 许可证），详见 [LICENSE](LICENSE) 文件。