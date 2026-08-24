# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

The SmartMet frontend plugin (`smartmet-plugin-frontend`) is a load-balancing reverse proxy for SmartMet Server clusters. It receives incoming HTTP requests and distributes them across backend servers discovered via UDP broadcasting through the Sputnik engine. It also provides response caching (one cache holding every content encoding) with memory and filesystem tiers, admin endpoints for cluster management, and pause/continue support for F5 health checks.

## Build commands

```bash
make                  # Build frontend.so (runs testsuite/check automatically)
make test             # Integration tests: starts backend + frontend smartmetd instances, sends HTTP requests
make cluster-test     # Cluster regression tests on their own (also run by "make test")
make load-test        # Load the frontend under perf (not a test, never in "make test")
make -C testsuite check  # Unit tests (Boost.Test): QEngineInfoTest, GridGenerationsInfoTest, ParameterLookupTest, ChunkedBodyDecoderTest
make format           # clang-format (Google-based, Allman braces, 100-col)
make clean            # Clean all build artifacts
make rpm              # Build RPM package
```

### Running a single unit test

```bash
cd testsuite && make QEngineInfoTest && ./QEngineInfoTest
```

### Integration tests

Two standalone C++ programs (not Boost.Test), sharing `test/TestHarness.{h,cpp}` —
starting a `smartmetd`, finding the port it picked, talking HTTP to it over a raw
socket, and shutting it down while noticing whether it died badly. Both require
`/usr/sbin/smartmetd` to be installed and use `--port=0`.

**`test/RunTests.cpp`** — run by `make test`, about 13 seconds. Starts two backends
and a frontend, replays the requests in `test/input/` (`.get`, `.post`, `.options`),
compares response bodies against `test/output/`, writes failures to `test/failures/`.
It then checks that backend connections were actually reused, which no response
comparison can see.

**`test/RunClusterTests.cpp`** — run by `make test` alongside `RunTests`, and on its
own by `make cluster-test`. About 30 seconds, nearly all of it spent waiting for
sputnik to notice a backend leaving and coming back. It covers what a cluster does
when backends come and go, all of which has been broken at some point:

| Test | Guards |
| --- | --- |
| backend connections are reused | the pool works at all |
| a paused backend is drained and restored | `/admin?what=pause` and `continue` — how an operator takes a server out — cost the clients nothing |
| a stalled backend is timed out | the backend timeout fires, instead of pinning a request handler thread forever |
| a dead backend is answered, not dropped | a request that cannot be served gets a framed HTTP error rather than a dropped connection |
| a small response is not held for an ACK | the server sets `TCP_NODELAY`; without it a small proxied response costs 40 ms waiting for a delayed ACK, visible only over a kept-alive connection |

It uses `cnf/reactor_frontend_cluster.conf`, whose only difference is a `backend.timeout`
of 8 seconds — long enough for queries that take milliseconds, short enough to wait for.
`RunClusterTests.cpp` hardcodes that number; keep the two in step.

Every request these tests make is bounded by a receive timeout. That is not a detail:
two of the four are about a frontend that has stopped answering, and without the bound
a regression makes the test *hang* instead of failing, which is much less useful.

`SMARTMETD` overrides which server the test programs run, so a locally built
smartmet-server can be tested without installing it — the difference between
measuring the change you just made and the one already deployed:

```bash
SMARTMETD=../../1950-smartmet-server/smartmetd ./RunClusterTests
```

The margins are wide on purpose. The client waits 40 s while the stall test asserts a
give-up within 18, so a correct frontend (8 s) and a broken one (40 s) land far either
side of the line rather than a second apart — these run on CI runners, where a second
of scheduling luck should not decide a verdict. For the same reason "this request
waited on the stalled backend" is decided from the backend timeout rather than from a
small number of seconds a merely busy machine could produce.

## Architecture

### Core classes

- **`Plugin`** (`frontend/Plugin.{h,cpp}`) — SmartMetPlugin implementation. Registers `/` (health check) and `/admin` handlers. Delegates unmatched requests to `HTTP`. Manages admin sub-requests: `clusterinfo`, `backends`, `activebackends`, `backendconnections`, `qengine`, `gridgenerations`, `pause`, `continue`. Uses Sputnik engine for backend discovery.

