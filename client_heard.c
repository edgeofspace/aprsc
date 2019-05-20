/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *	
 */

 /*
  *	The clients heard list contains a list of stations heard by
  *	a given station. It's used for message routing by the
  *	message destination callsign.
  *
  *	The module also maintains a list of callsigns which have transmitted
  *	messages to a given client. The list is used to pass courtesy
  *	positions to the client.
  *	
  *	The heard list is only touched by the worker thread operating
  *	on that client socket, so it doesn't need any locking at all.
  *
  *	Igates typically have up to ~300 "heard" stations at any given
  *	moment, so we use a hash table with 16 buckets, about 18 calls
  *	in each bucket. Store and check the hash value before doing
  *	string comparisons.
  */

#include <strings.h>
#include <string.h>

#include "client_heard.h"
#include "hlog.h"
#include "config.h"
#include "hmalloc.h"
#include "cellmalloc.h"
#include "keyhash.h"

//#define HEARD_DEBUG

#ifdef HEARD_DEBUG
#define DLOG(level, fmt, ...) \
            do { hlog(level, fmt, __VA_ARGS__); } while (0)
#else
#define DLOG(fmt, ...)
#endif

#ifndef _FOR_VALGRIND_
cellarena_t *client_heard_cells;
#endif


/*
 *	Update the heard list, either update timestamp of a heard
 *	callsign or insert a new entry
 */

static void heard_list_update(struct client_t *c, char *call, int call_len, time_t t, struct client_heard_t **list, int *entrycount, char *which)
{
	struct client_heard_t *h;
	uint32_t hash, idx;
	int i;
	
	hash = keyhashuc(call, call_len, 0);
	idx = hash;
	// "CLIENT_HEARD_BUCKETS" is 16..
	idx ^= (idx >> 16);
	//idx ^= (idx >>  8);
	//idx ^= (idx >>  4);
	i = idx % CLIENT_HEARD_BUCKETS;
	
	DLOG(LOG_DEBUG, "heard_list_update fd %d %s: updating %.*s (hash %u i %d)", c->fd, which, call_len, call, hash, i);
	
	for (h = list[i]; (h); h = h->next) {
		if (h->hash == hash && call_len == h->call_len
		    && strncasecmp(call, h->callsign, h->call_len) == 0) {
			// OK, found it from the list

			//DLOG(LOG_DEBUG, "heard_list_update fd %d %s: found, updating %.*s", c->fd, which, call_len, call);
			h->last_heard = t;
			
			/* Because of digipeating we'll see the same station
			 * really quickly again, and the less active stations are, well, less active,
			 * let's move it in the beginning of the list to find it quicker.
			 */
			 
			 if (list[i] != h) {
				//DLOG(LOG_DEBUG, "heard_list_update fd %d %s: moving to front %.*s", c->fd, which, call_len, call);
				*h->prevp = h->next;
				if (h->next)
					h->next->prevp = h->prevp;
				
				h->next = list[i];
				h->prevp = &list[i];
				if (h->next)
					h->next->prevp = &h->next;
				list[i] = h;
			}
			
			return;
		}
	}
	
	/* Not found, insert. */
	DLOG(LOG_DEBUG, "heard_list_update fd %d %s: inserting %.*s", c->fd, which, call_len, call);
#ifndef _FOR_VALGRIND_
	h = cellmalloc(client_heard_cells);
	if (!h) {
	        hlog(LOG_ERR, "heard_list_update: cellmalloc failed");
	        return;
	}
#else	
	h = hmalloc(sizeof(*h));
#endif
	h->hash = hash;
	strncpy(h->callsign, call, call_len);
	h->callsign[sizeof(h->callsign)-1] = 0;
	h->call_len = call_len;
	h->last_heard = t;
	
	/* insert in beginning of linked list */
	h->next = list[i];
	h->prevp = &list[i];
	if (h->next)
		h->next->prevp = &h->next;
	list[i] = h;
	*entrycount = *entrycount + 1;
}

void client_heard_update(struct client_t *c, struct pbuf_t *pb)
{
	heard_list_update(c, pb->data, pb->srccall_end - pb->data, pb->t, c->client_heard, &c->client_heard_count, "heard");
}

void client_courtesy_update(struct client_t *c, struct pbuf_t *pb)
{
	heard_list_update(c, pb->data, pb->srccall_end - pb->data, pb->t, c->client_courtesy, &c->client_courtesy_count, "courtesy");
}

/*
 *	Check for and optionally remove a callsign from a heard list.
 *
 *	NOTE: THIS IS THE MOST FREQUENTLY CALLED FUNCTION IN THE APPLICATION.
 *
 *	It's called more than once per packet per filtered client,
 *	currently about 1.24 * number-of-filtered-clients * packets.
 *	Not very scalable, as the number of packets goes up with the amount of
 *	clients on the network, O(N^2) complexity.
 *	A single tree keyed by the callsign and containing a list of clients
 *	having heard the callsign, looked up once per packet, would be O(N).
 *	For the time being this will work fine.
 *
 *	Do not bloat.
 */

