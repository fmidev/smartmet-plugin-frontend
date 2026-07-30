# Frontend conditional backend requests — design

Status: **proposal, partially prepared** — the RFC 7232 building blocks and the
cache-variant plumbing have landed (see *Already done* below); the protocol
change itself (single conditional request, render-free `304` on the miss path)
is **not implemented**.
Last reviewed: 2026-07-30 (line references verified against
frontend `26.7.28-1.fmi`, spine `26.7.16`, server `26.7.30-1.fmi`).

Scope: `smartmet-plugin-frontend` (`LowLatencyGatewayStreamer`, `Proxy`) +
`smartmet-library-spine` (protocol constants/helpers) + the **four** backend
plugins that emit an ETag (`wms`, `timeseries`, `edr`, `grid-gui`). See
*Modules to touch* for the exhaustive list — most plugins are unaffected.

## Motivation

On a **frontend-cache miss**, the frontend talks to the backend **twice over two
separate TCP connections**:

1. An ETag *probe* — the request is sent with `X-Request-ETag: true`
   (`LowLatencyGatewayStreamer.cpp:339`); the backend answers with the ETag only
   (`204 No Content`).
2. If the ETag is not in the frontend cache, `sendContentRequest()`
   (`LowLatencyGatewayStreamer.cpp:619`) **closes the socket and opens a new
   one** — "SmartMet doesn't currently support request pipelining"
   (`:624`) — and re-sends the request for the full body.

This wastes a connection and a round-trip on every miss, and for
**expensive-ETag** plugins it can waste an entire render (see below). The goal
is to collapse the miss path to **one conditional request on one connection**.

## Background: how the ETag is computed differs by plugin

The cost of the probe — and therefore what the second connection is really
wasting — depends on how the backend derives its ETag:

- **Cheap ETag** — computable from *inputs* without rendering: the data
  generation / model-run identity for the requested timestep, the (JSON) config
  hash, and the request parameters. **WMS** is predominantly here:
  `product.hash_value(theState)` is evaluated **before** any rendering
  (`wms/Plugin.cpp:308`, `wms/tiles/Handler.cpp:1235`), so a tile for a timestep
  whose gridded data and config are unchanged keeps the same ETag and the
  backend can answer without rendering. `grid-gui` is also cheap (ETag is a hash
  of file id + message index + rendering parameters + colour-map mtime,
  `grid-gui/Plugin.cpp:2243-2251`).
  Note WMS *falls back* to a body hash when the product hash is unavailable
  (`Fmi::hash_value(theSvg)` / `Fmi::hash_value(*buffer)`, `wms/Plugin.cpp:496,541`)
  — that fallback path behaves like an expensive ETag.
- **Expensive ETag** — the ETag *is* a hash of the rendered body. **timeseries**
  and **edr** are here: `product_hash = Fmi::hash_value(*result)`
  (`timeseries/Plugin.cpp:435,500`, `edr/PluginImpl.cpp:441,507`); the
  parameter-only hash was tried and abandoned — commented out entirely in
  timeseries (`Plugin.cpp:411-421`), while edr still computes
  `qph.hash_value(state, request, q)` for its product cache
  (`PluginImpl.cpp:419`) but has its early `etag_only()` call disabled
  (`:432-433`) and overwrites the hash with the body hash. So **edr already has
  a cheap input hash on hand** — a candidate for the cheap path if it can be
  shown to encode generation identity (principle 4). Here the probe
  **renders the whole product**, hashes it, returns `204`, and discards the body
  (`etag_only`, `timeseries/Plugin.cpp:128-152`, `edr/PluginImpl.cpp:155-179`).
  On a frontend miss the content request then renders **again** — a double
  render unless the backend's own product cache still holds it.

So: second connection = a wasted connect+round-trip for cheap-ETag plugins, and
potentially a wasted **render** for expensive-ETag plugins.

## Already done (prerequisites that landed since the first draft)

These are in master and reduce the remaining work; the doc body below is written
against this state.

