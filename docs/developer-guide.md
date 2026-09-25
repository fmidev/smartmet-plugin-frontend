# Frontend plugin developer guide

This guide is for developers who change `smartmet-plugin-frontend`, the load-balancing
reverse proxy of a SmartMet Server cluster. It describes how a request is routed to a
backend, how the response is re-framed, how the response cache works, how backend
connections are pooled, and the pitfalls.

Related documents:

* [CLAUDE.md](../CLAUDE.md): detailed notes, with the reasons behind them, on response
  framing, backend connection reuse, the backend timeout, high-load retirement, and the
  test programs.
* [Conditional-Backend-Request-Design.md](Conditional-Backend-Request-Design.md): a
  proposal (partly prepared, not implemented) for replacing the ETag probe with one
  conditional request.
* The server [developer guide](https://github.com/fmidev/smartmet-server/blob/master/docs/developer-guide.md)
  (connections, reply paths, compression) and the spine
  [developer guide](https://github.com/fmidev/smartmet-library-spine/blob/master/docs/developer-guide.md)
  (routing, public and private handlers, admin requests).
* The backend plugin's [developer guide](https://github.com/fmidev/smartmet-plugin-backend/blob/master/docs/developer-guide.md)
  and the sputnik engine, which does the service discovery.

## Contents

1. [The cluster](#1-the-cluster)
2. [Source files](#2-source-files)
3. [Building and testing](#3-building-and-testing)
4. [Routing a request](#4-routing-a-request)
5. [The ETag probe and the response cache](#5-the-etag-probe-and-the-response-cache)
6. [Response framing](#6-response-framing)
7. [Backend connections and timeouts](#7-backend-connections-and-timeouts)
8. [Admin requests and pausing](#8-admin-requests-and-pausing)
9. [Configuration](#9-configuration)
10. [Known pitfalls](#10-known-pitfalls)

---

## 1. The cluster

```
clients ──> (F5) ──> frontend smartmetd ──> backend smartmetd × N
                     frontend.so               backend.so + the data plugins
                     sputnik (frontend mode) <─ UDP ─ sputnik (backend mode)
```

* Each **backend** runs the sputnik engine in backend mode, which broadcasts the URIs its
  plugins serve (spine's `getURIMap()`, so only **public** handlers), its load and its
  status.
* Each **frontend** runs sputnik in frontend mode, which collects those broadcasts into
  `Services`: for every URI, the backends that serve it. The frontend plugin registers
  itself as the Reactor's **"no match" handler**, so every request that no local plugin
  handles is proxied.
* A backend stops broadcasting when it is paused, shutting down or overloaded, and the
  frontends then stop sending to it.

## 2. Source files

| File | Role |
|------|------|
| `frontend/Plugin.{h,cpp}` | The plugin: `/` health check, `/admin` with the cluster admin requests, pause and continue, starting sputnik. |
| `frontend/HTTP.{h,cpp}` | The "no match" handler: picks a backend through sputnik, fixes up the URI, forwards with `Proxy::HTTPForward()`, retries and retires backends. Builds the response cache from the configuration. |
| `frontend/Proxy.{h,cpp}` | Owns the backend I/O threads (`front-be-N`), the `ResponseCache` and the `BackendConnectionPool`; `HTTPForward()` runs one exchange. |
| `frontend/LowLatencyGatewayStreamer.{h,cpp}` | One backend exchange: ETag probe, cache lookup, content request, parsing the backend's head, streaming the body, timeouts, pooling the connection afterwards. |
| `frontend/ChunkedBodyDecoder.{h,cpp}` | Incremental decoder for chunked backend bodies. |
| `frontend/BackendConnectionPool.{h,cpp}` | Idle backend connections keyed by `ip:port`. |
| `frontend/ResponseCache.{h,cpp}` | Memory + filesystem cache of response bodies keyed by ETag and content encoding. |
| `frontend/info/` | The `qengine`, `gridgenerations` and similar admin requests that gather information from all backends (`TcpMultiQuery`). |

## 3. Building and testing

```bash
make                      # frontend.so (also runs the unit tests in testsuite/)
make -C testsuite check   # Boost.Test unit tests (info parsing, ChunkedBodyDecoder, …)
make test                 # RunTests + RunClusterTests: real smartmetd processes
make cluster-test         # RunClusterTests only
make load-test            # a load driver under perf; not a test
```

`test/RunTests.cpp` starts two backends and a frontend, replays `test/input/` against
`test/output/`, and checks that backend connections were reused.
`test/RunClusterTests.cpp` covers pausing, stalled and dead backends, the `TCP_NODELAY`
regression and chunked responses through the proxy. Both need `/usr/sbin/smartmetd`, or a
locally built server via `SMARTMETD=…`. Keep `backend.timeout` in
`reactor_frontend_cluster.conf` (8 s) in step with the value hard-coded in
`RunClusterTests.cpp`. The CLAUDE.md "Integration tests" section explains the timing
margins.

## 4. Routing a request

`HTTP::request()`:

1. If the proxy is shutting down → 503.
2. `Services::getService(request)` picks a backend for the URI with the configured
   forwarding strategy (sputnik's `forwarding`: `random` by default, or sticky,
   least-connections, inverse-load and other variants).
3. **No backend** serves the URI → 404. A conditional request (`If-None-Match` or
   `If-Modified-Since`) is answered **304** instead, so clients keep their cached copy
   while no backend is available.
4. URIs may carry a host prefix (`/<backend-host>/<uri>`) to address one backend; the
   prefix is stripped before forwarding.
5. `Proxy::HTTPForward()` runs the exchange (§5–§7). The streamer's destructor decrements
   the backend's active-request count, which the load-based strategies use.
6. **Failures**: a backend that refuses (the server's high-load reply,
   `PROXY_FAIL_REMOTE_DENIED`) is **retired** from `Services` and the request is sent to
   another backend. A backend that cannot be reached is checked with
   `queryBackendAlive()` and retired if dead. Retired backends come back with their next
   sputnik broadcast, a few seconds later. The retirement on high load is intentional
   backpressure; see CLAUDE.md before changing it.

## 5. The ETag probe and the response cache

Every proxied request starts with an **ETag probe**: the frontend sends the request with
`X-Request-ETag: true`.

* A plugin that supports frontend caching (for example wms, timeseries, edr, grid-gui)
  answers the probe with its ETag and no body (`204`).
* A plugin that does not know the header answers with the full response. The frontend
  notices the missing ETag and **passes the response through**, uncached. There is no
  extra round trip, but also no caching: this is why responses without an ETag (for
  example timeseries and EDR responses from the grid engine) are never cached by the
  frontend.

With an ETag, the frontend looks up `ResponseCache` by **(ETag, content encoding)**: first
in the encoding the client prefers (zstd, then gzip, mirroring the server's
choice), then the uncompressed variant.

* **Hit**: the cached body is returned as an ordinary buffered response with the stored
  `Content-Type`, `Cache-Control`, `Vary`, `Access-Control-Allow-Origin` and
  `Content-Encoding`, and an updated `Expires` from the probe reply. The probe's
  connection goes back to the pool if it ended cleanly.
* **Miss**: `sendContentRequest()` sends the real request (on the same connection if the
  probe left it at a message boundary), streams the body to the client, and stores it
  under the backend's ETag and `Content-Encoding` once complete.

The cache has a memory tier and a filesystem tier (`response_cache.*`). The older
`compressed_cache` / `uncompressed_cache` settings still work but are summed into the one
cache with a deprecation warning.

**The ETag is the whole key.** The request URL is not part of it. A plugin that emits an
ETag must make it unique for everything that affects the body (the request parameters,
the data version, the configuration), or different requests will share a cached body.

## 6. Response framing

The frontend no longer passes the backend's bytes through. It parses the backend's
response head, copies the status and end-to-end headers onto its own response, strips
the hop-by-hop headers and the length and transfer-encoding headers, and streams **only
the body**:

| Backend body | Client response |
|--------------|-----------------|
| `Content-Length` | `Content-Length` |
| chunked (decoded by `ChunkedBodyDecoder`) | chunked |
| until close | chunked |

So a client connection stays persistent even when the backend connection cannot be
reused, and the server layer adds `Date`, `Server` and `Vary`. The server must be new
enough to key sputnik's backend heartbeat on the response's originating backend rather
than on `isGatewayResponse`; frontend and server have to be deployed together (CLAUDE.md,
"Response framing").

## 7. Backend connections and timeouts

* **Pooling.** After a response, the backend connection is kept in
  `BackendConnectionPool` if the body ended at a known boundary, the backend did not say
  `Connection: close`, and the client's protocol allows it. A pooled connection is checked
  for liveness before reuse, and a request that fails on a reused connection is replayed on
  a fresh one. `backend.keepalive.idle_timeout` (20 s) must be **lower** than the
  backends' `keepalive.timeout` (30 s), and the backends' `maxconnections` must allow for
  `backend.keepalive.max_idle_connections` (32) idle connections per frontend.
* **Threads.** Backend I/O runs on `backend.threads` (20) threads named `front-be-N`,
  separate from the server's pools. The proxied request itself occupies a `srv-fast`
  thread on the frontend while it waits for the backend.
* **Timeout.** `backend.timeout` (600 s) bounds how long a backend may go without sending
  data. On expiry the exchange is aborted and the client gets a framed error (or a
  truncated chunked response), instead of a handler thread being blocked forever.
* **Shutdown.** New exchanges are refused once shutdown begins; running ones get
  `backend.shutdown_grace_period` to finish before the I/O threads stop and the plugin is
  unloaded.

## 8. Admin requests and pausing

The frontend serves `/admin` itself and passes each request to the Reactor
(`executeAdminRequest()`) with its own authentication callback, which checks HTTP Basic
credentials against `user` / `password` in the plugin configuration.

| Request | Access | Content |
|---------|--------|---------|
| `clusterinfo`, `backends`, `qengine`, `gridgenerations`, `gridgenerationsqd`, `activebackends` | public | Cluster state from sputnik and from all backends. |
| `backendconnections` | private | Backend connection pool state. |
| `pause`, `continue` (`?time=` / `?duration=`) | requires authentication | Take this frontend out of the load balancer. |

**Pausing** changes only the `/` health check text: `SmartMet Server` normally, `Frontend
Paused` (or paused-until) when paused. The F5 load balancer matches the word `SmartMet`, so
a paused frontend stops receiving traffic while it keeps serving what it gets. Timed pauses
expire on their own.

## 9. Configuration

Plugin configuration (see `cnf/frontend.conf.sample`):

| Key | Default | Meaning |
|-----|---------|---------|
| `user`, `password` | none | Basic authentication for `pause` / `continue`. |
| `response_cache.memory_bytes`, `.filesystem_bytes`, `.directory` | | The response cache. |
| `backend.timeout` | 600 | Seconds without data from the backend. |
| `backend.threads` | 20 | Backend I/O threads. |
| `backend.keepalive.enabled` | true | Pool backend connections. |
| `backend.keepalive.idle_timeout` | 20 | Seconds a pooled connection may stay idle. |
| `backend.keepalive.max_idle_connections` | 32 | Idle connections kept per backend. |
| `backend.shutdown_grace_period` | 10 | Seconds running exchanges get at shutdown. |

The forwarding strategy and the discovery settings belong to the sputnik engine's
configuration.

## 10. Known pitfalls

* **`pause` / `continue` need two sets of credentials:** the server's `admin.user` and
  `admin.password`, and the frontend's own `user` / `password`. Configure both, as
  `test/cnf/reactor_frontend_cluster.conf` and `test/cnf/plugins/frontend_cluster.conf` do.
* **No ETag, no cache.** Plugins must emit an ETag for the frontend to cache their
  responses, and it must identify the body completely (§5).
* **304 without a backend.** When no backend serves the URI, conditional requests get 304
  rather than 404. That is intended for outages, but it also means a typo'd URL with
  `If-None-Match` looks "unchanged".
* **High-load retirement is backpressure** (see CLAUDE.md and the sputnik engine).
* **Frontend and server versions are coupled** by the heartbeat change (§6).
