/* SPDX-License-Identifier: MIT */
#include "sip.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define MAX_LINE 2048

static int ieq(struct sv a, const char *b)
{
	size_t n = strlen(b);
	if (a.n != n)
		return 0;
	for (size_t i = 0; i < n; i++)
		if (tolower((unsigned char)a.p[i]) != tolower((unsigned char)b[i]))
			return 0;
	return 1;
}

static struct sv trim(struct sv s)
{
	while (s.n && (*s.p == ' ' || *s.p == '\t')) {
		s.p++;
		s.n--;
	}
	while (s.n && (s.p[s.n - 1] == ' ' || s.p[s.n - 1] == '\t'))
		s.n--;
	return s;
}

int sip_parse(const char *msg, size_t len, struct invite *out)
{
	memset(out, 0, sizeof(*out));
	const char *end = msg + len, *p = msg;

	/* start line: METHOD SP Request-URI SP SIP/2.0 CRLF */
	const char *eol = memchr(p, '\n', (size_t)(end - p));
	if (!eol || eol == p || eol[-1] != '\r' || eol - p > MAX_LINE)
		return -1;
	const char *sp1 = memchr(p, ' ', (size_t)(eol - p));
	const char *sp2 = sp1 ? memchr(sp1 + 1, ' ', (size_t)(eol - sp1 - 1)) : NULL;
	if (!sp1 || !sp2 || sp1 == p || sp2 == sp1 + 1)
		return -1;
	struct sv ver = { sp2 + 1, (size_t)(eol - 1 - (sp2 + 1)) };
	if (!ieq(ver, "SIP/2.0"))
		return -1;
	out->method = (struct sv){ p, (size_t)(sp1 - p) };
	out->ruri = (struct sv){ sp1 + 1, (size_t)(sp2 - sp1 - 1) };
	p = eol + 1;

	/* headers until an empty line */
	for (;;) {
		eol = memchr(p, '\n', (size_t)(end - p));
		if (!eol || eol[-1] != '\r' || eol - p > MAX_LINE)
			return -1;
		if (eol == p + 1) { /* CRLF alone */
			out->headers_end = eol + 1;
			break;
		}
		const char *colon = memchr(p, ':', (size_t)(eol - p));
		if (!colon)
			return -1;
		struct sv name = trim((struct sv){ p, (size_t)(colon - p) });
		struct sv val = trim((struct sv){ colon + 1, (size_t)(eol - 1 - (colon + 1)) });
		if (ieq(name, "Call-ID") || ieq(name, "i"))
			out->call_id = val;
		else if (ieq(name, "From") || ieq(name, "f"))
			out->from = val;
		else if (ieq(name, "P-Access-Network-Info") && !out->pani.p)
			out->pani = val;
		p = eol + 1;
	}
	out->len = len;
	return out->call_id.n ? 0 : -1; /* Call-ID is mandatory */
}

enum ecall_service sip_emergency_service(struct sv u)
{
	/* strip angle brackets and parameters: <urn:service:sos>;x=y */
	if (u.n && u.p[0] == '<') {
		const char *gt = memchr(u.p, '>', u.n);
		if (!gt)
			return SVC_NONE;
		u = (struct sv){ u.p + 1, (size_t)(gt - u.p - 1) };
	}
	const char *semi = memchr(u.p, ';', u.n);
	if (semi)
		u.n = (size_t)(semi - u.p);

	if (ieq(u, "urn:service:sos"))
		return SVC_SOS;
	if (ieq(u, "urn:service:sos.police"))
		return SVC_POLICE;
	if (ieq(u, "urn:service:sos.fire"))
		return SVC_FIRE;
	if (ieq(u, "urn:service:sos.ambulance"))
		return SVC_AMBULANCE;
	/* an unknown sos sub-service still is an emergency (RFC 5031 s.4.2) */
	if (u.n > 16 && ieq((struct sv){ u.p, 16 }, "urn:service:sos."))
		return SVC_SOS;
	return SVC_NONE;
}

const char *svc_name(enum ecall_service s)
{
	static const char *n[] = { "none", "sos", "police", "fire", "ambulance" };
	return s <= SVC_AMBULANCE ? n[s] : "none";
}

int sip_cell_id(struct sv pani, char *out, size_t cap)
{
	static const char key[] = "utran-cell-id-3gpp=";
	const size_t klen = sizeof(key) - 1;

	for (size_t i = 0; i + klen <= pani.n; i++) {
		if (strncasecmp(pani.p + i, key, klen) != 0)
			continue;
		const char *v = pani.p + i + klen, *e = pani.p + pani.n;
		if (v < e && *v == '"')
			v++;
		size_t n = 0;
		while (v + n < e && isxdigit((unsigned char)v[n]))
			n++;
		/* MCC(3) + MNC(2-3) + TAC(4) + ECI(7) = 16..17 hex digits */
		if (n < 16 || n > 17 || n >= cap)
			return -1;
		memcpy(out, v, n);
		out[n] = 0;
		return 0;
	}
	return -1;
}

size_t sip_route(const struct invite *in, const char *msg, const char *psap,
		 const char *loc_ref, char *out, size_t cap)
{
	const char *hdr_start = (const char *)memchr(msg, '\n', in->len) + 1;
	size_t hdr_len = (size_t)(in->headers_end - 2 - hdr_start); /* up to blank line */
	size_t body_len = in->len - (size_t)(in->headers_end - msg);
	int n = snprintf(out, cap,
			 "%.*s %s SIP/2.0\r\n%.*sGeolocation: <%s>\r\n"
			 "Geolocation-Routing: yes\r\n\r\n",
			 (int)in->method.n, in->method.p, psap, (int)hdr_len, hdr_start,
			 loc_ref);
	if (n < 0 || (size_t)n + body_len >= cap)
		return 0;
	memcpy(out + n, in->headers_end, body_len);
	out[(size_t)n + body_len] = 0;
	return (size_t)n + body_len;
}