1. **RFC 7232 evaluation lives in spine** (`spine` 26.7.7, `add-etag-filter`).
   - `Spine::HTTP::ETagFilter` — parses `If-Match` / `If-None-Match` (multiple
     entity-tags, `*`, weak/strong comparison) and
     `evaluate(etag) -> {full_response_required, suggested_status}`
     (`spine/HTTP.h:813-880`).
   - `Spine::HTTP::conditionalResponseStatus(request, etag)` — the backend-side
     convenience layer returning the bodyless status or `std::nullopt`
     (`spine/HTTP.h:926`).
   - `Spine::HTTP::request_etag_header` — the `X-Request-ETag` constant
     (`spine/HTTP.h:904`).
   - **Crucially**, `conditionalResponseStatus()` returns `std::nullopt` while
     the frontend is probing, so a backend never shortcuts a *client's*
     conditional behind the frontend's back (asserted in
     `spine/test/ETagFilterTest.cpp:392-397`). This is exactly the hook the new
     protocol needs: the frontend's own conditional must be distinguishable from
     a forwarded client conditional.
2. **Backend-side conditional handling is complete in WMS** (`wms` 26.7.8,
   `use-etag-filter`) — *all* ETag paths honour `If-None-Match`/`If-Match`
   before generating a body: `wms/Plugin.cpp:339`, `wms/wms/Handler.cpp:1056`
   (GetCapabilities) and `:1138`, `wms/wmts/Handler.cpp:725`,
   `wms/tiles/Handler.cpp:1248`. The earlier draft claimed only
   GetCapabilities had this. **Consequence: for WMS the backend half of this
   design already exists.** Once the frontend stops sending the probe header and
   sends its candidate as a conditional instead, WMS answers render-free.
3. **Frontend cache-hit path uses `ETagFilter`** (`frontend` 26.7.8) —
   `buildCacheResponse()` (`LowLatencyGatewayStreamer.cpp:110`) evaluates
   preconditions at `:174-183`, emits `304`/`412`, and now always advertises
   the `ETag` and a `Vary: Accept-Encoding` default on conditional responses
   (`:143-156`) as RFC 7232 requires.
4. **One encoding-keyed response cache** (`frontend` 26.7.14, commit `edf907f`) —
   the former compressed/uncompressed `ResponseCache` pair is a single cache
   keyed by `(ETag, content-encoding-token)` (`ResponseCache.cpp:15-19`,
   `ResponseCache.h:28-43`). Encoding is a free-form lowercased token, so any
   codec caches side by side. Config block `response_cache`
   (legacy `compressed_cache`/`uncompressed_cache` deprecated); admin stats are
   `Frontend::response_cache::*`.
5. **zstd on the backend + matching frontend negotiation** (`server` 26.7.14,
   commit `db7a94f`) — `select_content_encoding()` (`server/source/Utility.cpp:75`)
   prefers zstd over gzip, called from `AsyncConnection::startRegularReply()`
   (`server/source/AsyncConnection.cpp:1023`); the frontend's
   `clientAcceptsContentEncoding()` mirrors that preference order
   (`LowLatencyGatewayStreamer.cpp:44-69`) so zstd variants hit the cache.
   Only the non-streamed reply path is compressed; chunked replies deliberately
   are not.
6. **Miss-path lookup already tolerates encoding mismatch** — after the probe
   the frontend looks up `(etag, preferred-encoding)` and falls back to
   `(etag, "")` (`LowLatencyGatewayStreamer.cpp:554-563`). This matters because
   the frontend *cannot* predict the backend's choice: the backend also honours
   a `gzip=1` request parameter and skips compression below `compressLimit`
   (`server/source/Utility.cpp:94-104`). Any request→ETag index must inherit the
   same "encoding is a hint, identity is the fallback" discipline.

Still **not** done: the single-connection conditional protocol, the
request→ETag index, and the render-free `304` on the miss path
(`readDataResponseHeaders`, `LowLatencyGatewayStreamer.cpp:683-795`, still
streams the backend body unconditionally). Server-side HTTP keep-alive is also
unimplemented and, per *Non-goals*, not needed for this design.

## Correctness principles (these constrain the whole design)

1. **Freshness is data-driven, not time-driven.** Forecast data changes on
   background model runs on no schedule the frontend can predict; the same
   querystring yields a different ETag after a new run. Revalidation against the
   backend ETag is therefore the *correctness mechanism* — `Expires`/`max-age`
   cannot replace it (a longer TTL serves stale forecasts after a run).
   Observations rarely change; forecasts change frequently — the same protocol
   serves both, only the revalidation *hit rate* differs.
2. **The content cache (ETag→bytes) is content-addressed and inherently correct
   under background updates.** A new model run = new ETag = new key; the stale
   entry ages out via LRU and can never be served as current. The problem
   reduces to *discovering the current ETag cheaply*.
