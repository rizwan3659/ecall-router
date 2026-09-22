/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "route.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

/* ---------------- tables ---------------- */

static enum ecall_service svc_of(const char *s)
{
	for (int i = SVC_SOS; i <= SVC_AMBULANCE; i++)
		if (strcasecmp(s, svc_name((enum ecall_service)i)) == 0)
			return (enum ecall_service)i;
	return SVC_NONE;
}

int tables_load(struct tables *t, const char *cells_csv, const char *psap_csv)
{
	char line[256];
	FILE *f;

	memset(t, 0, sizeof(*t));
	if (!(f = fopen(cells_csv, "r")))
		return -1;
	while (fgets(line, sizeof(line), f) && t->ncell < MAX_ROWS) {
		struct cell *c = &t->cell[t->ncell];
		if (line[0] == '#' || sscanf(line, "%4[0-9a-fA-F],%lf,%lf", c->tac,
					     &c->lat, &c->lon) != 3)
			continue;
		t->ncell++;
	}
	fclose(f);

	if (!(f = fopen(psap_csv, "r")))
		return -1;
	while (fgets(line, sizeof(line), f) && t->npsap < MAX_ROWS) {
		struct psap *p = &t->psap[t->npsap];
		char svc[16];
		if (line[0] == '#' || sscanf(line, "%4[0-9a-fA-F*],%15[a-z],%95s", p->tac,
					     svc, p->uri) != 3)
			continue;
		if ((p->svc = svc_of(svc)) == SVC_NONE)
			continue;
		t->npsap++;
	}
	fclose(f);
	return t->ncell && t->npsap ? 0 : -1;
}

int tac_of_cell(const char *cell, char tac[5])
{
	size_t n = strlen(cell);
	size_t mnc = n == 16 ? 2 : n == 17 ? 3 : 0;
	if (!mnc)
		return -1;
	memcpy(tac, cell + 3 + mnc, 4);
	tac[4] = 0;
	for (int i = 0; i < 4; i++)
		tac[i] = (char)tolower((unsigned char)tac[i]);
	return 0;
}

static const char *find_psap(const struct tables *t, const char *tac,
			     enum ecall_service s)
{
	for (size_t i = 0; i < t->npsap; i++)
		if (t->psap[i].svc == s && strcmp(t->psap[i].tac, tac) == 0)
			return t->psap[i].uri;
	return NULL;
}

const char *psap_lookup(const struct tables *t, const char *tac, enum ecall_service s)
{
	const char *u = NULL;
	if (tac && !(u = find_psap(t, tac, s)))
		u = find_psap(t, tac, SVC_SOS);
	if (!u)
		u = find_psap(t, "*", s);
	if (!u)
		u = find_psap(t, "*", SVC_SOS);
	return u;
}

int cell_location(const struct tables *t, const char *tac, double *lat, double *lon)
{
	for (size_t i = 0; i < t->ncell; i++)
		if (strcmp(t->cell[i].tac, tac) == 0) {
			*lat = t->cell[i].lat;
			*lon = t->cell[i].lon;
			return 0;
		}
	return -1;
}

const char *nearest_tac(const struct tables *t, double lat, double lon)
{
	const char *best = NULL;
	double bd = 1e18;
	for (size_t i = 0; i < t->ncell; i++) {
		double dy = t->cell[i].lat - lat, dx = t->cell[i].lon - lon;
		double d = dx * dx + dy * dy; /* fine at city scale */
		if (d < bd) {
			bd = d;
			best = t->cell[i].tac;
		}
	}
	return best;
}

/* ---------------- LRF / RDF servers ---------------- */

struct server {
	int fd;
	unsigned delay_ms;
	const struct tables *t;
	int is_lrf;
	pthread_t thr;
};

static void sleep_ms(unsigned ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
	while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
		;
}

/* LRF:  "LOC <call-id> <cell-id>"          -> "LOC <call-id> <lat> <lon>" | "LOC <call-id> none"
 * RDF:  "RDF <call-id> <svc> <lat> <lon>"  -> "RDF <call-id> <psap-uri>" */
