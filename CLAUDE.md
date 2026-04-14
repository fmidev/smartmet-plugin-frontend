# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

The SmartMet frontend plugin (`smartmet-plugin-frontend`) is a load-balancing reverse proxy for SmartMet Server clusters. It receives incoming HTTP requests and distributes them across backend servers discovered via UDP broadcasting through the Sputnik engine. It also provides response caching (both compressed/gzip and uncompressed) with memory and filesystem tiers, admin endpoints for cluster management, and pause/continue support for F5 health checks.

## Build commands

```bash
make                  # Build frontend.so (runs testsuite/check automatically)
make test             # Integration tests: starts backend + frontend smartmetd instances, sends HTTP requests
make -C testsuite check  # Unit tests (Boost.Test): QEngineInfoTest, GridGenerationsInfoTest, ParameterLookupTest
make format           # clang-format (Google-based, Allman braces, 100-col)
make clean            # Clean all build artifacts
make rpm              # Build RPM package
```

### Running a single unit test

```bash
cd testsuite && make QEngineInfoTest && ./QEngineInfoTest
```

### Integration tests

The integration test runner (`test/RunTests.cpp`) is a standalone C++ program (not Boost.Test). It:
1. Starts two `smartmetd` backend processes using configs in `test/cnf/`
2. Starts a `smartmetd` frontend process
3. Sends raw HTTP requests from `test/input/` files (`.get`, `.post`, `.options`)
4. Compares response bodies against expected output in `test/output/`
5. Writes failures to `test/failures/`

Requires `/usr/sbin/smartmetd` to be installed. Uses `--port=0` for dynamic port allocation.

## Architecture

### Core classes

- **`Plugin`** (`frontend/Plugin.{h,cpp}`) — SmartMetPlugin implementation. Registers `/` (health check) and `/admin` handlers. Delegates unmatched requests to `HTTP`. Manages admin sub-requests: `clusterinfo`, `backends`, `activebackends`, `qengine`, `gridgenerations`, `pause`, `continue`. Uses Sputnik engine for backend discovery.

- **`HTTP`** (`frontend/HTTP.{h,cpp}`) — Request forwarding layer. Registers as the Reactor's "no match" handler (the catch-all for requests not handled by other plugins). Selects a backend via `Sputnik::Services::getService()`, strips host prefixes from URIs, and forwards via `Proxy::HTTPForward()`. Retries on `PROXY_FAIL_REMOTE_DENIED`; retires backends on connection failures.

- **`Proxy`** (`frontend/Proxy.{h,cpp}`) — Manages the backend connection pool (`boost::asio::io_context` with configurable thread count) and two `ResponseCache` instances (compressed + uncompressed). `HTTPForward()` is the main entry point for proxying a request to a specific backend host:port.

- **`LowLatencyGatewayStreamer`** (`frontend/LowLatencyGatewayStreamer.{h,cpp}`) — Streaming content handler that reads backend responses chunk-by-chunk via async Boost.Asio sockets and feeds them to the Spine HTTP server. Handles caching of streamable responses and backend timeouts.

- **`ResponseCache`** (`frontend/ResponseCache.{h,cpp}`) — Two-tier cache (memory LRU + filesystem) keyed by ETag. Stores response metadata (mime type, cache-control, etc.) separately from buffer content.

### `info/` subsystem

Classes in `frontend/info/` handle aggregating metadata from multiple backends:
- `BackendInfoRequests` — Sends parallel TCP queries to backends, collects responses
- `BackendInfoResponse` / `BackendInfoRec` — Base response/record types with table and JSON output
- `QEngineInfoRec` — Querydata engine info (producers, parameters, time ranges)
- `GridGenerationsInfoRec` — Grid generation info

These power the `/admin?what=qengine` and `/admin?what=gridgenerations` endpoints, which query all backends and return merged summaries.

### Key dependencies

- **`smartmet-engine-sputnik`** — Backend discovery via UDP broadcast; provides `Services` (backend routing) and `Engine` (cluster status)
- **`smartmet-library-spine`** — HTTP server framework, Reactor, plugin loading, configuration
- **`smartmet-library-macgyver`** — Utilities (caching, exceptions, string conversion, Base64)
- **`libconfig`** — Configuration file format (`.conf` files)

### Configuration

The plugin reads a libconfig `.conf` file (see `cnf/frontend.conf.sample`). Key settings:
- `user` / `password` — Basic auth credentials for admin endpoints
- `compressed_cache` / `uncompressed_cache` — Memory and filesystem cache sizes and paths
- `backend.timeout` — Backend connection timeout in seconds (default: 600)
- `backend.threads` — Backend IO thread pool size (default: 20)

### Plugin loading

The shared library exports `create()` / `destroy()` C functions. The server loads `frontend.so` at runtime. Engine references (`SmartMet::Engine::Sputnik`) are resolved at load time by the server.