static int heard_find(struct client_heard_t **list, int *entrycount, const char *callsign, int call_len, uint32_t hash, int drop_if_found)
{
	struct client_heard_t *h;
	uint32_t idx;
	
	// "CLIENT_HEARD_BUCKETS" is 16..
	/*
	idx = hash;
	idx ^= (idx >> 16);
	//idx ^= (idx >>  8);
	//idx ^= (idx >>  4);
	idx = idx % CLIENT_HEARD_BUCKETS;
	*/
	idx = (hash ^ (hash >> 16)) % CLIENT_HEARD_BUCKETS;
	
	//DLOG(LOG_DEBUG, "heard_find fd %d %s: checking for %.*s (hash %u idx %d)", c->fd, which, call_len, callsign, hash, idx);
	
	for (h = list[idx]; (h); h = h->next) {
		if (h->hash == hash && call_len == h->call_len
		    && strncasecmp(callsign, h->callsign, h->call_len) == 0) {
			/* OK, found it from the list. */
			//DLOG(LOG_DEBUG, "heard_find: found %.*s%s", call_len, callsign, (drop_if_found) ? " (dropping)" : "");
			
			if (drop_if_found) {
				//DLOG(LOG_DEBUG, "heard_find: dropping %.*s%s", call_len, callsign, (drop_if_found) ? " (dropping)" : "");
				*h->prevp = h->next;
				if (h->next)
					h->next->prevp = h->prevp;
				
#ifndef _FOR_VALGRIND_
				cellfree(client_heard_cells, h);
#else
				hfree(h);
#endif
				*entrycount = *entrycount -1;
			}
			
			return 1;
		}
	}
	
	return 0;
}

int client_heard_check(struct client_t *c, const char *callsign, int call_len, uint32_t hash)
{
	return heard_find(c->client_heard, &c->client_heard_count,
		callsign, call_len, hash,
		0);
}

int client_courtesy_needed(struct client_t *c, struct pbuf_t *pb)
{
	return heard_find(c->client_courtesy, &c->client_courtesy_count,
		pb->srcname, pb->srcname_len, pb->srcname_hash,
		1);
}

/*
 *	Free the whole client heard list
 */

static void heard_free_single(struct client_heard_t **list)
{
	int i;
	struct client_heard_t *n, *h;
	
	for (i = 0; i < CLIENT_HEARD_BUCKETS; i++) {
		h = list[i];
		while (h) {
			n = h->next;
#ifndef _FOR_VALGRIND_
			cellfree(client_heard_cells, h);
#else
			hfree(h);
#endif
			h = n;
		}
		list[i] = NULL;
	}
}

void client_heard_free(struct client_t *c)
{
	heard_free_single(c->client_heard);
	heard_free_single(c->client_courtesy);
	
	c->client_heard_count = 0;
	c->client_courtesy_count = 0;
}

/*
 *	Expire old entries from client_heard list
 */

static void heard_expire_single(struct client_heard_t **list, int *entrycount, int storetime)
{
	struct client_heard_t *n, *h;
	int expire_below = tick - storetime;
	int i;
	
	for (i = 0; i < CLIENT_HEARD_BUCKETS; i++) {
		h = list[i];
		while (h) {
			n = h->next;
			if (h->last_heard < expire_below || h->last_heard > tick) {
				DLOG(LOG_DEBUG, "heard_expire_single: expiring %.*s (%ul below %ul)", h->call_len, h->callsign, h->last_heard, expire_below);
				
				*h->prevp = h->next;
				if (h->next)
					h->next->prevp = h->prevp;
#ifndef _FOR_VALGRIND_
				cellfree(client_heard_cells, h);
#else
				hfree(h);
#endif
				*entrycount = *entrycount -1;
			}
			h = n;
		}
	}
}

void client_heard_expire(struct client_t *c)
{
	heard_expire_single(c->client_heard, &c->client_heard_count, heard_list_storetime);
	heard_expire_single(c->client_courtesy, &c->client_courtesy_count, courtesy_list_storetime);
}


void client_heard_init(void)
{
#ifndef _FOR_VALGRIND_
	client_heard_cells  = cellinit( "client_heard",
				  sizeof(struct client_heard_t),
				  __alignof__(struct client_heard_t),
				  CELLMALLOC_POLICY_FIFO,
				  512 /* 512 KB at the time */, 0 /* minfree */ );
	/* 512 KB arena size -> about 18k entries per single arena */
#endif
}

/*
 *	Generate JSON tree containing a client_heard_t list
 */

struct cJSON *client_heard_json(struct client_heard_t **list)
{
	struct client_heard_t *h;
	int i;
	cJSON *j = cJSON_CreateArray();
	
	for (i = 0; i < CLIENT_HEARD_BUCKETS; i++) {
		h = list[i];
		while (h) {
			cJSON_AddItemToArray(j, cJSON_CreateString(h->callsign));
			h = h->next;
		}
	}
	
	return j;
}

/*
 *	Load client_heard_t list from a JSON dump
 */

int client_heard_json_load(struct client_t *c, cJSON *dump)
{
	int i, len;
	cJSON *h;
	time_t t = tick;
	
	len = cJSON_GetArraySize(dump);
	
	for (i = 0; i < len; i++) {
		h = cJSON_GetArrayItem(dump, i);
		if (!h || !h->valuestring)
			continue;
		heard_list_update(c, h->valuestring, strlen(h->valuestring), t, c->client_heard, &c->client_heard_count, "heard");
		
	}
	
	return i;
}

/*
 *	cellmalloc status
 */
#ifndef _FOR_VALGRIND_
void client_heard_cell_stats(struct cellstatus_t *cellst)
{
	/* this is not quite thread safe, but is OK since the
	 * results are only used for statistics (no-one will
	 * notice if they don't quite add up always)
	 */
	cellstatus(client_heard_cells, cellst);
}
#endif