3. **A request→ETag mapping is a self-correcting hint, never a source of
   truth.** It goes stale on data events (expect a burst of misses at every
   model-run boundary), and a wrong candidate simply yields a full body — the
   same as a miss today.
4. **A cheap ETag MUST encode the data-generation / model-run identity** of every
   producer feeding the requested timestep, not just the querystring and config.
   Otherwise it silently serves pre-run data — the worst, quietest failure mode.
   This is the precondition that decides which plugins may opt into the cheap
   path (WMS and grid-gui can; timeseries/edr cannot, hence they hash the body).

## Design

### Fold probe + fetch into one conditional request

Replace the two-phase probe/fetch with a single conditional GET on one
connection:

1. The frontend derives a **candidate ETag** for the request (sources below) and
   sends **one** request carrying it as a conditional (instead of
   `X-Request-ETag: true`).
2. The backend computes its current ETag and, on the **same connection**:
   - candidate matches → bodyless **not-modified** reply → the frontend serves
     the bytes it holds for that ETag;
   - no match / no candidate → `200` + **body** + `ETag` → the frontend streams
     it, caches by ETag, and updates its hint.

This is strictly ≥ today in every case: frontend hit stays one connection; miss
drops from **two connections to one**; and for expensive-ETag plugins the
**double render is structurally eliminated** because there is only one request.

Note this needs **no** server-side keep-alive: it is still one request and one
response per connection, so the existing `Connection: close` model is untouched.

**Status-code detail.** The existing probe answers `204 No Content`; a
conditional match answers `304 Not Modified` (that is what
`conditionalResponseStatus()` returns, and it is the RFC-correct code). The
merged frontend handler must therefore treat **both** `204` and `304` +`ETag` as
"your candidate is current, serve your bytes", and must not confuse a backend
`304` that answers *the frontend's* conditional with one that answers *the
client's* — see *Compatibility* for how the two are kept apart.

### Three sources of the candidate ETag

The frontend should try, in order:

1. **The client's own conditional** — forward it inward when present. For WMS
   this is usually the right candidate already (a browser/map client
   revalidating a specific tile).
2. **A frontend request→ETag index** — a bounded, evictable map (normalized
   request → last-known ETag) that synthesises a candidate when the client did
   not send one. Its key **must** include everything that selects a
   representation — at minimum URI + query string + the `Vary` inputs. Note the
   *variant* dimension is now handled by the content cache itself, which is
   keyed by `(ETag, encoding)` (`ResponseCache.cpp:15-19`), so the index maps a
   request to an ETag only and the encoding is resolved at lookup time with the
   existing identity fallback (`:554-563`). Keeping the index encoding-free is
   deliberate: it avoids duplicating a decision the backend makes from
   `Accept-Encoding` + `gzip=1` + `compressLimit`.
3. **The backend's current ETag** — computed once per request, cheap or
   expensive per plugin.

### Render-free `304` on the miss path (the high-value WMS case)

Today the client-facing `304` is produced **only on a frontend-cache hit**, in
`buildCacheResponse()` (`LowLatencyGatewayStreamer.cpp:110`, evaluation at
`:174`). On the **miss** path (`readDataResponseHeaders`, `:683`) the backend
body is streamed to the client unconditionally — there is no client-conditional
check (`:729-786`).

