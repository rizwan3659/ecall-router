/* SPDX-License-Identifier: MIT */
#ifndef ROUTE_H
#define ROUTE_H

#include "sip.h"
#include <stddef.h>

#define URI_MAX 96
#define MAX_ROWS 256

struct cell { char tac[5]; double lat, lon; };
struct psap { char tac[5]; enum ecall_service svc; char uri[URI_MAX]; };

struct tables {
	struct cell cell[MAX_ROWS];
	size_t ncell;
	struct psap psap[MAX_ROWS];
	size_t npsap;
};

int tables_load(struct tables *t, const char *cells_csv, const char *psap_csv);

/* TAC (4 hex digits) from a cell id: MCC(3) MNC(2|3) TAC(4) ECI(7). */
int tac_of_cell(const char *cell_id, char tac[5]);

/* PSAP for a tracking area and service, with fallbacks in this order:
 * (tac, svc) -> (tac, sos) -> (*, svc) -> (*, sos). NULL only if the table
 * has no default at all. */
const char *psap_lookup(const struct tables *t, const char *tac, enum ecall_service s);

/* Centroid of a TAC, and the TAC nearest a point (what an RDF does). */
int cell_location(const struct tables *t, const char *tac, double *lat, double *lon);
const char *nearest_tac(const struct tables *t, double lat, double lon);

/* ---- LRF and RDF as UDP services on 127.0.0.1, answering after a delay ---- */
struct server;
struct server *lrf_start(const struct tables *t, unsigned delay_ms, int *port);
struct server *rdf_start(const struct tables *t, unsigned delay_ms, int *port);
void server_stop(struct server *s);

/* ---- the two routing strategies ---- */
struct router {
	const struct tables *t;
	int fd;                  /* UDP socket for LRF/RDF queries */
	int lrf_port, rdf_port;
	unsigned deadline_ms;    /* per remote query */
	size_t async_sent, async_answered, fallbacks;
};

int router_open(struct router *r, const struct tables *t, int lrf_port,
		int rdf_port, unsigned deadline_ms);
void router_close(struct router *r);

/* Returns 1 if routed (out filled, psap chosen), 0 if not an emergency
 * call, -1 if the message is malformed or the output did not fit. */
int route_serial(struct router *r, const char *msg, size_t len, char *out,
		 size_t cap, char psap[URI_MAX]);
int route_fast(struct router *r, const char *msg, size_t len, char *out,
	       size_t cap, char psap[URI_MAX]);

/* Collect background LRF answers (fast path) without blocking. */
void router_collect(struct router *r);

#endif
