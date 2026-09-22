/* SPDX-License-Identifier: MIT */
/* ecall-bench <calls> <lrf-ms> <rdf-ms>: time from INVITE received to routed
 * INVITE ready, for the serial and the fast strategy. */
#define _GNU_SOURCE
#include "route.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static int cmp(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

static void report(const char *name, double *v, size_t n, size_t fallbacks)
{
	qsort(v, n, sizeof(*v), cmp);
	printf("%-7s p50 %8.3f ms   p99 %8.3f ms   max %8.3f ms   fallbacks %zu\n",
	       name, v[n / 2], v[n * 99 / 100], v[n - 1], fallbacks);
}

int main(int argc, char **argv)
{
	size_t n = argc > 1 ? strtoul(argv[1], NULL, 10) : 200;
	unsigned lrf_ms = argc > 2 ? (unsigned)atoi(argv[2]) : 10;
	unsigned rdf_ms = argc > 3 ? (unsigned)atoi(argv[3]) : 5;
	struct tables t;
	if (n == 0 || tables_load(&t, "data/cells.csv", "data/psap.csv") < 0)
		return 1;

	int lp, rp;
	struct server *l = lrf_start(&t, lrf_ms, &lp), *r = rdf_start(&t, rdf_ms, &rp);
	struct router a, b;
	router_open(&a, &t, lp, rp, 2000);
	router_open(&b, &t, lp, rp, 2000);

	double *ts = malloc(n * sizeof(double)), *tf = malloc(n * sizeof(double));
	char m[1024], out[2048], psap[URI_MAX];
	/* two separate phases so the fast path's background LRF traffic can't
	 * queue in front of the serial path's queries */
	for (int phase = 0; phase < 2; phase++)
		for (size_t i = 0; i < n; i++) {
			int len = snprintf(m, sizeof(m),
				"INVITE urn:service:sos SIP/2.0\r\nCall-ID: bench-%d-%zu\r\n"
				"P-Access-Network-Info: 3GPP-E-UTRAN-FDD; utran-cell-id-3gpp=40498%s0000001\r\n"
				"Content-Length: 0\r\n\r\n", phase, i, t.cell[i % t.ncell].tac);
			double t0 = now_ms();
			if (phase == 0) {
				route_serial(&a, m, (size_t)len, out, sizeof(out), psap);
				ts[i] = now_ms() - t0;
			} else {
				route_fast(&b, m, (size_t)len, out, sizeof(out), psap);
				tf[i] = now_ms() - t0;
				router_collect(&b);
			}
		}
	printf("%zu emergency INVITEs, LRF %u ms, RDF %u ms (simulated over UDP on loopback)\n",
	       n, lrf_ms, rdf_ms);
	report("serial", ts, n, a.fallbacks);
	report("fast", tf, n, b.fallbacks);

	/* Routing no longer waits for the LRF, but the LRF still has to keep up:
	 * measure how long its backlog from this burst takes to drain. */
	double d0 = now_ms(), limit = (double)n * lrf_ms + 1000;
	while (b.async_answered < b.async_sent && now_ms() - d0 < limit) {
		usleep(1000);
		router_collect(&b);
	}
	printf("fast path: %zu background LRF queries, %zu answered; backlog drained "
	       "%.0f ms after the last call was routed\n",
	       b.async_sent, b.async_answered, now_ms() - d0);

	router_close(&a);
	router_close(&b);
	server_stop(l);
	server_stop(r);
	free(ts);
	free(tf);
	return 0;
}
