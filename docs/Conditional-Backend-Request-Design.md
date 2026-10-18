# Frontend conditional backend requests — design

Status: **proposal, partially implemented** — the RFC 7232 building blocks,
per-encoding entity-tags, the `(ETag, encoding)` response cache and backend
connection reuse have landed (see *Already done* below). The protocol change
itself (one conditional request instead of the ETag probe, and a request→ETag
index) is **not implemented**: every proxied request still starts with the
`X-Request-ETag` probe.
Last reviewed: 2026-10-18 against frontend, spine and server `26.10.17`. Code is
referred to by function name; line numbers are deliberately left out.

Scope: `smartmet-plugin-frontend` (`LowLatencyGatewayStreamer`, `Proxy`) +
`smartmet-library-spine` (protocol constants/helpers) + the **four** backend
plugins that emit an ETag (`wms`, `timeseries`, `edr`, `grid-gui`). See
*Modules to touch* for the exhaustive list — most plugins are unaffected.

## Motivation

On a **frontend-cache miss**, the frontend talks to the backend **twice**:

1. An ETag *probe* — the request is sent with `X-Request-ETag: true`
   (`LowLatencyGatewayStreamer::sendAndListen()`); the backend answers with the
   ETag only (`204 No Content`), read by `readCacheResponse()`.
2. If no variant of that ETag is in the frontend cache, `sendContentRequest()`
   re-sends the request for the full body, read by `readDataResponseHeaders()`.

Since the backend connection pool landed, step 2 normally goes out **on the
probe's connection** (`probeLeftCleanConnection()` confirms the bodyless `204`
left it at a message boundary), so a miss no longer costs a second TCP
connection. It still costs a **second round-trip**, and for **expensive-ETag**
plugins it can waste an entire render (see below). The goal is to collapse the
miss path to **one conditional request**.

## Background: how the ETag is computed differs by plugin

The cost of the probe depends on how the backend derives its ETag:

- **Cheap ETag** — computable from *inputs* without rendering: the data
  generation / model-run identity for the requested timestep, the (JSON) config
  hash, and the request parameters. **WMS** is predominantly here:
  `product.hash_value(theState)` is evaluated **before** any rendering
  (`wms/Plugin.cpp`, `wms/tiles/Handler.cpp`), so a tile for a timestep whose
  gridded data and config are unchanged keeps the same ETag and the backend can
  answer without rendering. `grid-gui` is also cheap (its ETag is a hash of the
  file id, message index, rendering parameters and colour-map modification time).
  Note WMS *falls back* to a body hash when the product hash is unavailable
  (`Fmi::hash_value(theSvg)` / `Fmi::hash_value(*buffer)` in `wms/Plugin.cpp`)
  — that fallback path behaves like an expensive ETag.
- **Expensive ETag** — the ETag *is* a hash of the rendered body. **timeseries**
  and **edr** are here: `product_hash = Fmi::hash_value(*result)`. The
  parameter-only hash was tried and abandoned — the early `etag_only()` call is
  commented out in both plugins. edr still computes `qph.hash_value(state,
  request, q)` for its product cache, so **edr has a cheap input hash on hand** —
  a candidate for the cheap path if it can be shown to encode generation
  identity (principle 4). Here the probe **renders the whole product**, hashes
  it, returns `204`, and discards the body (`etag_only()` in
  `timeseries/Plugin.cpp` and `edr/PluginImpl.cpp`). On a frontend miss the
  content request then renders **again** — a double render unless the backend's
  own product cache still holds it.

So: the second request = a wasted round-trip for cheap-ETag plugins, and
potentially a wasted **render** for expensive-ETag plugins.

## Already done

These are in master; the rest of this document is written against this state.

