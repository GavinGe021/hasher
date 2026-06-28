# hasher – High-performance Multi-algorithm Hash Calculator

[![License](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![OpenSSL](https://img.shields.io/badge/OpenSSL-3.x-green.svg)](https://www.openssl.org/)
[![Build Status](https://github.com/GavinGe021/hasher/actions/workflows/build.yml/badge.svg)](https://github.com/GavinGe021/hasher/actions)

[中文文档](README.zh-CN.md) | [简体中文](README.zh-CN.md)

`hasher` is a lightweight, high-performance command-line tool for computing hash digests of files or strings. Built on OpenSSL 3.x EVP, it supports 20+ algorithms with asynchronous I/O, batch parallel processing, and an intelligent progress bar.

**Version 2.0.0** introduces HMAC, checksum verification/generation, JSON/CSV output, colorized output, and configuration file support.

## ✨ Features

- **Multi-algorithm support**: MD4, MD5, SHA-1, SHA-2 family, SHA-3, BLAKE2, SM3, RIPEMD-160, Whirlpool, Keccak, and more.
- **High performance**: Double-buffering + asynchronous I/O fully utilizes CPU and disk bandwidth.
- **Parallel processing**: Process multiple files concurrently with `--threads`.
- **Smart progress bar**: Real-time speed and ETA for large files.
- **HMAC support**: `--hmac <key>` for keyed-hash message authentication.
- **Check mode**: `--check <hashfile>` to verify checksums (like `sha256sum -c`).
- **Generate mode**: `--generate <file>` to output standard checksum files.
- **JSON/CSV output**: `--format json` or `--format csv` for script integration.
- **Colorized output**: `--color always/never/auto` to control terminal colors.
- **Config file**: `~/.hasher/config` for persistent settings.
- **Cross-platform**: Native I/O on Windows and Linux with no performance compromise.

## 📦 Installation

### Build from Source

    git clone https://github.com/GavinGe021/hasher.git
    cd hasher
    make
    # Or manually:
    # g++ -std=c++17 -O3 -pthread -o hasher hasher.cpp -lcrypto -lssl
    # Dependencies: OpenSSL 3.x dev libraries (libssl-dev / openssl-devel)

### Pre-built Binaries (Recommended)

Download the static binaries for your platform from the [Releases](https://github.com/GavinGe021/hasher/releases) page.

### Package Managers (Community Maintained)

- Windows (winget): `winget install hasher` (submission pending)
- Windows (Chocolatey): `choco install hasher`
- Linux (APT): Coming soon via PPA
- macOS (Homebrew): `brew install hasher`

## 🚀 Usage

    hasher [OPTIONS] <input> [algorithm] [<input2> ...]

### Examples

    # Basic hash (string)
    hasher hello

    # File hash
    hasher --file myfile.iso

    # Auto-detect (file if exists, else string)
    hasher myfile

    # Specify algorithm
    hasher hello sha512

    # Parallel processing (4 threads)
    hasher --threads=4 file1.iso file2.iso file3.iso

    # SHAKE with custom output length (512 bits)
    hasher --digest-bits=512 --file data.bin shake256

    # HMAC
    hasher --hmac mykey hello sha256

    # Verify checksums
    hasher --check checksum.sha256

    # Generate checksum file
    hasher --generate output.sha256 file1.bin file2.bin

    # JSON output
    hasher --format json hello

    # Verbose mode with speed
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
| `--verbose, -v` | Print speed, time, and split hashes for md5-sha1 |
| `--digest-bits=<N>` | Output bits for SHAKE algorithms |
| `--format <fmt>` | Output format: `text`, `json`, `csv` |
| `--color <mode>` | Color mode: `auto`, `always`, `never` |
| `--hmac <key>` | Compute HMAC with the given key |
| `--check <file>` | Verify checksums from file |
| `--generate <file>` | Write standard checksum file |

### Supported Algorithms

`md4`, `md5`, `md5-sha1`, `sha1`, `sha224`, `sha256`, `sha384`, `sha512`, `sha512-224`, `sha512-256`, `sha3-224`, `sha3-256`, `sha3-384`, `sha3-512`, `shake128`, `shake256`, `blake2b512`, `blake2s256`, `ripemd160`, `sm3`, `whirlpool`, `mdc2`, `keccak224`, `keccak256`, `keccak384`, `keccak512`

> **Note**: Algorithms like MD4, MD5, and SHA-1 are cryptographically broken. The tool will display a warning when they are used.

## ⚙️ Configuration

Create `~/.hasher/config` with `key = value` pairs. Supported keys:

- `default_algorithm` – default hash algorithm (e.g., `sha256`)
- `buffer_size_mb` – I/O buffer size in MB
- `color` – `auto`, `always`, `never`
- `format` – `text`, `json`, `csv`
- `progress` – `true` / `false`
- `threads` – number of parallel processes
- `verbose` – `true` / `false`

Environment variable `HASHER_CONFIG` can override the config file path.

## 📊 Performance

On a test environment (NVMe SSD + Intel i7), hashing a 10GB file with SHA-256 achieves ~600 MB/s. Parallel mode fully utilizes multi-core CPUs.

## 🔒 Security

- This tool is for hash computation only; it does not handle encryption or authentication.
- Users are responsible for the risks of using weak algorithms (the tool displays warnings).

## 🤝 Contributing

Issues and Pull Requests are welcome. Please ensure your code is C++17 compatible and follows OpenSSL usage guidelines.

## 📜 License

This project is licensed under the **MIT License**. OpenSSL 3.x is used under the Apache 2.0 License. See the [LICENSE](LICENSE) file for details.