# `mrtk run` JSON-RPC Interface

> **Status:** Draft — protocol 0.1, not implemented yet ·
> **Tracking:** [#326](https://github.com/h-shiono/MRTKLIB/issues/326) ·
> **Contract:** [`openrpc.json`](openrpc.json)
>
> This document is the reference for the JSON-RPC 2.0 interface of the
> real-time server `mrtk run` (rtkrcv). Frontends such as GUIs use it instead
> of parsing the text of the telnet console. It states what each method and
> event means and, where the choice was not obvious, why.
> [`openrpc.json`](openrpc.json) is the machine-readable contract for the same
> protocol version; mocks and contract tests validate against it.

---

## 1. Overview

- The server opens one WebSocket endpoint. Requests and responses are
  JSON-RPC 2.0 messages in text frames.
- Several clients may be connected at the same time. They all control and
  observe the same server.
- State is read with methods (pull). Events — solutions, lifecycle changes
  and log messages — carry a sequence number (`seq`) and are held in a buffer;
  a client fetches "all events after `seq` N" whenever it wants.

  *Why pull:* every known client (an RTKNAVI-style GUI, the telnet console,
  the docker UI) already polls on a timer. Numbered events let a polling
  client miss nothing without the server keeping a queue per client, and a
  push mode can be added later without changing the event format.

A typical client:

1. starts `mrtk run` with a token and waits for the ready line (§2),
2. connects with the token (§3),
3. calls `sync` to get the current state and the event boundary `seq` (§7),
4. calls `getEvents(after = seq)` on a timer (for example every 100 ms) and
   applies the events in order,
5. calls `start` / `stop` when the user asks and follows the outcome in the
   status events (§5),
6. calls `sync` again if `getEvents` reports lost events.

Protocol 0.1 covers process startup, authentication, the lifecycle, the
status and the solution and log events. Configuration, streams and satellites
follow in later versions (§11).

## 2. Starting the server

New command-line options of `mrtk run` (all other options are unchanged):

| Option | Meaning |
|---|---|
| `--rpc-port PORT` | Open the RPC endpoint on `PORT`. `0` lets the operating system choose a free port. Without this option no RPC endpoint is opened. |
| `--rpc-bind ADDR` | Address to listen on. Default `127.0.0.1`. Any other address must be given explicitly. |
| `--rpc-origin ORIGIN` | Accept browser connections from `ORIGIN`, for example `http://localhost:8080`. May be repeated. Default: no browser origin is accepted. |
| `--no-console` | Do not open the local console. Together with `--rpc-port` and without `-p`, the process serves only the RPC endpoint. Requires `--rpc-port`. |
| `--rpc-no-auth` | For testing only: accept connections without a token (§3). Refused unless the endpoint listens on a loopback address. The server prints a warning, and `getVersion` reports `"authentication": "none"`. This cannot be set in the configuration file or over RPC. |

### Token

The process that starts `mrtk run` (the parent, for example a GUI) generates
a random token and passes it in the environment variable `MRTK_RPC_TOKEN`.
Generate 32 random bytes and encode them in base64url without padding
(43 characters). The token may contain only base64url characters
(`A`–`Z`, `a`–`z`, `0`–`9`, `-`, `_`), because it travels in a WebSocket
handshake header (§3); the server refuses to start with any other character.

The server reads the variable at startup and removes it from its own
environment, so programs that the server runs (`misc-startcmd`, downloads)
do not inherit it.

If `MRTK_RPC_TOKEN` is not set and `--rpc-no-auth` is not given:

- when stderr is a terminal, the server generates a token and prints it on
  stderr for the person who started it;
- otherwise the server exits with an error that names `MRTK_RPC_TOKEN`.

*Why:* a parent that forgot to pass a token would otherwise receive a
generated token among the child's output, which frontends often copy into
their logs.

### Ready line

When the endpoint is listening, the server prints one line on stderr:

```text
MRTK-RPC-READY {"url":"ws://127.0.0.1:43215/rpc"}
```

The parent reads stderr lines until one starts with `MRTK-RPC-READY `, and
parses the JSON object after it. The object may gain members in later
versions; ignore unknown ones. The line never contains the token. When the
endpoint listens on all addresses, `url` uses the loopback address
(`127.0.0.1`, or `[::1]` for IPv6). If the
port cannot be bound, the server exits with a non-zero status and prints no
ready line.

*Why stderr:* stdout may carry the local console or solution output.
*Why a line instead of a fixed wait:* a fixed port can be taken by another
program, and a fixed delay is too short on slow machines and wasted on fast
ones; the line also tells "still starting" apart from "failed".

Example:

```sh
MRTK_RPC_TOKEN="$(openssl rand -base64 32 | tr '+/' '-_' | tr -d '=')" \
  mrtk run --config conf/run.toml --rpc-port 0 --no-console
```

### Ending the process

Use the `shutdown` method (§5). SIGINT, SIGTERM and the Windows console
control events take the same path: positioning stops, navigation data is
saved and the process exits.

## 3. Connecting

Connect to the `url` of the ready line (`ws://HOST:PORT/rpc`) and offer two
WebSocket subprotocols: the protocol name `mrtk-rpc` and the token entry
`mrtk-token.<token>`.

```text
Sec-WebSocket-Protocol: mrtk-rpc, mrtk-token.<token>
```

In a browser:

```js
const ws = new WebSocket(url, ["mrtk-rpc", "mrtk-token." + token]);
```

Native clients use the same header; there is only one way to present the
token.

The server checks the request before upgrading the connection:

| Condition | Response |
|---|---|
| `mrtk-rpc` not offered | HTTP 400 |
| `Origin` header present and not allowed by `--rpc-origin` | HTTP 403 |
| token entry missing or wrong (unless `--rpc-no-auth`) | HTTP 401 |
| otherwise | upgrade; the server selects `mrtk-rpc` and never echoes the token entry |

Requests without an `Origin` header come from non-browser programs and skip
the origin check. The origin check stops web pages open in a browser on the
same machine; the token stops every other program and user.

*Why the handshake:* no unauthenticated connection ever exists, so every
request on a connection is authenticated, and the token is checked at the
same place as the origin. Browsers cannot set an `Authorization` header on a
WebSocket, and a token in the URL query would end up in proxy logs.
*Limitation:* a browser only sees a failed connection, without the HTTP
status. Native clients receive the status.

### Encryption

Protocol 0.x has no TLS. The token travels in clear text in the handshake.
On a loopback address the traffic does not leave the machine. For any other
address, put the server behind a reverse proxy or VPN that provides TLS; the
proxy must forward the `Sec-WebSocket-Protocol` header. Encryption does not
replace the token: TLS proves the server to the client, not the client to
the server.

### What the token allows

A client that holds the token can do anything that the operating-system user
running the server can do: the configuration can direct output to any path
and run commands. Treat the token like a password. The server never shows it
in status, events or log messages.

### JSON-RPC rules

- Every request needs an `id`. Requests without an `id` (notifications) are
  not executed.
- Batch requests are not supported in 0.x; a batch is answered with error
  `-32600`.
- Parameters are passed by name (`params` is an object). For methods without
  parameters, `params` may be omitted or `{}`.
- Requests on one connection may be processed concurrently, so responses can
  arrive in a different order than the requests. Match them by `id`.
- Binary frames close the connection with WebSocket status 1003, which
  RFC 6455 defines for an endpoint that receives a type of data it cannot
  accept (such as binary data at a text-only endpoint). When the process
  exits, connections close with status 1001 ("going away", defined for a
  server going down), so a client can tell this from a broken connection,
  which ends without a close frame.

## 4. Versioning

`getVersion` returns `protocolVersion` as `MAJOR.MINOR`.

- **0.x (now):** prototype. Any change may be incompatible. The version
  becomes 1.0 when the protocol is agreed.
- **From 1.0:** a minor version adds things (methods, optional parameters,
  result and event members, values of sets documented as open); a major
  version breaks compatibility.

Clients must:

- ignore members they do not know,
- skip events whose `kind` they do not know,
- treat an unknown `lastError.reason` like `startFailed`.

Optional members are omitted when they have no value; they are never `null`.
[`openrpc.json`](openrpc.json) describes exactly one version: its objects are
closed, so that mocks and contract tests detect stray members.

## 5. Lifecycle

```text
            start                    started
 stopped ─────────► starting ───────────────────► running
    ▲                  │ start failed                │
    │                  ▼                             │ stop, input end
    └───────── stopped (stopReason "error",          ▼
                        lastError)      stopped ◄── stopping
```

| State | `start` | `stop` | `restart` |
|---|---|---|---|
| `stopped` | → `starting` | success, no effect | same as `start` |
| `starting`, `stopping` | error `busy` | error `busy` | error `busy` |
| `running` | error `alreadyRunning` | → `stopping` | → `stopping` → `stopped` → `starting` |

- **Responses come when the request takes effect,** not when the transition
  ends. A start can take seconds (receiver commands, `misc-startcmd`, opening
  serial ports). The result `seq` is the last event published when the
  request took effect, including the transition event it published; the
  outcome follows as status events with a greater `seq`: `running`, or
  `stopped` with `stopReason: "error"` and `lastError`.
- **`busy`:** every lifecycle request during `starting` or `stopping`, or
  while a restart is in progress, is refused at once, with the current state
  in `data.state`. Wait for the next status event and retry. *Why:* the answer is immediate, and with several
  clients it is clear whose request took effect.
- **`alreadyRunning`:** `start` while running is an error. *Why:* a client
  that changed the configuration and meant `restart` would otherwise believe
  the change had been applied.
- **`stop` while stopped succeeds** without effect. *Why:* "be stopped" is
  already true, and a GUI can stop on exit without checking first.
- **`restart`** publishes `stopping`, `stopped`, `starting`, then `running`
  or `stopped`. It keeps other lifecycle requests out until it has finished:
  they get `busy` even while the intermediate `stopped` is shown, so the
  restart stops and starts again with the same configuration as one
  operation.
- **`shutdown`** is accepted in every state. During a transition it is
  recorded and carried out when the transition ends; a transition is never
  interrupted. After it is accepted, `start`, `stop` and `restart` fail with
  `shuttingDown`; reading methods work until the connection closes. If a
  transition never ends (for example a receiver command hangs), the parent's
  last resort is to terminate the process.
- **Input end:** when every configured input (inputs of type `none` aside) is
  a file and all have been read to the end, the server stops by itself with
  `stopReason: "inputEnd"`. Any input that has no end — serial, network,
  stdin, ftp/http — keeps it running. If all inputs are `none`, it never stops
  by itself.

The lifecycle object (`status.lifecycle` in `getStatus` and `sync`, `data` of
status events):

| Member | Meaning |
|---|---|
| `state` | `stopped`, `starting`, `running` or `stopping` |
| `stopReason` | In `stopping` and `stopped`, except before the first start: `request` (a stop, restart or shutdown request from any interface, or a signal), `inputEnd`, or `error` (the start failed) |
| `lastError` | The most recent start failure, `{reason, message}`: present in `stopped` with `stopReason: "error"` and in a following `starting`, until a start succeeds. `reason` is a stable name from an open set (0.1: `startFailed`); `message` is English text for display, not part of the contract |

## 6. Status

`getStatus` returns `{seq, status}`. `seq` is the last event `seq` published
when the content was determined; `status` is:

| Member | Present | Meaning |
|---|---|---|
| `lifecycle` | always | §5 |
| `mode` | while running | Positioning mode: the configuration key `positioning.mode` in its TOML form, e.g. `kinematic`, `ppp-kine` |
| `solution` | while running, after the first solution of the run | The last published solution event (§8); never newer than the events |

Outside `running`, `status` holds only `lifecycle`. *Why:* a start rebuilds the
server's internal state, so reading it while starting is unsafe; after a
stop, the final solutions have already been published as events before
`stopped`, and returning old values would require telling the client which
run they belong to.

*Stale responses:* a response built just before a stop can arrive after the
`stopped` event. A client that has already applied a status event with a
greater `seq` than the response's `seq` discards the response.

## 7. Events

Each event is `{seq, kind, data}`; `kind` tells how to read `data`:

| `kind` | `data` | Published when |
|---|---|---|
| `status` | the lifecycle (§5) | the lifecycle state, `stopReason` or `lastError` changes; the event carries the whole new lifecycle |
| `solution` | a solution (§8) | the server produces a solution |
| `logMessage` | a log message (§9) | the server emits a log message |

```json
{"seq": 5, "kind": "status", "data": {"state": "running"}}
```

### Sequence numbers

- One sequence for all kinds. It increases in publish order for the life of
  the process and is not reset by `start`. `0` means that nothing has been
  published yet. Values stay below 2^53, so JavaScript numbers hold them
  exactly.
- Publish order is not time order: a solution can carry an older `time` than
  an event before it. Sort by the solution's `time` when time order matters.
- All solutions of a run are published before its `stopped` event, and no
  solution of a new run before its `running` event.

### Initial synchronization and polling

`sync` returns the state together with a boundary `seq`: the state reflects
all events up to and including `seq` and none after it. Fetching
`getEvents(after = seq)` then yields exactly the later events — nothing is
missed and nothing is applied twice. `sync` returns `{seq, status}`; later
versions add the other state objects (`streams`, `satellites`) next to
`status`. In 0.1, where `status` is the only state object, it has the same
shape as `getStatus`.

`getEvents(after, limit)` returns events with a `seq` greater than `after`, in
ascending order:

| Member | Meaning |
|---|---|
| `events` | The events (at most `limit`, and at most the server's own maximum) |
| `next` | Cursor for the next call: the `seq` of the last returned event, or `after` when none was returned |
| `more` | `true` when more events are ready now; call again without waiting |
| `lost` | `true` when some events after `after` are no longer held; `events` is then empty |
| `oldestSeq` | With `lost`: the oldest `seq` still held |

After `lost`, call `sync` again. The status is recovered; the missing
solutions and log messages are not. `after` greater than the last published
`seq` is an invalid parameter.

### The event buffer

- One buffer holds all kinds. Its capacity is not fixed yet; it will be set
  from measurements. A client polling every 0.1–1 s does not lose events in
  normal operation. Events are lost when a client stops polling for long
  (a frozen GUI, a sleeping computer) or when level-2 log messages flood the
  buffer.
- The buffer is cleared when a run starts. Events of a run, including the
  final solutions, stay available until the next start.
- A cursor is valid only on its connection. After reconnecting, start again
  with `sync`.
- To record solutions without gaps, use the server's output streams (solution
  file, TCP output), not RPC.

## 8. Solution events

The solution events are the solutions the server produces: one per epoch
with a solution, and while there is none, a solution with status `none` at
most once per second, with these exceptions:

- a `none` solution is not published when the previous solution event of
  the run was a `none` with the same time;
- no `none` solution is published before the first epoch of a run has a
  time (the server reports "no solution" before it has read any
  observation).

The output filters of the solution file (maximum
standard deviation, NMEA interval) do not apply, so the solution file can
contain fewer solutions.

| Member | Meaning |
|---|---|
| `time` | `{week, tow}`: GPS week (not truncated to 10 bits) and time of week in GPST (s), with nanosecond resolution |
| `leapSeconds` | GPST − UTC (s) used by the server for this epoch, from its leap-second table |
| `status` | `fix`, `float`, `sbas`, `dgps`, `single`, `ppp`, `dr` (dead reckoning) or `none` |
| `position` | `ecef` `{x, y, z}` (m) and `llh` `{lat, lon, height}` (deg, deg, m; WGS84, ellipsoidal height) |
| `covariance` | Position covariance (m²): `ecef` `{xx, yy, zz, xy, yz, zx}` and `enu` `{ee, nn, uu, en, nu, ue}` at the solution position |
| `geoid` | `{model, separation}`: the geoid model in use (`output.geoid_model`: `internal`, `egm96`, `egm08_2.5`, `egm08_1`, `gsi2000`) and the geoid height above the ellipsoid (m). `separation` is absent when the model gives no value |
| `validSatellites` | Number of valid satellites in the solution |
| `age` | Age of differential corrections (s), as the engine reports it (the solution file shows the same value). A mode that does not compute it leaves 0, so 0 is not necessarily an age. Its exact meaning depends on the positioning mode |
| `ratio` | Ambiguity validation ratio (second-best to best candidate). 0 is also used when no ambiguity resolution was done for the epoch |
| `base` | Base station position (`ecef`, `llh`) used for the solution. Absent when the mode uses no base station or its position is not known yet |

A `none` solution has only `time`, `leapSeconds` and `status`; every other
solution has all members except `base`, which depends on the mode. *Why:* the
server can keep the coordinates of an earlier epoch while it has no solution;
leaving them out keeps a client from showing them as valid.

Positions are given in both ECEF and latitude/longitude/height so that
clients need no ellipsoid conversions. Coordinates are written with at least
0.1 mm resolution.

**Height.** `llh.height` is always the ellipsoidal height, whatever the
`output.height_type` setting. The orthometric height is
`llh.height - geoid.separation`. A client can show either without changing
the configuration. When `geoid.separation` is absent, the client still knows
which model gave no value. A geoid model result of exactly 0 means "no
value", so `separation` is omitted then.

**Limitation: solutions dropped before the event buffer.** The server keeps
solutions in a fixed-size internal buffer until the RPC layer collects them,
which it does periodically. If collection stalls until that buffer is full
(for example while positioning holds the server lock for long), the server
drops the solutions that follow, and protocol 0.1 does not report it. This
needs a stall of the buffer's length in epochs (256 solutions: 25.6 s at
10 Hz) and is not expected in normal operation. It is separate from `lost`
in `getEvents`, which concerns the event buffer.

