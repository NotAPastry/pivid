/* Unit test for the caster subscriber table: upsert/renew/age-out/cap + the
 * loopback-accept gate. Pure -- no sockets. */
#include "subscribers.h"
#include <arpa/inet.h>   /* htonl, INADDR_LOOPBACK */
#include <assert.h>
#include <stdio.h>

/* Distinct fake peer IPs (network order doesn't matter for keying/equality). */
static uint32_t ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | d;
}

static void test_upsert_and_count(void) {
    SubTable t; subs_init(&t);
    assert(subs_count(&t) == 0);
    assert(subs_upsert(&t, ip(10,0,0,1), 777, 5000, 5001, 1000) == 0);
    assert(subs_count(&t) == 1);
    assert(subs_upsert(&t, ip(10,0,0,2), 777, 6000, 6001, 1000) == 0);
    assert(subs_count(&t) == 2);
    printf("test_upsert_and_count PASS\n");
}

static void test_renew_is_idempotent(void) {
    SubTable t; subs_init(&t);
    assert(subs_upsert(&t, ip(10,0,0,1), 777, 5000, 5001, 1000) == 0);
    /* Same IP again: renew, not a second entry; ports may change. */
    assert(subs_upsert(&t, ip(10,0,0,1), 777, 7000, 7001, 4000) == 0);
    assert(subs_count(&t) == 1);
    /* The renewed entry survives an age-out that would have dropped the old. */
    assert(subs_age_out(&t, 8000, CASTER_SUB_TTL_MS) == 0);  /* 8000-4000=4000 < 5000 */
    assert(subs_count(&t) == 1);
    printf("test_renew_is_idempotent PASS\n");
}

static void test_age_out(void) {
    SubTable t; subs_init(&t);
    subs_upsert(&t, ip(10,0,0,1), 777, 5000, 5001, 1000);
    subs_upsert(&t, ip(10,0,0,2), 777, 6000, 6001, 3000);
    /* At now=6001: entry1 age 5001 (>5000 -> drop), entry2 age 3001 (keep). */
    assert(subs_age_out(&t, 6001, CASTER_SUB_TTL_MS) == 1);
    assert(subs_count(&t) == 1);
    printf("test_age_out PASS\n");
}

static void test_remove(void) {
    SubTable t; subs_init(&t);
    subs_upsert(&t, ip(10,0,0,1), 777, 5000, 5001, 1000);
    subs_upsert(&t, ip(10,0,0,2), 777, 6000, 6001, 1000);
    subs_remove(&t, ip(10,0,0,1), 777);
    assert(subs_count(&t) == 1);
    subs_remove(&t, ip(10,0,0,9), 777);   /* absent -> no-op */
    assert(subs_count(&t) == 1);
    printf("test_remove PASS\n");
}

static void test_cap_rejects_new(void) {
    SubTable t; subs_init(&t);
    for (int i = 0; i < CASTER_MAX_SUBSCRIBERS; i++)
        assert(subs_upsert(&t, ip(10,0,0,(uint8_t)(i + 1)), 777, 5000, 5001, 1000) == 0);
    assert(subs_count(&t) == CASTER_MAX_SUBSCRIBERS);
    /* Over-cap NEW ip is rejected. */
    assert(subs_upsert(&t, ip(10,0,0,200), 777, 5000, 5001, 1000) == -1);
    /* But renewing an EXISTING ip at cap still works (no new slot needed). */
    assert(subs_upsert(&t, ip(10,0,0,1), 777, 9000, 9001, 2000) == 0);
    assert(subs_count(&t) == CASTER_MAX_SUBSCRIBERS);
    printf("test_cap_rejects_new PASS\n");
}

static void test_src_gate(void) {
    uint32_t loop = htonl(INADDR_LOOPBACK);   /* 127.0.0.1, network order */
    uint32_t remote = htonl(0x0A000001u);     /* 10.0.0.1 */
    /* Forwarding OFF: only loopback allowed. */
    assert(subs_src_allowed(loop,   false) == true);
    assert(subs_src_allowed(remote, false) == false);
    /* Forwarding ON: both allowed. */
    assert(subs_src_allowed(loop,   true)  == true);
    assert(subs_src_allowed(remote, true)  == true);
    printf("test_src_gate PASS\n");
}


/* --- Same-IP instances are DISTINCT subscribers -------------------------
 * Two viewers on one host (RX-local player + a udp_tap capture, both
 * 127.0.0.1) each keepalive from their own control socket. Keyed by IP
 * alone they clobbered one slot's video_port on every keepalive, starving
 * each other in a sawtooth (bottom-half-corruption investigation taint,
 * 2026-07-05). Key = (ip, ctrl_port) = the client instance. */
static void test_same_ip_two_instances_coexist(void) {
    SubTable t; subs_init(&t);
    /* Same IP, two control sockets -> two entries. */
    assert(subs_upsert(&t, ip(127,0,0,1), 1111, 5000, 5001, 1000) == 0);
    assert(subs_upsert(&t, ip(127,0,0,1), 2222, 6000, 6001, 1000) == 0);
    assert(subs_count(&t) == 2);
    /* Instance 1 keepalive must NOT clobber instance 2's video_port. */
    assert(subs_upsert(&t, ip(127,0,0,1), 1111, 5000, 5001, 3000) == 0);
    assert(subs_count(&t) == 2);
    int seen5000 = 0, seen6000 = 0;
    for (int i = 0; i < CASTER_MAX_SUBSCRIBERS; i++) {
        if (!t.e[i].used) continue;
        if (t.e[i].video_port == 5000) seen5000++;
        if (t.e[i].video_port == 6000) seen6000++;
    }
    assert(seen5000 == 1 && seen6000 == 1);
    printf("test_same_ip_two_instances_coexist PASS\n");
}

static void test_same_ip_unsubscribe_removes_only_own_instance(void) {
    SubTable t; subs_init(&t);
    subs_upsert(&t, ip(127,0,0,1), 1111, 5000, 5001, 1000);
    subs_upsert(&t, ip(127,0,0,1), 2222, 6000, 6001, 1000);
    subs_remove(&t, ip(127,0,0,1), 1111);   /* instance 1 leaves */
    assert(subs_count(&t) == 1);
    /* The survivor is instance 2. */
    for (int i = 0; i < CASTER_MAX_SUBSCRIBERS; i++)
        if (t.e[i].used) assert(t.e[i].video_port == 6000);
    printf("test_same_ip_unsubscribe_removes_only_own_instance PASS\n");
}

int main(void) {
    test_upsert_and_count();
    test_renew_is_idempotent();
    test_age_out();
    test_remove();
    test_cap_rejects_new();
    test_src_gate();
    test_same_ip_two_instances_coexist();
    test_same_ip_unsubscribe_removes_only_own_instance();
    printf("ALL test_caster_subscribers PASS\n");
    return 0;
}