static void *serve(void *arg)
{
	struct server *s = arg;
	char req[512], rsp[512], id[256], a[64], b[64], c[64];
	struct sockaddr_in from;

	for (;;) {
		socklen_t fl = sizeof(from);
		ssize_t n = recvfrom(s->fd, req, sizeof(req) - 1, 0,
				     (struct sockaddr *)&from, &fl);
		if (n <= 0)
			break;
		req[n] = 0;
		if (strcmp(req, "STOP") == 0)
			break;
		sleep_ms(s->delay_ms); /* the remote lookup */

		int len = -1;
		if (s->is_lrf && sscanf(req, "LOC %255s %63s", id, a) == 2) {
			char tac[5];
			double lat, lon;
			if (tac_of_cell(a, tac) == 0 && cell_location(s->t, tac, &lat, &lon) == 0)
				len = snprintf(rsp, sizeof(rsp), "LOC %s %.4f %.4f", id, lat, lon);
			else
				len = snprintf(rsp, sizeof(rsp), "LOC %s none", id);
		} else if (!s->is_lrf && sscanf(req, "RDF %255s %63s %63s %63s", id, a, b, c) == 4) {
			const char *tac = nearest_tac(s->t, atof(b), atof(c));
			const char *u = psap_lookup(s->t, tac, svc_of(a));
			len = snprintf(rsp, sizeof(rsp), "RDF %s %s", id, u ? u : "none");
		}
		if (len > 0 && (size_t)len < sizeof(rsp))
			sendto(s->fd, rsp, (size_t)len, 0, (struct sockaddr *)&from, fl);
	}
	return NULL;
}

static int udp_socket(int *port)
{
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	struct sockaddr_in a = { .sin_family = AF_INET,
				 .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	socklen_t l = sizeof(a);
	if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 ||
	    getsockname(fd, (struct sockaddr *)&a, &l) < 0) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	if (port)
		*port = ntohs(a.sin_port);
	return fd;
}

static struct server *start(const struct tables *t, unsigned delay_ms, int *port, int lrf)
{
	struct server *s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;
	s->t = t;
	s->delay_ms = delay_ms;
	s->is_lrf = lrf;
	if ((s->fd = udp_socket(port)) < 0 || pthread_create(&s->thr, NULL, serve, s)) {
		free(s);
		return NULL;
	}
	return s;
}

struct server *lrf_start(const struct tables *t, unsigned d, int *p) { return start(t, d, p, 1); }
struct server *rdf_start(const struct tables *t, unsigned d, int *p) { return start(t, d, p, 0); }

void server_stop(struct server *s)
{
	struct sockaddr_in a;
	socklen_t l = sizeof(a);
	getsockname(s->fd, (struct sockaddr *)&a, &l);
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	sendto(fd, "STOP", 4, 0, (struct sockaddr *)&a, l);
	close(fd);
	pthread_join(s->thr, NULL);
	close(s->fd);
	free(s);
}

/* ---------------- routers ---------------- */

int router_open(struct router *r, const struct tables *t, int lrf_port,
		int rdf_port, unsigned deadline_ms)
{
	memset(r, 0, sizeof(*r));
	r->t = t;
	r->lrf_port = lrf_port;
	r->rdf_port = rdf_port;
	r->deadline_ms = deadline_ms;
	r->fd = udp_socket(NULL);
	return r->fd < 0 ? -1 : 0;
}

void router_close(struct router *r)
{
	close(r->fd);
}

static int send_to(struct router *r, int port, const char *m)
{
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port),
				 .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	return sendto(r->fd, m, strlen(m), 0, (struct sockaddr *)&a, sizeof(a)) < 0 ? -1 : 0;
}

static long long now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

/* Waits for a reply that starts with `prefix` (e.g. "LOC <call-id> "),
 * discarding stale replies to earlier queries. 0 on success, -1 on timeout. */
static int await(struct router *r, const char *prefix, char *buf, size_t cap)
{
	long long end = now_ms() + r->deadline_ms;
	struct pollfd p = { .fd = r->fd, .events = POLLIN };
	size_t pl = strlen(prefix);

	for (;;) {
		long long left = end - now_ms();
		if (left <= 0 || poll(&p, 1, (int)left) <= 0)
			return -1;
		ssize_t n = recv(r->fd, buf, cap - 1, 0);
		if (n <= 0)
			continue;
		buf[n] = 0;
		if ((size_t)n > pl && memcmp(buf, prefix, pl) == 0)
			return 0;
		if (strncmp(buf, "LOC ", 4) == 0)
			r->async_answered++; /* a late background answer */
	}
}