Consequence today: a client revalidating a tile with `If-None-Match: X`, when
the frontend does not hold `X` (cold node, evicted, or a different frontend in
the pool), triggers a **full backend render + full tile transfer** that the
client discards. For WMS — cheap ETag, constant revalidation — this is the most
wasteful path in the system. (The backend would already have answered `304`
without rendering; it is the frontend's probe header that suppresses it.)

Fix: when the backend cheaply confirms the client's candidate is current, the
frontend must answer the client with `304` **with no render and no frontend
cache entry**. Concretely, teach the miss path to run the same
`ETagFilter::evaluate()` against the backend's returned ETag and emit
`304`/`412` accordingly, independent of whether the frontend has bytes cached.
This reuses the hit-path machinery so both paths share one RFC 7232
implementation.

### Frontend changes (bulk of the work)

- New **request→ETag index** (bounded, evictable, config + admin stats).
- `sendAndListen()`: send the candidate as a conditional instead of
  `X-Request-ETag: true` (`:339`, removal at `:354`); keep sending the legacy
  probe header during a compatibility window (below).
- **Merge** `readCacheResponse` (`:468`) and `readDataResponseHeaders` (`:683`)
  into one handler that switches on the status line: `204`/`304` → serve from
  cache or emit client `304`; `200` → stream + cache; on the **same** socket.
  **Delete** the close/reconnect in `sendContentRequest()` (`:619-635`).
- Keep the `!etagHeader` pass-through branch (`:526-548`) intact — that is the
  path all non-caching plugins take.
- Maintain the index: on a `200` carrying an ETag, record request→ETag after
  caching; on a not-modified reply, refresh freshness.
- Route the backend ETag through `Spine::HTTP::ETagFilter` on the miss path so
  client `304`/`412` is possible without a cache entry.

### Backend-plugin changes

Most of this is already in place; what remains is small and per-plugin:

- **wms** — nothing structural. It computes the ETag before rendering and calls
  `conditionalResponseStatus()` on every ETag path. It only needs to recognise
  the new frontend conditional as a conditional (i.e. not be suppressed as a
  probe) — which is a spine-level change to the suppression rule, not plugin
  code, unless the negotiation header is inspected locally.
- **timeseries** / **edr** — `etag_only()` still understands only the binary
  "ETag-only vs body" (`timeseries/Plugin.cpp:128-152`,
  `edr/PluginImpl.cpp:155-179`). Extend it to compare the incoming conditional
  against the computed hash and answer bodyless not-modified. No double render
  because there is a single request; the saving is the transfer, not the render.
  The two copies are near-identical — factor the shared logic into spine rather
  than editing both by hand.
- **grid-gui** — replace its ad-hoc exact-string `If-None-Match` compare
  (`grid-gui/Plugin.cpp:2253-2255`, and the same pattern at `:2479`, `:2651`)
  with `conditionalResponseStatus()`. As written it ignores weak/strong
  comparison, `*`, `If-Match`, and — more importantly — it is **not probe-aware**,
  so it can answer a frontend probe with `304`. Cleanup, not a blocker.
- Each plugin effectively declares itself cheap or expensive; the frontend
  protocol is identical either way, only the backend cost differs.

### Compatibility

A backend predating the change returns a body the frontend already knows how to
pass through (the `!etagHeader` branch in `readCacheResponse`). The frontend's
inward conditional **must not** be mistakable for a *client* conditional, or an
old backend could answer with a spurious `304` that the frontend forwards to a
client that never asked for one. Two viable mechanisms:

1. **Request header** (recommended) — send the candidate in a versioned internal
   header (e.g. `X-Frontend-If-None-Match`) rather than plain `If-None-Match`,
   and have the backend echo support in the response. An old backend ignores the
   unknown header and simply returns the full body — safe by construction, and
   the frontend can then set the index entry and stop sending the probe to that
   backend. The constant and the evaluation belong next to
   `request_etag_header` in `spine/HTTP.h:904`.
2. **Sputnik capability flag** — the broadcast protocol has a **per-service
   `allowcache` bool** (`BroadcastMessage.pb.h:501-511`) that is currently
   always broadcast as `false` (`sputnik/Messages.cpp:106`) and never read by
   the frontend; `BackendService::AllowCache()` exists but is unused. It is a
   natural carrier for "this handler supports conditional requests". Note the
   broadcast carries **no version field**, so the earlier draft's suggestion to
   "gate on the backend version advertised via Sputnik" is not possible without
   extending the protobuf.

Option 1 needs no engine change and degrades safely; option 2 avoids a per-request
header at the cost of touching sputnik and a protobuf field.

## Modules to touch

**No — not all plugins.** Only plugins that emit an `ETag` participate in
frontend caching at all; the other 13 are pass-through
(`grep 'setHeader("ETag"'` over `brainstorm/plugins` is the authoritative list).

| Repo / module | Change | Size |
| --- | --- | --- |
| `smartmet-plugin-frontend` | Merge probe+fetch handlers, drop the reconnect, request→ETag index, miss-path `ETagFilter`, config + admin stats for the index | **Large** — the bulk of the work |
| `smartmet-library-spine` | Internal conditional-request header constant + helper; make the probe-suppression rule aware of the frontend conditional; shared expensive-ETag helper for timeseries/edr | Small, but **must land first** (version bump + `Requires:` bump in every consumer spec) |
| `smartmet-plugin-wms` | Effectively none — already computes cheap ETags pre-render and calls `conditionalResponseStatus()` on all 5 paths; possibly recognise the new header | None → tiny |
| `smartmet-plugin-timeseries` | Teach `etag_only()` to compare an incoming conditional (expensive ETag: saves transfer, not render) | Small |
| `smartmet-plugin-edr` | Same change as timeseries (duplicated code) | Small |
| `smartmet-plugin-grid-gui` | Migrate ad-hoc `If-None-Match` compare to `conditionalResponseStatus()`; make it probe-aware | Small, optional |
| `smartmet-engine-sputnik` | **Only if** capability negotiation goes via broadcast (`allowcache` field, or a new version field) | Small, avoidable |
| `smartmet-server` | **None.** Still one request per connection; compression/zstd already landed. Only touched if keep-alive is later pursued (see `server/docs/HTTP-KeepAlive-Design.md`) | None |
| Other plugins — `admin`, `autocomplete`, `avi`, `backend`, `cross_section`, `download`, `grid-admin`, `meta`, `q3`, `textgen`, `trajectory`, `wfs` | **None.** No ETag emitted → frontend already classifies them non-cacheable; the merged handler must preserve the `!etagHeader` pass-through | None (regression-test only) |

Ordering: spine → wms (verify) → frontend → timeseries/edr → grid-gui, with a
`Requires: smartmet-library-spine >= <new>` bump in each dependent spec.

## Non-goals

- **Backend keep-alive as the fix.** Keeping connection #1 open and sending the
  content request on it removes only the *reconnect* — it keeps the extra
  round-trip and the expensive-ETag double render, and needs backend server-side
  keep-alive plus a framing change (see the server repo's
  `docs/HTTP-KeepAlive-Design.md`, still unimplemented). Strictly inferior for
  this problem; only a fallback if touching backend plugins is off the table.
- **Replacing revalidation with longer TTLs** — precluded by correctness
  principle 1.
- **Compressing streamed/chunked replies** — settled separately when zstd
  landed; out of scope here.

## Risks

1. **Frontend conditional mistaken for a client conditional** — an old or
   inattentive backend answering the frontend's hint with `304` while the client
   sent no conditional would produce a bogus client `304`. Mitigated by the
   versioned internal header (Compatibility option 1) and by the existing
   probe-suppression precedent in `conditionalResponseStatus()`. Highest-severity
   risk now that the variant key is handled by the cache.
2. **Cheap-ETag omitting generation identity** — silently serves pre-run data
   (correctness principle 4). Enforce that only plugins that can enumerate all
   inputs *including upstream data version* opt into the cheap path. WMS's
   body-hash fallback (`wms/Plugin.cpp:496,540`) must not be mistaken for the
   cheap path.
3. **Encoding-variant resolution** — the frontend cannot predict the backend's
   encoding choice (`Accept-Encoding` + `gzip=1` + `compressLimit`). Keep the
   `(etag, encoding)` lookup with identity fallback (`:554-563`); never key the
   request→ETag index by encoding.
4. **Streaming classification** — `Proxy::HTTPForward` peeks the first 4096
   bytes to detect backend deny/high-load (`Proxy.cpp`); the merged handler must
   still classify `204`/`304` vs `200` vs error before committing to stream.
5. **Index memory** — new frontend state, potentially higher cardinality than the
   content cache (many requests may map to one ETag); must be bounded/evictable
   and reported in `/admin`.

## Testing

- Miss path uses **one** backend connection (assert via backend connection
  count / logs), not two.
- Expensive-ETag plugin (timeseries): a cold frontend miss renders **once**, not
  twice.
- WMS client revalidation against a **cold** frontend with unchanged data →
  render-free `304`, no body transferred.
- Model-run boundary: after a new generation for a producer, the next request
  for an affected query returns a fresh `200`+body (new ETag), and subsequent
  identical requests revalidate to not-modified.
- Variant correctness: gzip-capable, zstd-capable and identity-only clients for
  the same query each receive a decodable encoding (cache keyed
  `(ETag, encoding)`, identity fallback exercised).
- No spurious `304`: a client sending **no** conditional never receives one,
  even though the frontend sent a candidate inward.
- Pass-through regression: a non-ETag plugin (e.g. `download`, `wfs`) still
  streams unchanged through the merged handler.
- Mixed-version cluster: new frontend against an old backend falls back to
  pass-through without spurious `304`s.
