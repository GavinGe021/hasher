# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [2.0.0] – 2026-06-28

### Added
- HMAC support via `--hmac <key>` for keyed-hash message authentication (using EVP_MAC in OpenSSL 3.x)
- Check mode `--check <file>` to verify checksums (like `sha256sum -c`)
- Generate mode `--generate <file>` to output standard checksum files
- JSON output format (`--format json`)
- CSV output format (`--format csv`)
- Colorized output with `--color auto/always/never`
- Configuration file support (`~/.hasher/config`) with keys: `default_algorithm`, `buffer_size_mb`, `color`, `format`, `progress`, `threads`, `verbose`
- Environment variable `HASHER_DEFAULT_ALGO` to override default algorithm
- Environment variable `HASHER_CONFIG` to specify custom config file path
- Smart progress bar with real-time speed (MB/s) and ETA
- Color-coded `OK`/`FAILED` output in check mode

### Changed
- Improved parsing of checksum files in `--check` mode to handle filenames with spaces
- Algorithm name lookup now tries original case, lower case, and alias list
- Updated help text to include all new options
- Version bumped to 2.0.0

### Fixed
- Fixed `FileHandle` buffer issue for cross-platform compatibility (now uses native I/O)
- Fixed HMAC handling for long keys (exceeding block size)

## [1.0.0] – 2026-06-27

### Added
- Initial release
- Support for 20+ hash algorithms (MD4, MD5, SHA-1, SHA-2, SHA-3, BLAKE2, SM3, RIPEMD-160, Whirlpool, Keccak, etc.)
- File and string hashing
- Parallel processing with `--threads`
- Progress bar for large files
- Verbose output with speed and time
- md5-sha1 split display
- Cross-platform (Windows/Linux)