static int prepare(const char *msg, size_t len, struct invite *inv,
		   enum ecall_service *svc, char cell[24], char callid[128])
{
	if (sip_parse(msg, len, inv) < 0)
		return -1;
	if (!(inv->method.n == 6 && memcmp(inv->method.p, "INVITE", 6) == 0))
		return 0;
	if ((*svc = sip_emergency_service(inv->ruri)) == SVC_NONE)
		return 0;
	if (inv->call_id.n >= 128)
		return -1;
	for (size_t i = 0; i < inv->call_id.n; i++) /* keep the text protocol safe */
		if (!isgraph((unsigned char)inv->call_id.p[i]))
			return -1;
	memcpy(callid, inv->call_id.p, inv->call_id.n);
	callid[inv->call_id.n] = 0;
	if (sip_cell_id(inv->pani, cell, 24) < 0)
		cell[0] = 0;
	return 1;
}

static int emit(const struct invite *inv, const char *msg, const char *psap,
		const char *callid, char *out, size_t cap, char psap_out[URI_MAX])
{
	char ref[192];
	snprintf(ref, sizeof(ref), "https://lrf.example.org/loc/%s", callid);
	snprintf(psap_out, URI_MAX, "%s", psap);
	return sip_route(inv, msg, psap, ref, out, cap) ? 1 : -1;
}

/* Serial: LRF for a location, then RDF for a PSAP, then route. */
int route_serial(struct router *r, const char *msg, size_t len, char *out,
		 size_t cap, char psap[URI_MAX])
{
	struct invite inv;
	enum ecall_service svc = SVC_NONE;
	char cell[24], id[128], q[320], a[320], pre[160], uri[URI_MAX];
	int k = prepare(msg, len, &inv, &svc, cell, id);
	if (k <= 0)
		return k;

	const char *chosen = NULL;
	double lat, lon;
	snprintf(q, sizeof(q), "LOC %s %s", id, cell[0] ? cell : "-");
	snprintf(pre, sizeof(pre), "LOC %s ", id);
	if (send_to(r, r->lrf_port, q) == 0 && await(r, pre, a, sizeof(a)) == 0 &&
	    sscanf(a + strlen(pre), "%lf %lf", &lat, &lon) == 2) {
		snprintf(q, sizeof(q), "RDF %s %s %.4f %.4f", id, svc_name(svc), lat, lon);
		snprintf(pre, sizeof(pre), "RDF %s ", id);
		if (send_to(r, r->rdf_port, q) == 0 && await(r, pre, a, sizeof(a)) == 0 &&
		    sscanf(a + strlen(pre), "%95s", uri) == 1 && strcmp(uri, "none") != 0)
			chosen = uri;
	}
	if (!chosen) { /* no location or no answer in time: never block the call */
		chosen = psap_lookup(r->t, NULL, svc);
		r->fallbacks++;
	}
	return chosen ? emit(&inv, msg, chosen, id, out, cap, psap) : -1;
}

/* Fast: route on the cell's tracking area from a local table right away;
 * ask the LRF in the background so the PSAP can dereference the precise
 * location by the Geolocation reference. */
int route_fast(struct router *r, const char *msg, size_t len, char *out,
	       size_t cap, char psap[URI_MAX])
{
	struct invite inv;
	enum ecall_service svc = SVC_NONE;
	char cell[24], id[128], q[320], tac[5];
	int k = prepare(msg, len, &inv, &svc, cell, id);
	if (k <= 0)
		return k;

	int known = cell[0] && tac_of_cell(cell, tac) == 0;
	double lat, lon;
	if (known && cell_location(r->t, tac, &lat, &lon) < 0)
		known = 0;
	const char *chosen = psap_lookup(r->t, known ? tac : NULL, svc);
	if (!known)
		r->fallbacks++;

	snprintf(q, sizeof(q), "LOC %s %s", id, cell[0] ? cell : "-");
	if (send_to(r, r->lrf_port, q) == 0)
		r->async_sent++;
	return chosen ? emit(&inv, msg, chosen, id, out, cap, psap) : -1;
}

void router_collect(struct router *r)
{
	char buf[512];
	ssize_t n;
	while ((n = recv(r->fd, buf, sizeof(buf), MSG_DONTWAIT)) > 0)
		if (n > 4 && memcmp(buf, "LOC ", 4) == 0)
			r->async_answered++;
}
