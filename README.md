# ecall-router

Routing IMS emergency calls (`urn:service:sos`) to the right PSAP, done two
ways:

- **serial**: ask the LRF where the caller is, then ask the RDF which PSAP
  covers that location, then route. The call waits for both round trips.
- **fast**: route straight away on the caller's tracking area (taken from
  the cell ID in `P-Access-Network-Info`) using a local cell-to-PSAP table.
  The precise location goes *by reference* (`Geolocation:` URI, RFC 6442).
  The LRF query runs in the background, and the PSAP fetches the location
  when it needs it.

```
serial:  INVITE ─▶ LRF (location) ─▶ RDF (PSAP) ─▶ route
fast:    INVITE ─▶ cell → TAC → PSAP table ─▶ route (Geolocation by reference)
                        └────────▶ LRF query in the background
```

This follows the split 3GPP TS 23.167 allows between routing, which needs to
be fast, and location delivery, which needs to be precise. The LRF and RDF
here are real UDP services on loopback with a configurable delay, so the
serial path pays genuine round trips.

> This is a clean-room re-implementation of the idea behind the emergency
> services work I did at C-DOT (E-CSCF, LRF and RDF, with optimised routing
> and location handling). It contains no C-DOT code, and its tables are
> synthetic. The numbers below come from this repository only.

## Build and run

```sh
make test                  # parser, tables, both strategies agree, LRF timeout
make asan
./ecall-bench 200 10 5     # calls, LRF delay ms, RDF delay ms
```

## Results

2-vCPU cloud VM, gcc 13, 200 calls per strategy, run in separate phases:

```
200 emergency INVITEs, LRF 10 ms, RDF 5 ms (simulated over UDP on loopback)
serial  p50   15.579 ms   p99   15.913 ms   max   26.427 ms   fallbacks 0
fast    p50    0.003 ms   p99    0.011 ms   max    0.029 ms   fallbacks 0
fast path: 200 background LRF queries, 200 answered; backlog drained
           2031 ms after the last call was routed
```

How to read it: the fast path takes the remote lookups off the call-setup
path, so routing time no longer depends on LRF or RDF latency. The last line
is the catch. The simulated LRF answers one query at a time, so a burst of
200 calls leaves location answers arriving up to 2 s late. Routing isn't
delayed, but the PSAP would see the precise location late. In a real
deployment the LRF needs capacity for peak call rates. Moving work off the
critical path doesn't remove it.

## Behaviour worth checking in review

- **Both strategies pick the same PSAP** for every cell and service in the
  table. That's a test (24 cases).
- **Never block an emergency call.** If the LRF or RDF doesn't answer within
  the deadline, or the cell is unknown, the call goes to the default PSAP for
  that service. It is still routed, never rejected. That's a test too.
- **Fallback order:** (tracking area, service), then (tracking area, sos),
  then (default, service), then (default, sos).
- **RFC 5031 service URNs** are parsed case-insensitively. An unknown
  `sos.xxx` sub-service is still treated as an emergency.
- **The input parser is strict:** CRLF only, bounded line length, Call-ID
  required. Call-IDs containing spaces or control characters are refused
  before they reach the LRF text protocol.

## Not done yet

- A real SIP transport and transactions: the router works on message buffers
- PIDF-LO location by value, and HELD (RFC 5985) for dereferencing
- LRF capacity: a pool of workers instead of one serial server

MIT licensed.