**Base position.** The server attaches the base position current when it
collects the solution. With a moving base, several epochs collected together
share the latest base position.

### Time conversion

The server does not send UTC; it sends GPST and the offset. Clients need no
leap-second table:

| Display | Computation |
|---|---|
| GPST, calendar | 1980-01-06 00:00:00 + `week` × 604800 s + `tow` |
| UTC | GPST calendar − `leapSeconds` s |
| Local time (e.g. JST) | UTC + zone offset |
| GPST, week and seconds | `week`, `tow` |

*Why:* sending both GPST and a UTC value with every solution repeats the same
instant twice, while the offset is all a client needs. During an inserted
leap second a client shows the same UTC second twice; the server's own UTC
time cannot represent `23:59:60` either.

## 9. Log messages

`logMessage` is `{level, text}`.

- **Level 1** (default): errors from all positioning engines and warnings
  from the RTK and VRS engines. **Level 2** adds detailed diagnostics; it
  can be selected while running with a configuration key, which is defined
  together with the configuration methods (not in 0.1). Level 2 can be
  frequent. The `level` of a message is the lowest selected level at which it
  is delivered.
- `text` is English text for display. It is not part of the contract: show
  it, do not parse it. Long messages are truncated. Secrets (passwords in
  stream paths, the token) never appear.
