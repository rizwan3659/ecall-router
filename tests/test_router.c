/* SPDX-License-Identifier: MIT */
#include "../src/route.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", \
	__FILE__, __LINE__, #c); fails++; } } while (0)

static struct tables T;

static size_t invite(char *buf, size_t cap, const char *ruri, const char *cell,
		     const char *callid)
{
	int n = snprintf(buf, cap,
		"INVITE %s SIP/2.0\r\n"
		"Via: SIP/2.0/UDP 10.0.0.1;branch=z9hG4bK1\r\n"
		"From: <sip:+919800000001@ims.example>;tag=1\r\n"
		"To: <%s>\r\n"
		"Call-ID: %s\r\n"
		"CSeq: 1 INVITE\r\n"
		"%s%s%s"
		"Content-Length: 4\r\n\r\nv=0\n",
		ruri, ruri, callid,
		cell ? "P-Access-Network-Info: 3GPP-E-UTRAN-FDD; utran-cell-id-3gpp=" : "",
		cell ? cell : "", cell ? "\r\n" : "");
	return (size_t)n;
}

static void parser(void)
{
	char m[1024], cell[24];
	struct invite inv;
	size_t n = invite(m, sizeof(m), "urn:service:sos", "4049801a010000001", "c1");
	CHECK(sip_parse(m, n, &inv) == 0);
	CHECK(inv.call_id.n == 2 && memcmp(inv.call_id.p, "c1", 2) == 0);
	CHECK(sip_cell_id(inv.pani, cell, sizeof(cell)) == 0 &&
	      strcmp(cell, "4049801a010000001") == 0);

	struct sv s;
#define SVC(str) (s = (struct sv){ str, sizeof(str) - 1 }, sip_emergency_service(s))
	CHECK(SVC("urn:service:sos") == SVC_SOS);
	CHECK(SVC("URN:Service:SOS.Fire") == SVC_FIRE);
	CHECK(SVC("<urn:service:sos.police>") == SVC_POLICE);
	CHECK(SVC("urn:service:sos.ambulance;x=1") == SVC_AMBULANCE);
	CHECK(SVC("urn:service:sos.animal-control") == SVC_SOS); /* unknown sub-service */
	CHECK(SVC("urn:service:counseling") == SVC_NONE);
	CHECK(SVC("sip:112@ims.example") == SVC_NONE);
	CHECK(SVC("<urn:service:sos") == SVC_NONE);

	/* malformed */
	CHECK(sip_parse("INVITE x SIP/2.0\r\n", 18, &inv) == -1);          /* no blank line */
	CHECK(sip_parse("INVITE x SIP/3.0\r\nCall-ID: a\r\n\r\n", 32, &inv) == -1);
	CHECK(sip_parse("INVITE x SIP/2.0\r\nFrom: a\r\n\r\n", 29, &inv) == -1); /* no Call-ID */
	CHECK(sip_parse("INVITE x SIP/2.0\nCall-ID: a\n\n", 29, &inv) == -1); /* bare LF */

	struct sv pani = { "3GPP-E-UTRAN-FDD; utran-cell-id-3gpp=12", 39 };
	CHECK(sip_cell_id(pani, cell, sizeof(cell)) == -1); /* too short */
}

static void tables(void)
{
	char tac[5];
	CHECK(tac_of_cell("4049801a010000001", tac) == 0 && strcmp(tac, "1a01") == 0); /* 3-digit MNC */
	CHECK(tac_of_cell("40498" "2B01" "0000001", tac) == 0 && strcmp(tac, "2b01") == 0);
	CHECK(tac_of_cell("123", tac) == -1);
	CHECK(strcmp(psap_lookup(&T, "1a01", SVC_POLICE), "sip:police-100@delhi.psap.example") == 0);
	CHECK(strcmp(psap_lookup(&T, "1a02", SVC_FIRE), "sip:psap-112@gurugram.psap.example") == 0);
	CHECK(strcmp(psap_lookup(&T, "ffff", SVC_FIRE), "sip:psap-112@national.psap.example") == 0);
	CHECK(strcmp(psap_lookup(&T, NULL, SVC_SOS), "sip:psap-112@national.psap.example") == 0);
}