1. **RFC 7232 evaluation lives in spine** (`spine/HTTP.h`).
   - `Spine::HTTP::ETagFilter` — parses `If-Match` / `If-None-Match` (multiple
     entity-tags, `*`, weak/strong comparison) and
     `evaluate(etag) -> {full_response_required, suggested_status}`;
     `matchingETag(etag)` returns the `If-None-Match` tag that matched.
   - `Spine::HTTP::conditionalResponseStatus(request, etag)` — the backend-side
     convenience layer returning the bodyless status or `std::nullopt`.
   - `Spine::HTTP::request_etag_header` — the `X-Request-ETag` constant.
   - **Crucially**, `conditionalResponseStatus()` returns `std::nullopt` while
     the frontend is probing, so a backend never shortcuts a *client's*
     conditional behind the frontend's back (asserted in
     `spine/test/ETagFilterTest.cpp`). This is exactly the hook the new protocol
     needs: the frontend's own conditional must be distinguishable from a
     forwarded client conditional.
2. **Per-encoding entity-tags** (spine/server/frontend `26.9.29`). An encoded
   response is a different representation, so the server appends the content
   coding to the plugin's entity-tag when it compresses a body
   (`Spine::HTTP::contentCodedETag()`, called from `compress_response()` in
   `server/source/Utility.cpp`): `"abc-timeseries"` becomes
   `"abc-timeseries+zstd"`. `Spine::HTTP::baseETag()` strips the coding again.
   `ETagFilter` matches a coded tag only when the request accepts that coding,
   and a `304` names the variant the client holds
   (`Spine::HTTP::notModifiedETag()`, applied by the server to every `304` in
   `AsyncConnection`). The probe answers with the coding independent tag, since
   it has no body to encode.
3. **Backend-side conditional handling.** These answer `If-None-Match` /
   `If-Match` with `conditionalResponseStatus()` before generating a body:
   - **wms** — all ETag paths (`wms/Plugin.cpp`, GetCapabilities and GetMap in
     `wms/wms/Handler.cpp`, `wms/wmts/Handler.cpp`, `wms/tiles/Handler.cpp`);
   - **edr** — `etag_only()` in `edr/PluginImpl.cpp` (after rendering, since the
     ETag is a body hash), plus its metadata documents;
   - **grid-gui** — its former exact-string `If-None-Match` comparison was
     replaced with `conditionalResponseStatus()`.

   **timeseries** is the exception: its `etag_only()` only answers the probe and
   ignores client conditionals, so it always sends the body.
