/* SPDX-License-Identifier: MIT */
/* Just enough SIP (RFC 3261) to classify and route an emergency INVITE.
 * The parser works on a caller-owned buffer and returns views into it. */
#ifndef SIP_H
#define SIP_H

#include <stddef.h>

struct sv { const char *p; size_t n; }; /* string view, not NUL-terminated */

enum ecall_service { SVC_NONE = 0, SVC_SOS, SVC_POLICE, SVC_FIRE, SVC_AMBULANCE };

struct invite {
	struct sv method, ruri, call_id, from, pani; /* pani = P-Access-Network-Info */
	const char *headers_end;                    /* first byte of the body */
	size_t len;
};

/* Parses the start line and headers. Returns 0, or -1 if the message is not
 * a well-formed request (no CRLF CRLF, bad start line, oversized header). */
int sip_parse(const char *msg, size_t len, struct invite *out);

/* urn:service:sos[.police|.fire|.ambulance] per RFC 5031 (case-insensitive).
 * Anything else, including other urn:service values, is SVC_NONE. */
enum ecall_service sip_emergency_service(struct sv ruri);
const char *svc_name(enum ecall_service s);

/* Extracts utran-cell-id-3gpp from P-Access-Network-Info (3GPP TS 24.229).
 * Returns 0 and copies the value (MCC+MNC+TAC+ECI, hex/decimal digits only)
 * into out, or -1 if absent or not plausible. */
int sip_cell_id(struct sv pani, char *out, size_t cap);

/* Writes the routed INVITE: Request-URI replaced by psap, plus
 * Geolocation (by reference) and Geolocation-Routing headers. Returns the
 * length written, or 0 if cap is too small. */
size_t sip_route(const struct invite *in, const char *msg, const char *psap,
		 const char *loc_ref, char *out, size_t cap);

#endif
