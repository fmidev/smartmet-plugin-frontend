# Frontend conditional backend requests — design

Status: **proposal** (not yet implemented)
Scope: `smartmet-plugin-frontend` (`LowLatencyGatewayStreamer`, `Proxy`,
`ResponseCache`) + a small, replicated change in caching backend plugins
(`timeseries`, `wms`, …) + spine protocol support.

## Motivation

On a **frontend-cache miss**, the frontend talks to the backend **twice over two
separate TCP connections**:

1. An ETag *probe* — the request is sent with `X-Request-ETag: true`
   (`LowLatencyGatewayStreamer.cpp:308`); the backend answers with the ETag only
   (`204 No Content`).
2. If the ETag is not in the frontend cache, `sendContentRequest()`
   (`LowLatencyGatewayStreamer.cpp:595`) **closes the socket and opens a new
   one** — "SmartMet doesn't currently support request pipelining"
   (`:600`) — and re-sends the request for the full body.

This wastes a connection and a round-trip on every miss, and for
**expensive-ETag** plugins it can waste an entire render (see below). The goal
is to collapse the miss path to **one conditional request on one connection**.

## Background: how the ETag is computed differs by plugin

The cost of the probe — and therefore what the second connection is really
wasting — depends on how the backend derives its ETag:

- **Cheap ETag** — computable from *inputs* without rendering: the data
  generation / model-run identity for the requested timestep, the (JSON) config
  hash, and the request parameters. **WMS** is predominantly here: the tile for a
  timestep whose gridded data has not changed (and whose config is unchanged)
  keeps the same ETag, so the backend can answer the probe without rendering.
  WMS already computes such ETags and honours `If-None-Match`→`304` /
  `X-Request-ETag`→`204` for GetCapabilities (`wms/wms/Handler.cpp:1052-1128`).
- **Expensive ETag** — the ETag *is* a hash of the rendered body. **timeseries**
  is here: `product_hash = Fmi::hash_value(*result)`
  (`timeseries/Plugin.cpp:435,500`); a parameter-only hash was tried and
  abandoned (`:411-421`). Here the probe **renders the whole product**, hashes
  it, returns `204`, and discards the body (`etag_only`, `:128-152`). On a
  frontend miss the content request then renders **again** — a double render
  unless the backend's own product cache still holds it.

So: second connection = a wasted connect+round-trip for cheap-ETag plugins, and
potentially a wasted **render** for expensive-ETag plugins.

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
   path (WMS can; timeseries cannot, hence it hashes the body).

## Design

### Fold probe + fetch into one conditional request

Replace the two-phase probe/fetch with a single conditional GET on one
connection:

1. The frontend derives a **candidate ETag** for the request (sources below) and
   sends **one** request carrying it as `If-None-Match` (instead of
   `X-Request-ETag: true`).
2. The backend computes its current ETag and, on the **same connection**:
   - candidate matches → `204 No Content` → the frontend serves the bytes it
     holds for that ETag;
   - no match / no candidate → `200` + **body** + `ETag` → the frontend streams
     it, caches by ETag, and updates its hint.

This is strictly ≥ today in every case: frontend hit stays one connection; miss
drops from **two connections to one**; and for expensive-ETag plugins the
**double render is structurally eliminated** because there is only one request.

### Three sources of the candidate ETag

The frontend should try, in order:

1. **The client's own `If-None-Match`** — forward it inward when present. For
   WMS this is usually the right candidate already (a browser/map client
   revalidating a specific tile).
2. **A frontend request→ETag index** — a bounded, evictable map (normalized
   request → last-known ETag) that synthesises a candidate when the client did
   not send one. Its key **must** include everything that selects a cache
   variant — at minimum URI + query string + `Accept-Encoding` (gzip and
   uncompressed are separate cache entries, `LowLatencyGatewayStreamer.cpp:531-538`)
   and any `Vary` inputs. A wrong *variant* key is the sharpest correctness risk
   (serving gzip to a client that cannot decode it); a merely *stale* entry is
   harmless (§correctness-3).
3. **The backend's current ETag** — computed once per request, cheap or
   expensive per plugin.

### Render-free `304` on the miss path (the high-value WMS case)

Today the client-facing `304` is produced **only on a frontend-cache hit**, in
`buildCacheResponse()` (`LowLatencyGatewayStreamer.cpp:118`), which now uses the
new `Spine::HTTP::ETagFilter` (spine ≥ 26.7.7) to evaluate `If-Match` /
`If-None-Match` / `If-Modified-Since` per RFC 7232. On the **miss** path
(`readDataResponseHeaders`, `:659`) the backend body is streamed to the client
unconditionally — there is no client-`If-None-Match` check.