- Log messages can arrive after `stopped`, for example from a download that
  is still finishing.

These are not the same as the configuration's log streams
(`[streams.log.*]`, copies of raw input data), the solution status file
(`-r`), the trace file (`-t`) or the telnet `log` command. For diagnostics
beyond level 2, use the trace file.

## 10. Errors

Errors use the JSON-RPC error object. Branch on `data.reason`, not on
`message`, which is English text for display.

| `code` | `data.reason` | Meaning |
|---|---|---|
| -32700 | — | Parse error |
| -32600 | — | Invalid request (also: batch requests) |
| -32601 | — | Method not found |
| -32602 | `invalidParams` | Invalid parameters |
| -32603 | — | Internal error |
| 1001 | `busy` | A lifecycle request during `starting` or `stopping`, or during a restart; `data.state` holds the state |
| 1002 | `alreadyRunning` | `start` while running |
| 1003 | `shuttingDown` | A lifecycle request after `shutdown` was accepted |

MRTKLIB's own codes start at 1001, outside the range that JSON-RPC reserves
(-32768 to -32000). Start failures are not errors of the `start` request;
they appear as `lastError` (§5).

## 11. Not yet provided

Planned for later versions:

- configuration: `getConfig`, `setConfig`, `loadConfig`, `saveConfig` and the
  option schema, including the RPC settings in the TOML configuration and the
  token given in the configuration;
- `streams` (per-stream state, byte counts and messages, as state and events)
  and `satellites`;
- further status members: satellite counts, DOP, CPU time per cycle, missed
  observations, input message counts, uptime, estimated states, baseline
  length;
- velocity in solutions;
- the geoid separation for file-based geoid models (until then
  `geoid.separation` is present only with the `internal` model);
- `lastError.reason` values more specific than `startFailed`.

Possible later additions, without changing what is defined here: push
notifications, filtering events by kind, resuming a cursor after reconnecting,
choosing the coordinate representations per request, timestamps on log
messages (the computer's clock in UTC), and a report of solutions dropped
before the event buffer.

## 12. Relation to the telnet console

The telnet console stays for compatibility and is frozen: no new commands and
no format changes. Both interfaces go through the same command layer inside
`mrtk run`, with these differences:

- After a stop, the telnet console still shows the last values; RPC returns
  only the lifecycle (§6).
- During `starting` and `stopping`, the telnet console keeps its display
  format with empty values.
