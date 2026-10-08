<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Native MCP tools

A session owns a native HTTP client. Its catalog joins the existing Responses
function catalog and its calls use the existing durable tool-start/tool-result
lifecycle. The MCP owner contains transport, discovery, credentials and admission;
provider code and the session reducer remain independent of remote schemas.

Streamable HTTP supports the stateless 2026-07-28 protocol and initialized 2025
servers. HTTP uses the existing libcurl trust configuration and SSE parser. Each
operation has a deadline from its first network operation and pumps the session's
normal input/cancellation path. URL validation requires HTTPS outside loopback;
redirects never forward tokens or silently change the configured resource.
Local stdio servers are outside this HTTP client design.

Catalogs are immutable during a turn. Discovery refreshes between turns, on expiry
or after a list-change notification; explicit reload stages a replacement. Exact
input schemas become function parameters. Full server declarations, including
annotations and output schemas, remain available through inspection and model
metadata. Names derive from configured server identity and exact remote tool
identity, never the server's self-reported name. A single event-loop-owned
libcurl subscription per active server receives catalog changes. It mutates only
the dirty flag; the next turn refreshes the immutable contract. Lost streams
schedule refresh instead of creating a retry loop. Shutdown closes all owned
streams before bounded legacy-session deletion.

Local configuration owns admission. A remote readOnlyHint is descriptive only.
An unlisted call returns an inspectable pending request; an operator approval
binds the configured endpoint, catalog declaration and canonical arguments.
Approval is single use. No network failure retries tools/call automatically.
Transport loss after dispatch reports outcome unknown; a successful MCP result
reports server acceptance and preserves the server's data without inferring that
an external object was delivered or read back.

OAuth credentials use a separate private directory under the application store,
bound to server name, resource and configured client identity. Discovery validates
protected-resource and issuer metadata. Authorization-code flow uses PKCE S256,
random state and an exact loopback callback; a terminal URL/callback flow needs no
browser launch. Refresh is serialized with other clients sharing that credential.
Runtime tokens join the ordinary tool/provider redaction set, including older
admitted values across explicit config reload. Credentials remain in memory and
private OAuth storage; the frozen catalog and native journal contain no tokens.
OAuth codes and tokens never enter a session command, prompt or event journal.

Qualification uses isolated HTTP/OAuth/SSE fixtures through the production CLI and
model dispatch. The production matrix remains the portability contract. Live
service catalogs and scope grants are separate from these local protocol tests.

MCP schemas, arguments and results permit fractional numbers. Provider document
measurement and wire serialization use sorted JSON with finite real numbers.
Native host canonical records retain their integer-only contract: at Responses
admission an MCP call's arguments are stored as an opaque JSON string inside its
argument object. The shared response-argument projection restores that object for
rules, rendering, MCP dispatch and subsequent provider context. The native graph
codec and checkpoint use their existing bounded string representation. Full MCP
results are already text in the native tool-result envelope. This keeps arbitrary
remote JSON out of host counter/enum interpretation without losing its values.