- **`HTTP`** (`frontend/HTTP.{h,cpp}`) — Request forwarding layer. Registers as the Reactor's "no match" handler (the catch-all for requests not handled by other plugins). Selects a backend via `Sputnik::Services::getService()`, strips host prefixes from URIs, and forwards via `Proxy::HTTPForward()`. Retries on `PROXY_FAIL_REMOTE_DENIED`; retires backends on connection failures.

  > **High-load retirement is intentional, don't remove it.** A `1234` high-load reply maps to `PROXY_FAIL_REMOTE_DENIED`, and `transport` then calls `Services::removeBackend()` — globally retiring the busy backend. This looks like a bug but is deliberate backpressure: the Sputnik discovery heartbeat re-adds the backend within ~2-3s once it stops reporting high load, so the cluster briefly stops bombarding a struggling backend with requests that would just bounce. It is also what stops the deterministic `sticky` forwarder from looping on one backend, so the resend loop is *not* eternal. Do not "fix" this by dropping the retirement or adding a per-request excluded-backend set. The real defect lives in the engine: `Services::removeBackend` `SIGKILL`s when retirement empties the last service, which under cluster-wide high load can kill the frontend (see `smartmet-engine-sputnik` CLAUDE.md, Backend health tracking).

- **`Proxy`** (`frontend/Proxy.{h,cpp}`) — Owns the backend I/O threads (`boost::asio::io_context` with configurable thread count), the `ResponseCache` and the `BackendConnectionPool`. `HTTPForward()` is the main entry point for proxying a request to a specific backend host:port.

- **`BackendConnectionPool`** (`frontend/BackendConnectionPool.{h,cpp}`) — Idle backend connections kept for the next request, keyed by `ip:port`. See "Backend connection reuse" below.

- **`LowLatencyGatewayStreamer`** (`frontend/LowLatencyGatewayStreamer.{h,cpp}`) — Streaming content handler that reads backend responses via async Boost.Asio sockets and feeds them to the Spine HTTP server. Handles caching of streamable responses and backend timeouts. See "Response framing" below: it parses the backend's head and streams the **body only**.

- **`ChunkedBodyDecoder`** (`frontend/ChunkedBodyDecoder.{h,cpp}`) — Incremental chunked-body decoder. Separate from the streamer so it can be unit tested (`testsuite/ChunkedBodyDecoderTest.cpp`), since a socket can split a chunk header, its data or its terminator anywhere.

- **`ResponseCache`** (`frontend/ResponseCache.{h,cpp}`) — Two-tier cache (memory LRU + filesystem) keyed by (ETag, content encoding), so that the identity, gzip and zstd variants of one resource live side by side. Stores response metadata (mime type, cache-control, etc.) separately from buffer content.

  > **The ETag key is the coding independent one.** A backend appends the content coding to the entity-tag of an encoded response (`"abc-timeseries+zstd"`), but answers the frontend's `X-Request-ETag` probe with the tag of the data itself, since the probe has no body to encode. `Spine::HTTP::baseETag()` therefore strips the coding before the tag is used as a cache key, and `Spine::HTTP::contentCodedETag()` puts it back when a variant is served. Keying on the tag as received would make every lookup miss. Likewise, the codings to look up are negotiated with `Spine::HTTP::rankContentEncodings()` and the server's own `supportedContentEncodings()` — every coding the client accepts, best first, since the backend may no longer offer the best one: it is the backend that encodes the responses, so a negotiation of our own that disagrees with the server's cannot find what the backend produced.

### Response framing, and why the gateway is no longer a byte stream

The streamer used to forward the backend's status line, headers and body to the
client verbatim, and detect the end of the body by EOF on the backend socket.
That leaked the backend hop's connection semantics to the client: `Proxy` asks
its backends for `Connection: close`, so every proxied response carried
`Connection: close` to the client and **no client ever got a persistent
connection through the frontend**.