Consequence today: a client revalidating a tile with `If-None-Match: X`, when
the frontend does not hold `X` (cold node, evicted, or a different frontend in
the pool), triggers a **full backend render + full tile transfer** that the
client discards. For WMS — cheap ETag, constant revalidation — this is the most
wasteful path in the system.

Fix: when the backend cheaply confirms the client's candidate is current
(returns `204`/not-modified), the frontend must be able to answer the client
with `304` **with no render and no frontend cache entry**. Concretely, teach the
miss path to run the same `ETagFilter::evaluate()` against the backend's
returned ETag and emit `304`/`412` accordingly, independent of whether the
frontend has bytes cached. This reuses the machinery the maintainer just added
on the hit path, so both paths share one RFC 7232 implementation.

### Frontend changes (bulk of the work)

- New **request→ETag index** (with the variant-complete key above).
- `sendAndListen()`: send `If-None-Match: <candidate>` instead of
  `X-Request-ETag: true` (`:308`); keep sending the legacy probe header during a
  compatibility window (below).
- **Merge** `readCacheResponse` (`:437`) and `readDataResponseHeaders` (`:659`)
  into one handler that switches on the status line: `204` → serve from cache /
  emit client `304`; `200` → stream + cache; on the **same** socket. **Delete**
  the close/reconnect in `sendContentRequest()` (`:595`).
- Maintain the index: on a `200` carrying an ETag, record request→ETag after
  caching; on a `204`, refresh freshness.
- Route the backend ETag through `Spine::HTTP::ETagFilter` on the miss path so
  client `304`/`412` is possible without a cache entry.

### Backend-plugin changes (WMS first)

- Accept the candidate ETag (via `If-None-Match`, or a versioned internal
  header) and **compare** it to the computed ETag: equal → `204`, else full
  body. Today plugins only understand the binary "ETag-only vs body"
  (`etag_only`, `timeseries/Plugin.cpp:128-152`).
- **Cheap-ETag plugins** compute the ETag *before* rendering and short-circuit —
  this is the only case that saves real backend work. WMS already has most of
  this machinery (`Handler.cpp:1052-1128`), making it the natural first mover.
- **Expensive-ETag plugins** render once, then compare — no double render
  because there is a single request.
- Each plugin effectively declares itself cheap or expensive; the frontend
  protocol is identical either way, only the backend cost differs.

### Compatibility

A backend predating the change returns a body the frontend already knows how to
pass through (the `!etagHeader` branch in `readCacheResponse`). Add explicit
capability negotiation (e.g. an `X-Frontend-Conditional: 1` request header, or
gate on the backend version advertised via Sputnik) so a new frontend does not
send inward `If-None-Match` that an old backend might mistake for a *client*
conditional and answer with a spurious `304`.

## Non-goals

- **Backend keep-alive as the fix.** Keeping connection #1 open and sending the
  content request on it removes only the *reconnect* — it keeps the extra
  round-trip and the expensive-ETag double render, and needs backend server-side
  keep-alive plus a framing change (see the server repo's
  `HTTP-KeepAlive-Design.md`). Strictly inferior for this problem; only a
  fallback if touching backend plugins is off the table.
- **Replacing revalidation with longer TTLs** — precluded by correctness
  principle 1.

## Risks

1. **Variant-key correctness** — the request→ETag index key must capture every
   body-affecting / variant-selecting input. A wrong variant is not
   self-correcting (unlike staleness). Highest-severity risk.
2. **Cheap-ETag omitting generation identity** — silently serves pre-run data
   (correctness principle 4). Enforce that only plugins that can enumerate all
   inputs *including upstream data version* opt into the cheap path.
3. **Streaming classification** — `Proxy::HTTPForward` peeks the first 4096
   bytes to detect backend deny/high-load (`Proxy.cpp`); the merged handler must
   still classify `204` vs `200` vs error before committing to stream.
4. **Index memory** — new frontend state, potentially higher cardinality than the
   content cache (many requests may map to one ETag); must be bounded/evictable.

## Testing

- Miss path uses **one** backend connection (assert via backend connection
  count / logs), not two.
- Expensive-ETag plugin (timeseries): a cold frontend miss renders **once**, not
  twice.
- WMS client revalidation against a **cold** frontend with unchanged data →
  render-free `304`, no body transferred.
- Model-run boundary: after a new generation for a producer, the next request
  for an affected query returns a fresh `200`+body (new ETag), and subsequent
  identical requests revalidate to `304`/`204`.
- Variant correctness: a gzip-capable and a gzip-incapable client for the same
  query receive the correct encoding (index key includes `Accept-Encoding`).
- Mixed-version cluster: new frontend against an old backend falls back to
  pass-through without spurious `304`s.