4. **Frontend cache-hit path uses `ETagFilter`** — `buildCacheResponse()`
   evaluates the client's preconditions, emits `304`/`412` (a `304` carrying
   `matchingETag()`), and always advertises the `ETag` (coded with the served
   variant's encoding) and a `Vary: Accept-Encoding` default. A bare
   `If-Modified-Since` (no entity-tag precondition) is answered `304` on a hit,
   since the frontend keeps no `Last-Modified` and a hit means the ETag is
   current.
5. **One encoding-keyed response cache** — the former compressed/uncompressed
   `ResponseCache` pair is a single cache keyed by `(base ETag, content
   encoding)` (`ResponseCache.{h,cpp}`). Encoding is a free-form lowercased
   token, so any codec caches side by side. Config block `response_cache`
   (legacy `compressed_cache`/`uncompressed_cache` deprecated); admin stats are
   `Frontend::response_cache::*`.
6. **Content-coding negotiation shared with the server** — the server picks the
   coding with `Spine::HTTP::rankContentEncodings()` over
   `supportedContentEncodings()` (q-values honoured, zstd preferred over gzip).
   The frontend's `clientAcceptsContentEncodings()` uses the same function and
   returns **every** acceptable coding, best first, and `findCachedResponse()`
   tries them in that order before falling back to the identity variant. This
   matters because the frontend *cannot* predict the backend's choice: the
   backend may not offer the best coding (`compresscodings`), honours a
   `gzip=1` request parameter, and skips compression below `compressLimit`. Any
   request→ETag index must inherit the same "encoding is a hint, identity is the
   fallback" discipline.
7. **Backend connection reuse** (server keep-alive `26.9.2`, frontend
   `26.9.3`) — `BackendConnectionPool` keeps idle backend connections, and on a
   miss `sendContentRequest()` writes the content request onto the probe's
   connection when `probeLeftCleanConnection()` allows it, instead of closing
   it and opening a new one.
8. **Render-free `304` on the miss path, for backends that check conditionals**
   — the content request is sent without `X-Request-ETag`, so it carries the
   client's `If-None-Match` to the backend undisturbed. wms, edr and grid-gui
   answer it with a bodyless `304`, which `readDataResponseHeaders()` forwards
   (non-`200` responses are not cached). For WMS the `304` costs no render.

Still **not** done: the single conditional request (the probe still precedes
every request), the request→ETag index, conditional handling in timeseries'
`etag_only()`, and an `ETagFilter` evaluation in the frontend's own miss path
(it relies on the backend, so a client conditional reaching timeseries, or a
bare `If-Modified-Since`, gets the full body on a miss).

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

This is strictly ≥ today in every case: a frontend hit stays one round-trip; a
miss drops from **two round-trips to one**; and for expensive-ETag plugins the
**double render is structurally eliminated** because there is only one request.

The design does not depend on backend keep-alive (it is one request and one
response), but it works unchanged on pooled connections.

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
   keyed by `(base ETag, encoding)`, so the index maps a request to a base
   ETag only and the encoding is resolved at lookup time with the existing
   identity fallback (`findCachedResponse()`). Keeping the index encoding-free
   is deliberate: it avoids duplicating a decision the backend makes from
   `Accept-Encoding` + `compresscodings` + `gzip=1` + `compressLimit`.
3. **The backend's current ETag** — computed once per request, cheap or
   expensive per plugin.

### Render-free `304` on the miss path (the high-value WMS case)

The frontend itself produces a client-facing `304` **only on a frontend-cache
hit**, in `buildCacheResponse()`. On the **miss** path (`readDataResponseHeaders()`)
it does not evaluate the client's conditional; it streams whatever the backend
answers to the content request.

For WMS, edr and grid-gui this already gives a render-free (or at least
transfer-free) `304`: the content request carries the client's
`If-None-Match` without the probe header, so the backend answers it (*Already
done*, item 8). What remains is the cost before that answer — the probe, and
for edr the render it causes — and the plugins that do not check conditionals
(timeseries), where a client revalidating against a cold frontend still gets
the full body.

Remaining fix: run the same `ETagFilter::evaluate()` in the miss path against
the ETag the backend returned, and emit `304`/`412` independent of whether the
frontend has bytes cached. This reuses the hit-path machinery so both paths
share one RFC 7232 implementation, and covers backends that do not check
conditionals themselves.

### Frontend changes (bulk of the work)

- New **request→ETag index** (bounded, evictable, config + admin stats).
- `sendAndListen()`: send the candidate as a conditional instead of
  `X-Request-ETag: true`; keep sending the legacy probe header during a
  compatibility window (below).
- **Merge** `readCacheResponse()` and `readDataResponseHeaders()` into one
  handler that switches on the status line: `204`/`304` → serve from cache or
  emit client `304`; `200` → stream + cache. `sendContentRequest()` (already
  reusing the probe's connection) then disappears.
- Keep the pass-through of responses without an ETag (`passThroughResponse()`)
  intact — that is the path all non-caching plugins take.
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
- **edr** — `etag_only()` already answers client conditionals with
  `conditionalResponseStatus()` after rendering. It must accept the new
  frontend conditional the same way.
- **timeseries** — `etag_only()` still understands only the binary "ETag-only
  vs body". Extend it to compare the incoming conditional against the computed
  hash and answer bodyless not-modified, as edr does. No double render because
  there is a single request; the saving is the transfer, not the render. The
  two copies are near-identical — factor the shared logic into spine rather
  than editing both by hand.
- **grid-gui** — done: it uses `conditionalResponseStatus()`, so it is
  probe-aware and handles weak/strong comparison, `*` and `If-Match`.
- Each plugin effectively declares itself cheap or expensive; the frontend
  protocol is identical either way, only the backend cost differs.

### Compatibility

A backend predating the change returns a body the frontend already knows how to
pass through (`passThroughResponse()`, called from `readCacheResponse()`). The frontend's
inward conditional **must not** be mistakable for a *client* conditional, or an
old backend could answer with a spurious `304` that the frontend forwards to a
client that never asked for one. Two viable mechanisms:

1. **Request header** (recommended) — send the candidate in a versioned internal
   header (e.g. `X-Frontend-If-None-Match`) rather than plain `If-None-Match`,
   and have the backend echo support in the response. An old backend ignores the
   unknown header and simply returns the full body — safe by construction, and
   the frontend can then set the index entry and stop sending the probe to that
   backend. The constant and the evaluation belong next to
   `request_etag_header` in `spine/HTTP.h`.
2. **Sputnik capability flag** — the broadcast protocol has a **per-service
   `allowcache` bool** (`BroadcastMessage.proto`) that is currently always
   broadcast as `false` (`sputnik/Messages.cpp`) and never read by the
   frontend; `BackendService::AllowCache()` exists but is unused. It is a
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
| `smartmet-plugin-frontend` | Merge probe+fetch handlers, request→ETag index, miss-path `ETagFilter`, config + admin stats for the index | **Large** — the bulk of the work |
| `smartmet-library-spine` | Internal conditional-request header constant + helper; make the probe-suppression rule aware of the frontend conditional; shared expensive-ETag helper for timeseries/edr | Small, but **must land first** (version bump + `Requires:` bump in every consumer spec) |
| `smartmet-plugin-wms` | Effectively none — already computes cheap ETags pre-render and calls `conditionalResponseStatus()` on all 5 paths; possibly recognise the new header | None → tiny |
| `smartmet-plugin-timeseries` | Teach `etag_only()` to compare an incoming conditional (expensive ETag: saves transfer, not render) | Small |
| `smartmet-plugin-edr` | Already answers client conditionals in `etag_only()`; accept the new frontend conditional | Tiny |
| `smartmet-plugin-grid-gui` | None — already uses `conditionalResponseStatus()` | None |
| `smartmet-engine-sputnik` | **Only if** capability negotiation goes via broadcast (`allowcache` field, or a new version field) | Small, avoidable |
| `smartmet-server` | **None.** Keep-alive, compression and per-encoding ETags already landed | None |
| Other plugins — `admin`, `autocomplete`, `avi`, `backend`, `cross_section`, `download`, `grid-admin`, `meta`, `q3`, `textgen`, `trajectory`, `wfs` | **None.** No ETag emitted → frontend already classifies them non-cacheable; the merged handler must preserve the pass-through (`passThroughResponse()`) | None (regression-test only) |

Ordering: spine → wms (verify) → frontend → timeseries/edr, with a
`Requires: smartmet-library-spine >= <new>` bump in each dependent spec.

## Non-goals

- **Backend keep-alive as the fix.** This has been implemented (the content
  request reuses the probe's connection) and removed the *reconnect*, but it
  keeps the extra round-trip and the expensive-ETag double render, which only
  the single conditional request removes.
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
   body-hash fallback (`wms/Plugin.cpp`) must not be mistaken for the
   cheap path.
3. **Encoding-variant resolution** — the frontend cannot predict the backend's
   encoding choice (`Accept-Encoding` + `compresscodings` + `gzip=1` +
   `compressLimit`). Keep the `(etag, encoding)` lookup with identity fallback
   (`findCachedResponse()`); never key the request→ETag index by encoding.
4. **Streaming classification** — `Proxy::HTTPForward` peeks the first 4096
   bytes to detect backend deny/high-load (`Proxy.cpp`); the merged handler must
   still classify `204`/`304` vs `200` vs error before committing to stream.
5. **Index memory** — new frontend state, potentially higher cardinality than the
   content cache (many requests may map to one ETag); must be bounded/evictable
   and reported in `/admin`.

## Testing

- Miss path uses **one** backend request (assert via backend access logs), not
  two.
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