/* Both strategies must pick the same PSAP for every known cell/service. */
static void same_decisions(void)
{
	int lp, rp;
	struct server *l = lrf_start(&T, 1, &lp), *r = rdf_start(&T, 1, &rp);
	struct router a, b;
	router_open(&a, &T, lp, rp, 500);
	router_open(&b, &T, lp, rp, 500);

	char m[1024], out[2048], pa[URI_MAX], pb[URI_MAX], cell[24], id[32];
	const char *svcs[] = { "urn:service:sos", "urn:service:sos.police",
			       "urn:service:sos.fire", "urn:service:sos.ambulance" };
	int cases = 0;
	for (size_t c = 0; c < T.ncell; c++)
		for (int s = 0; s < 4; s++) {
			snprintf(cell, sizeof(cell), "40498%s0000001", T.cell[c].tac);
			snprintf(id, sizeof(id), "call-%zu-%d", c, s);
			size_t n = invite(m, sizeof(m), svcs[s], cell, id);
			CHECK(route_serial(&a, m, n, out, sizeof(out), pa) == 1);
			CHECK(route_fast(&b, m, n, out, sizeof(out), pb) == 1);
			CHECK(strcmp(pa, pb) == 0);
			cases++;
		}
	CHECK(a.fallbacks == 0 && b.fallbacks == 0);

	/* routed output: PSAP as Request-URI, location by reference */
	CHECK(strncmp(out, "INVITE sip:", 11) == 0);
	CHECK(strstr(out, "Geolocation: <https://lrf.example.org/loc/call-") != NULL);
	CHECK(strstr(out, "Geolocation-Routing: yes\r\n\r\nv=0\n") != NULL);

	/* not an emergency: left alone */
	size_t n = invite(m, sizeof(m), "sip:bob@ims.example", "40498" "1a01" "0000001", "x");
	CHECK(route_fast(&b, m, n, out, sizeof(out), pb) == 0);

	/* unknown or missing cell: default PSAP, and the call is still routed */
	n = invite(m, sizeof(m), "urn:service:sos", NULL, "nocell");
	CHECK(route_fast(&b, m, n, out, sizeof(out), pb) == 1);
	CHECK(strcmp(pb, "sip:psap-112@national.psap.example") == 0);
	CHECK(route_serial(&a, m, n, out, sizeof(out), pa) == 1 && strcmp(pa, pb) == 0);

	/* a Call-ID that would break the LRF text protocol is rejected */
	n = invite(m, sizeof(m), "urn:service:sos", NULL, "a b");
	CHECK(route_fast(&b, m, n, out, sizeof(out), pb) == -1);

	printf("same PSAP from both strategies in %d cases\n", cases);
	router_close(&a);
	router_close(&b);
	server_stop(l);
	server_stop(r);
}

/* If the LRF is too slow, the serial path must fall back, not hang. */
static void lrf_timeout(void)
{
	int lp, rp;
	struct server *l = lrf_start(&T, 300, &lp), *r = rdf_start(&T, 1, &rp);
	struct router a;
	router_open(&a, &T, lp, rp, 50);
	char m[1024], out[2048], p[URI_MAX];
	size_t n = invite(m, sizeof(m), "urn:service:sos", "40498" "2b01" "0000001", "slow");
	CHECK(route_serial(&a, m, n, out, sizeof(out), p) == 1);
	CHECK(a.fallbacks == 1 && strcmp(p, "sip:psap-112@national.psap.example") == 0);
	router_close(&a);
	server_stop(l);
	server_stop(r);
}

int main(void)
{
	if (tables_load(&T, "data/cells.csv", "data/psap.csv") < 0) {
		fprintf(stderr, "cannot load data/*.csv (run from the repo root)\n");
		return 1;
	}
	parser();
	tables();
	same_decisions();
	lrf_timeout();
	if (fails) {
		fprintf(stderr, "%d check(s) failed\n", fails);
		return 1;
	}
	printf("all tests passed\n");
	return 0;
}
