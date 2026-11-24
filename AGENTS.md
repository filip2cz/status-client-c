# Architecture guide for agents

This file describes the current implementation and the constraints that matter when changing it. Treat `status.c` as the source of truth; keep this document and `README.md` in sync with behavior changes.

## Purpose and scope

This repository contains a Linux C client for [BotoX/ServerStatus](https://github.com/BotoX/ServerStatus). The process connects to a ServerStatus master over TCP, authenticates with one configured account, samples local machine statistics, and periodically sends line-oriented `update {JSON}` messages. If a connection fails, the client tries again until it receives `SIGINT` or `SIGTERM`.

The implementation is intentionally self-contained in `status.c`. It uses POSIX/Linux interfaces and does not depend on a third-party JSON or networking library. Keep that property in mind: the GitHub workflow cross-compiles the same file for x86, ARM, and Blackfin targets.

## Files and build outputs

* `status.c` contains configuration parsing, TCP protocol handling, metric collection, retry logic, and `main`.
* `Makefile` builds `status-client` from `status.c`. The default flags use C11 and warnings; `CC`, `CFLAGS`, `CPPFLAGS`, `LDFLAGS`, and `TARGET` can be overridden.
* `tests/test_status.c` includes `status.c` into a test translation unit to call its internal `static` functions. Its local DNS wrapper redirects the IPv4/IPv6 probes to loopback, so tests do not need external network access.
* `.github/workflows/main.yml` runs tests first, then starts four independent architecture build jobs on separate runners in parallel: x86, AArch64/ARMv7, ARMv5, and Blackfin. Each build uploads its binaries as an artifact. A `verify-binaries` job downloads the artifacts, restores executable bits, runs `--help` on x86/x64 natively and on AArch64/ARM under `qemu-user` with the matching cross-libc roots, and checks the Blackfin ELF class and machine using `readelf`. GitHub's zipped artifacts do not preserve executable permissions. Current QEMU user-mode emulation does not support Blackfin, so that target receives a format/architecture check rather than an execution smoke test. The final release job waits for verification (and, transitively, tests and all builds), merges the artifacts, and creates a draft release containing `status-linux-x64`, `status-linux-x86`, `status-linux-aarch64`, `status-linux-armv7l`, `status-linux-armv5te`, and `status-linux-blackfin`. ARM build steps explicitly install their `libc6-dev-*-cross` packages because the workflow suppresses apt recommendations. ARMv7 uses the hard-float ABI with the `vfpv3-d16` FPU.
* `README.md` documents setup and the three configuration modes.
* `config.json.example` and `login.txt.example` are templates. Real `config.json` and `login.txt` files are ignored by Git because they contain credentials.

There is no separate library layer or generated source. New dependencies can make one or more cross-compilers fail, so prefer libc/POSIX facilities already used in the program. The legacy Blackfin uClibc toolchain lacks some newer libc APIs, so the source uses its own `read_stream_line`, checks finite doubles using `DBL_MAX`, and reads load average from `/proc/loadavg` instead of relying on `getline`, `isfinite`, or `getloadavg`. Run the unit suite with `make test`; it builds an ignored `tests/test_status` executable. CI runs architecture builds only after tests pass and verifies downloaded binaries before creating a release draft.

The suite calls each production function from the included source file, including the renamed entry point, and covers parsers, validation failures, socket framing (including upstream NUL-padded lines), authentication success and rejection, reset during authentication, connected-socket interface selection, IPv4/IPv6 default-route fallback parsing and interface filtering, metric calculations/reads, connectivity probes, and the update loop. It uses child processes for handshake scenarios and a local alarm to stop the normally continuous sender loop. DNS for the probe hostnames is intercepted at link time and rewritten to loopback; keep these tests hermetic rather than allowing them to contact public hosts.

## Runtime architecture

The high-level control flow is:

1. Parse exactly one configuration mode and validate all values.
2. Install handlers for `SIGINT` and `SIGTERM`; handlers only set the `stopping` flag.
3. Resolve the configured master and try each returned address with a bounded nonblocking TCP connect.
4. Complete the ServerStatus greeting and authentication exchange.
5. Collect and send metrics at the configured interval while the connection remains healthy.
6. Close the socket and retry after three seconds after a connection or metric collection failure.

The top-level state lives in `ProgramConfig` and is passed into the active routines. `stopping` and `quiet` are the only process-wide mutable flags. Do not perform logging, allocation, or socket work inside the signal handler.

### Configuration modes

The modes are mutually exclusive:

1. **JSON:** `status-client --config FILE`. All seven keys are required: `host` (string), `port` (integer-valued number), `username` (string), `password` (string), `interval` (number), `mountpoint` (string), and `quiet` (boolean). Config mode cannot be mixed with other settings or positional arguments.
2. **Explicit options:** `--host HOST --port PORT --username USER --password PASS`, with optional `--interval`, `--mountpoint`, and `--quiet`. Host, port, username, and password are mandatory. Command-line passwords can be visible to local process inspection; JSON mode avoids putting them in the argument vector.
3. **Legacy:** `host port`, with credentials read from `login.txt` in the current working directory. `-C FILE` selects a different credentials file. `-i`, `-m`, and `-q` can also be supplied in this mode. This preserves the original deployment invocation so the executable can be replaced without changing its service command.

The default interval is one second, the default mountpoint is `/`, and quiet mode defaults to false in option-based modes. Intervals must be finite values between 0.1 and 3600 seconds; ports must be in the TCP range 1–65535. Usernames cannot contain `:` or line breaks because the wire login uses a colon separator. Passwords cannot contain line breaks.

### JSON configuration parser

The parser is deliberately local to `status.c`; it exists to avoid another library in cross builds. `read_config_json` reads at most 64 KiB. It parses the required flat settings, decodes JSON string escapes (including UTF-16 surrogate pairs), rejects duplicate known keys and malformed JSON, and skips unknown values so future unrelated keys do not prevent startup. Unknown values have a recursion-depth guard around 32 nested containers. Do not silently relax required fields or accept a different type for a known setting without updating the docs and sample file.

The parser helpers (`json_space`, `json_string`, `json_number`, `json_boolean`, `json_skip_value`, and related helpers) share a `JsonParser` cursor. Bounds checks are important: output values are copied into fixed-size arrays in `ProgramConfig` and `Credentials`.

### TCP protocol and reconnect behavior

`connect_to_server` uses `getaddrinfo(AF_UNSPEC)` and tries the resolved IPv4/IPv6 addresses. `connect_one` performs a nonblocking connect and polls for completion for up to ten seconds per address; name resolution itself is outside that timeout. The line reader has a 4096-byte buffer, removes ASCII control bytes from completed protocol lines (including the NUL padding appended by upstream ServerStatus), and waits at most 30 seconds for socket input. `wait_fd` prioritizes requested readiness over a simultaneous hangup so buffered data can be drained before the peer close is handled. Sending handles partial writes and suppresses `SIGPIPE` where `MSG_NOSIGNAL` is available.

The protocol exchange is:

1. Read a line containing `Authentication required`; ignore upstream NUL padding after line endings.
2. Send `username:password` followed by `\n`.
3. Read a line containing `Authentication successful`.
4. Read the server's `IPv4` or `IPv6` announcement.
5. Send periodic lines beginning with `update `, followed by a JSON object and newline.

The connection to the master is plain TCP; this implementation does not add TLS or encrypt credentials in transit. Do not describe it as a secure transport.

The family announced by the server tells the client which *other* address family to probe: an `IPv4` server announcement causes an IPv6 probe and an `online6` field; an `IPv6` announcement causes an IPv4 probe and an `online4` field. Probes use `ipv6.google.com:80` or `ipv4.google.com:80`, with a two-second connect timeout, and repeat every ten seconds. This probe is part of the reported online status, not the connection to the ServerStatus master.

`authenticate` owns the handshake state and skips up to eight empty protocol lines between nonempty messages, matching the upstream parser's treatment of CR/LF separators. `read_line` removes nonprinting ASCII control bytes because the upstream sender pads each line with NUL bytes. Handshake transport failures identify the phase that failed; diagnostics never include credentials. `send_updates` owns the connected reporting loop. Errors return to `main`, which closes the socket and waits three seconds before reconnecting. Avoid adding unbounded waits to either path.

## Metric collection and units

`collect_metrics` combines the metric readers into a `Metrics` snapshot. The wire field names and units must remain compatible with ServerStatus:

| Wire fields | Source and calculation | Unit |
| --- | --- | --- |
| `uptime` | `sysinfo()` uptime | seconds |
| `load` | first value from `/proc/loadavg` | load average |
| `memory_total`, `memory_used` | `/proc/meminfo`; uses `MemAvailable`, or falls back to `MemFree + Buffers + Cached` | KiB |
| `swap_total`, `swap_used` | `SwapTotal` and `SwapFree` from `/proc/meminfo` | KiB |
| `hdd_total`, `hdd_used` | `statvfs()` for configured mountpoint, based on filesystem block counts | MiB |
| `cpu` | Deltas from aggregate CPU counters in `/proc/stat`; guest counters are already included in user/nice and are not added again | percent |
| `network_rx`, `network_tx` | Counter deltas from `/proc/net/dev`, divided by elapsed monotonic time; uses the interface matching the active ServerStatus socket's local address, falling back to interfaces on the lowest-metric IPv4/IPv6 default routes and then to all interfaces except `lo` and `tun*` | bytes/second |
| `online4` or `online6` | Periodic TCP reachability probe to the other IP family | boolean |

CPU and network rates need a previous snapshot, so the first update after a connection reports zero for these rates. Missing or unreadable CPU/network counters mark those counter snapshots invalid and produce zero rates; they do not fail `collect_metrics`. `connected_socket_interface` matches `getsockname()` output to interface addresses using Linux UAPI `SIOCGIFCONF` via `<linux/if.h>` for IPv4 and `/proc/net/if_inet6` for IPv6, so `send_updates` counts the interface actually selected for the ServerStatus TCP connection. Include `<sys/socket.h>` before `<linux/if.h>`: the older Blackfin uClibc kernel header uses `struct sockaddr` in `ifreq` but does not provide its definition itself. This avoids requiring `getifaddrs`, which may be absent from older uClibc builds, and avoids relying on `net/if.h` declarations missing from the Blackfin toolchain. If direct address mapping fails, route helpers select the best-metric default-route interfaces for both address families, deduplicating a shared interface; if no default route is available, the reader falls back to all interfaces except `lo` and `tun*`. `send_updates` measures the interval after any connectivity probe completes so time spent probing is included in the rate denominator. In contrast, failures to read required uptime, load, memory, or disk metrics fail collection and end the current reporting loop. `monotonic_seconds` is used for elapsed time; do not calculate rates with wall-clock time. Memory and disk fields are integers formatted as JSON numbers. `send_updates` builds the JSON line with `snprintf`; if adding fields, keep its bounds check and JSON syntax intact.

## Function map

`status.c` is organized in functional sections rather than separate translation units:

* **Process utilities and config:** `handle_signal`, `log_message`, `usage`, `trim`, `read_stream_line`, `is_finite_number`, `load_credentials`, JSON parser helpers, `read_config_json`.
* **Socket primitives:** `wait_fd`, `connect_one`, `connect_to_server`, `send_all`, `send_line`, `read_line`, `read_protocol_line`.
* **Server handshake:** `report_handshake_error`, `authenticate`.
* **Metric readers/calculations:** `read_cpu_counters`, `connected_ipv4_interface`, `connected_ipv6_interface`, `connected_socket_interface`, IPv4/IPv6 route selection helpers, `read_network_counters_from_stream`, `read_network_counters`, `read_memory`, `read_disk`, `read_load_average`, `cpu_percent`, `counter_rate`, `collect_metrics`.
* **Periodic status loop:** `probe_connect`, `monotonic_seconds`, `sleep_interval`, `send_updates`.
* **Program entry and orchestration:** `main` selects the mode, validates the resulting `ProgramConfig`, and owns reconnect/cleanup.

Fallible file, configuration, and socket operations generally return `0` on success and `-1` on failure, often leaving `errno` available for diagnostics. CPU and network readers return structs with a `valid` flag; `probe_connect` returns 1 for reachable and 0 for unreachable. Probe failure is represented as offline because it is a reported metric, while failure to collect required local metrics ends the current reporting connection and triggers a reconnect.

## Change guidance for agents

* After **every change** to the project, update this `AGENTS.md` in the same work session so it describes the resulting architecture and current constraints. This applies to source, build files, workflows, configuration, documentation, and changes to this file itself. Do not leave the guide stale after a code change.
* **Do not use Git.** Agents must not run Git commands or otherwise inspect or manipulate Git state. This forbids read-only commands such as `git status`, `git diff`, and `git log`, as well as staging, commits, branch operations, checkout/switch, reset, rebase, merge, cherry-pick, stash, fetch, pull, push, tags, and worktree operations. Work directly with the project files only.
* Preserve all three configuration modes unless a request explicitly changes compatibility. In particular, do not change the positional legacy invocation or the default `login.txt` lookup silently.
* When adding a configuration field, update `ProgramConfig`, JSON parsing and validation, option parsing if applicable, `README.md`, and `config.json.example`. Keep the JSON configuration complete and reject invalid values before connecting.
* When changing metrics, document the source, units, first-sample behavior, and the exact ServerStatus wire key. Do not rename protocol fields casually.
* When changing the handshake, verify compatibility with BotoX/ServerStatus' newline-delimited protocol and retain bounded socket waits, reconnect handling, and partial-write handling.
* Keep credentials out of logs. The real credential files are ignored by Git; never replace those ignore rules with tracked secrets.
* Keep code compatible with the compilers and libc implementations used by both `Makefile` and `.github/workflows/main.yml`. The workflow uses GNU C99 and an older uClibc for Blackfin while the local Makefile uses C11; avoid libc APIs unavailable in that toolchain unless the Blackfin build is updated too.
* Build with `make` and run `make test` after C changes. Tests are written in `tests/test_status.c`; the target uses GNU linker `--wrap=getaddrinfo` to keep probe tests local.