It now parses the backend head (which it already did, for caching) and re-emits
it as this frontend's own response:

- `LowLatencyGatewayStreamer::waitForResponseHead()` blocks until the head is
  parsed; `Proxy::HTTPForward()` copies its status and headers onto the Spine
  `Response` and puts only the **body** in the streamer.
- Hop-by-hop fields — and whatever `Connection` names — are stripped, along with
  `Content-Length` and `Transfer-Encoding`: the frontend frames the client
  response itself and may frame it differently.
- `BodyFraming` records how the *backend* delimited its body. `LENGTH` becomes a
  `Content-Length` response to the client; `CHUNKED` and `UNTIL_CLOSE` both
  become chunked, which is self-delimiting — so a client connection survives a
  backend connection that could not be reused. That is what "manage keep-alive
  independently on the client and backend sides" means in practice.
- A chunked backend body is decoded before it reaches the client, so the server
  re-frames plain bytes and no handler ever sees chunk framing.
- A frontend cache hit is returned as a complete, ordinary buffered response
  rather than a byte stream, so it is framed and compressible like any other.

Two consequences worth knowing:

1. `Date`, `Server` and `Vary` on proxied responses now come from the frontend's
   server layer rather than the backend, as they already did for cache hits.
   Everything else the backend sent (`ETag`, `Cache-Control`, `Expires`,
   `Content-Type`, `Content-Encoding`, `X-*`) is passed through.
2. `Response::isGatewayResponse` is no longer set. The backend heartbeat hooks
   that sputnik registers used to be keyed on that flag; the server now keys
   them on the response recording an originating backend instead
   (`AsyncConnection::notifyBackendFinished`). A frontend built against a server
   older than that change will stop feeding sputnik's heartbeat and healthy
   backends will be retired — the two must be deployed together.

### Backend connection reuse

Because the client response is framed independently of the backend one, a backend
connection whose response ended at a known boundary no longer has to be thrown
away. `BackendConnectionPool` (`frontend/BackendConnectionPool.{h,cpp}`) keeps such
connections, keyed by `ip:port`, on the `Proxy`.

Three things had to be true before this was safe, and all three are checks in the
code rather than assumptions:

- **The message really ended.** `finishBackendResponse()` only pools a socket when
  the framing said so: a `Content-Length` that was reached with nothing past it
  (`itsBodyOverrun`), or a chunked body whose terminating chunk arrived with no
  bytes left in the decoder (`ChunkedBodyDecoder::pending()`). `UNTIL_CLOSE` bodies
  can never qualify — the close *is* the framing.
- **The backend agreed.** `backendAllowsReuse()` reads the response's `Connection`
  field before it is stripped, with the two versions' opposite defaults: HTTP/1.1
  persists unless it says `close`, HTTP/1.0 does not unless it says `keep-alive`.
- **The connection is still there.** A backend closes idle connections on its own
  keep-alive timeout, so staleness is normal, not exceptional. `acquire()` checks
  with a `MSG_PEEK | MSG_DONTWAIT` `recv` — zero bytes means the backend closed,
  *readable* bytes mean the connection is out of sync and is just as unusable —
  and `retryOnFreshConnection()` replays the request once on a new connection when
  a reused one dies before answering. Neither is sufficient alone: the check
  cannot cover the microseconds between the peek and the write.

The frontend also stops asking backends for `Connection: close`, and strips
hop-by-hop fields (plus `Expect`, whose 100-continue negotiation is finished at
the client hop and whose interim response would be read as the response head)
from the forwarded request.

**The cache miss is where most of the win is.** A miss used to cost two
connections: one for the ETag probe and another for the content. Plugins answer
the probe with a bodyless `204 No Content`, so `probeLeftCleanConnection()` can
confirm the socket is at a message boundary and `sendContentRequest()` writes the
content request straight onto it.

`/admin?what=backendconnections` reports what the pool is doing — reuse is
invisible in the responses themselves, so this is the only way to distinguish a
working pool from one that finds every connection dead. `test/RunTests.cpp` asserts
reuse actually happens, since a subtly broken pool would otherwise still pass every
response comparison.

