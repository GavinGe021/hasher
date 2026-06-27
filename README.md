# hasher – High-performance Multi-algorithm Hash Calculator

[![License](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![OpenSSL](https://img.shields.io/badge/OpenSSL-3.x-green.svg)](https://www.openssl.org/)
[![Build Status](https://github.com/GavinGe021/hasher/actions/workflows/build.yml/badge.svg)](https://github.com/GavinGe021/hasher/actions)

[中文文档](README.zh-CN.md) | [简体中文](README.zh-CN.md)

`hasher` is a lightweight, high-performance command-line tool for computing hash digests of files or strings. Built on OpenSSL 3.x EVP, it supports 20+ algorithms with asynchronous I/O, batch parallel processing, and an intelligent progress bar.

## Features

- Multi-algorithm support: MD4, MD5, SHA-1, SHA-2 family, SHA-3, BLAKE2, SM3, RIPEMD-160, Whirlpool, Keccak, and more.
- High performance: Double-buffering + asynchronous I/O fully utilizes CPU and disk bandwidth.
- Parallel processing: Process multiple files concurrently with `--threads`.
- Smart progress bar: Auto-shows for single files >100MB (controlled by `--progress`/`--no-progress`).
- Customizable SHAKE output: Set output length in bits via `--digest-bits`.
- Cross-platform: Native I/O on Windows and Linux with no performance compromise.
- Verbose output: `--verbose` shows speed, elapsed time, and split hashes for `md5-sha1`.

## Installation

### Build from Source

Clone the repository and compile:

    git clone https://github.com/GavinGe021/hasher.git
    cd hasher
    make

Or manually:

    g++ -std=c++17 -O3 -pthread -o hasher hasher.cpp -lcrypto -lssl

**Dependencies**: OpenSSL 3.x dev libraries (`libssl-dev` / `openssl-devel`).

### Pre-built Binaries (Recommended)

Download the static binaries for your platform from the [Releases](https://github.com/GavinGe021/hasher/releases) page.

### Package Managers (Community Maintained)

- Windows (winget): `winget install hasher` (submission pending)
- Windows (Chocolatey): `choco install hasher`
- Linux (APT): Coming soon via PPA
- macOS (Homebrew): `brew install hasher`

## Usage

    hasher [OPTIONS] <input> [algorithm] [<input2> ...]

### Basic Examples

    # Hash string "hello" with SHA-256 (default)
    hasher hello

    # Hash a file
    hasher --file myfile.iso

    # Auto-detect (file if exists, else string)
    hasher myfile

    # Specify algorithm (SHA-512)
    hasher hello sha512

    # Parallel processing of multiple files (4 threads)
    hasher --threads=4 file1.iso file2.iso file3.iso

    # Custom SHAKE256 output length (512 bits)
    hasher --digest-bits=512 --file data.bin shake256

    # Show detailed speed info
    hasher --verbose largefile.bin

### Options

| Option | Description |
| :--- | :--- |
| `--help, -h` | Show help message |
| `--version, -V` | Show version and OpenSSL info |
| `--file` | Force treat inputs as file paths |
| `--string` | Force treat inputs as literal strings |
| `--progress, -p` | Force show progress bar (single file only) |
| `--no-progress` | Disable progress bar |
| `--buffer-size=<MB>` | I/O buffer size in MB (default: 64) |
| `--threads=<N>` | Number of parallel file processes (default: 1) |
| `--verbose, -v` | Print speed, time, and split hashes |
| `--digest-bits=<N>` | Output bits for SHAKE algorithms |

### Supported Algorithms

`md4`, `md5`, `md5-sha1`, `sha1`, `sha224`, `sha256`, `sha384`, `sha512`, `sha512-224`, `sha512-256`, `sha3-224`, `sha3-256`, `sha3-384`, `sha3-512`, `shake128`, `shake256`, `blake2b512`, `blake2s256`, `ripemd160`, `sm3`, `whirlpool`, `mdc2`, `keccak224`, `keccak256`, `keccak384`, `keccak512`

> Note: Algorithms like MD4, MD5, and SHA-1 are cryptographically broken. The tool will display a warning when they are used.

## Performance

On a test environment (NVMe SSD + Intel i7), hashing a 10GB file with SHA-256 achieves ~600 MB/s. Parallel mode fully utilizes multi-core CPUs.

## Security

- This tool is for hash computation only; it does not handle encryption or authentication.
- Users are responsible for the risks of using weak algorithms (the tool displays warnings).

## Contributing

Issues and Pull Requests are welcome. Please ensure your code is C++17 compatible and follows OpenSSL usage guidelines.

## License

This project is licensed under the MIT License. OpenSSL 3.x is used under the Apache 2.0 License. See the [LICENSE](LICENSE) file for details.