Configured under `backend.keepalive` (`enabled`, `idle_timeout`,
`max_idle_connections`). **`idle_timeout` must stay below the backend server's own
`keepalive.timeout`** (30 s by default in smartmet-server) or every pooled
connection is dead by the time it is picked up.

Two limits worth knowing:

- **The client's protocol version decides.** The frontend forwards the client's
  HTTP version to the backend, so an HTTP/1.0 client request gets `Connection:
  close` and no reuse. Upgrading it here would change what the backend may answer
  with, and an HTTP/1.0 request need not carry the `Host` that HTTP/1.1 requires.
- **Chunked backend bodies are not exercised locally.** `test/cnf` loads no plugin
  that streams, so the `CHUNKED` reuse path is covered by
  `ChunkedBodyDecoderTest` and by production traffic, not by `make test`.

### The backend timeout

`itsBackendTimeoutInSeconds` is enforced as a deadline plus a re-armed timer, not by
moving the timer on every read:

- `extendBackendDeadline()` moves `itsDeadline` and nothing else. It runs on every
  read completion, so it has to stay cheap.
- `armTimeoutTimer()` waits for the current deadline. `handleTimeout()` re-arms
  itself if the deadline has moved since, and otherwise **fails the stream**: sets
  `FAILED`, closes the socket, and wakes both `waitForResponseHead()` and
  `getChunk()`.

Failing the stream is the part that matters. Both waiters loop while the status says
`ONGOING`, and `HTTPForward()` blocks in `waitForResponseHead()` on a request-handler
thread, so a backend that accepts a connection and then says nothing would otherwise
hold that thread for the life of the process. `boost::asio::basic_waitable_timer`
also makes the re-arming mandatory: `expires_after()` cancels the outstanding wait,
so a timer that is pushed back without a new `async_wait()` never fires again.

### Load driver (`test/RunLoadTest.cpp`)

Not a test — nothing passes or fails. `make load-test` stands up the same cluster,
keeps the frontend as busy as the client machine can manage, and records it with
`perf record -p` for the duration. Options are passed through `LOAD_ARGS`, and a
target containing `&` has to be quoted:

```bash
make load-test LOAD_ARGS="--seconds=60 --threads=16"
cd test && ./RunLoadTest '--target=/timeseries?places=Helsinki&param=name,time'
```

It reports throughput and latency percentiles, and — more importantly — **CPU
seconds for the frontend, the backends and the load driver separately**. A profile
of the frontend only means something while the frontend is what is busy, and with
an 80 kB `obsparameters` response from a test backend on the same machine it is
not: the backends burn three times the frontend's CPU, and the driver says so
rather than letting you read a profile of a process that was mostly waiting.

Requests go over kept-alive connections (`TestHarness::HttpConnection`), which is
what real clients do and what makes the frontend rather than the TCP handshake the
thing being measured. It honours `Connection: close`, which the server sends every
`keepalive.maxrequests` responses — a client that does not reports the server's
correct behaviour as an error.

This is what found the missing `TCP_NODELAY` in smartmet-server: 181 requests/s at
a suspiciously tight 43 ms p50 while nothing anywhere was using CPU.

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
- `response_cache` — Memory and filesystem cache sizes and path. The deprecated `compressed_cache` / `uncompressed_cache` blocks are still honoured: their sizes are summed and the first directory is used
- `backend.timeout` — Backend connection timeout in seconds (default: 600)
- `backend.threads` — Backend IO thread pool size (default: 20)
- `backend.keepalive.enabled` — Reuse backend connections (default: true)
- `backend.keepalive.idle_timeout` — Seconds a pooled connection may sit idle (default: 20; must be below the backend's own `keepalive.timeout`)
- `backend.keepalive.max_idle_connections` — Idle connections kept per backend (default: 32)

### Plugin loading

The shared library exports `create()` / `destroy()` C functions. The server loads `frontend.so` at runtime. Engine references (`SmartMet::Engine::Sputnik`) are resolved at load time by the server.
