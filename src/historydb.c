/*
 *	aprsc
 *
 *	(c) Matti Aarnio, OH2MQK, <oh2mqk@sral.fi>
 *
 *	This program is licensed under the BSD license, which can be found
 *	in the file LICENSE.
 *	
 */

#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>

#include "hlog.h"
#include "worker.h"
#include "config.h"
#include "cellmalloc.h"
#include "historydb.h"
#include "hmalloc.h"
#include "keyhash.h"
#include "cJSON.h"

#ifndef _FOR_VALGRIND_
cellarena_t *historydb_cells;
#endif


/* OPTIMIZE: Possibly multiple parallel locks (like 1000 ?) that keep
//control on a subset of historydb hash bucket chains ???
// Note: mutex lock size is about 1/4 of rwlock size...
*/

rwlock_t historydb_rwlock;

#define HISTORYDB_HASH_MODULO 8192 /* fold bits: 13 / 26 */
struct history_cell_t *historydb_hash[HISTORYDB_HASH_MODULO];

/* monitor counters and gauges */
long historydb_inserts;
long historydb_lookups;
long historydb_hashmatches;
long historydb_keymatches;
long historydb_cellgauge;
long historydb_noposcount;

long historydb_cleanup_cleaned;

void historydb_nopos(void) {}         /* profiler call counter items */
void historydb_nointerest(void) {}
void historydb_hashmatch(void) {}
void historydb_keymatch(void) {}
void historydb_dataupdate(void) {}


void historydb_init(void)
{
	rwl_init(&historydb_rwlock);

	// printf("historydb_init() sizeof(mutex)=%d sizeof(rwlock)=%d\n",
	//       sizeof(pthread_mutex_t), sizeof(rwlock_t));

#ifndef _FOR_VALGRIND_
	historydb_cells = cellinit( "historydb",
				    sizeof(struct history_cell_t),
				    __alignof__(struct history_cell_t), 
				    CELLMALLOC_POLICY_FIFO,
				    2048 /* 2 MB */,
				    0 /* minfree */ );
#endif
}

/* Called only under WR-LOCK */
static void historydb_free(struct history_cell_t *p)
{
#ifndef _FOR_VALGRIND_
	cellfree( historydb_cells, p );
#else
	hfree(p);
#endif
	--historydb_cellgauge;
}

/* Called only under WR-LOCK */
static struct history_cell_t *historydb_alloc(void)
{
	++historydb_cellgauge;
#ifndef _FOR_VALGRIND_
	return cellmalloc( historydb_cells );
#else
	return hmalloc(sizeof(struct history_cell_t)+packet_len);
#endif
}

/*
 *     The  historydb_atend()  does exist primarily to make valgrind
 *     happy about lost memory object tracking.
 */
void historydb_atend(void)
{
	int i;
	struct history_cell_t *hp, *hp2;
	for (i = 0; i < HISTORYDB_HASH_MODULO; ++i) {
		hp = historydb_hash[i];
		while (hp) {
			hp2 = hp->next;
			historydb_free(hp);
			hp = hp2;
		}
		historydb_hash[i] = NULL;
	}
}

static int historydb_dump_entry(FILE *fp, struct history_cell_t *hp)
{
	int klen;
	char key[CALLSIGNLEN_MAX+1];
	
	/* create a null-terminated key string */
	klen = (hp->keylen < CALLSIGNLEN_MAX) ? hp->keylen : CALLSIGNLEN_MAX;
	strncpy(key, hp->key, klen);
	key[klen] = 0;

	cJSON *js = cJSON_CreateObject();
	cJSON_AddNumberToObject(js, "arrivaltime", hp->arrivaltime);
	cJSON_AddStringToObject(js, "key", key);
	cJSON_AddNumberToObject(js, "packettype", hp->packettype);
	cJSON_AddNumberToObject(js, "flags", hp->flags);
	cJSON_AddNumberToObject(js, "lat", (double)hp->lat);
	cJSON_AddNumberToObject(js, "lon", (double)hp->lon);
	
	/* the tree is built, print it out to a malloc'ed string */
	char *out = cJSON_PrintUnformatted(js);
	cJSON_Delete(js);
	klen = fprintf(fp, "%s\n", out);
	hfree(out);
	
	if (klen < 0)
		hlog(LOG_ERR, "historydb_dump_entry failed to write entry: %s", strerror(errno));
		
	return klen;
}

static int historydb_load_entry(char *s)
{
	cJSON *j;
	cJSON *arrivaltime, *key, *packettype, *flags, *lat, *lon;
	struct history_cell_t *cp;
	int keylen;
	uint32_t h1, h2, i;
	time_t expirytime   = tick - lastposition_storetime;
	
	j = cJSON_Parse(s);
	if (!j) {
		hlog(LOG_ERR, "historydb_load_entry JSON decode failed: %s", s);
		return -1;
	}
	
	arrivaltime = cJSON_GetObjectItem(j, "arrivaltime");
	key = cJSON_GetObjectItem(j, "key");
	packettype = cJSON_GetObjectItem(j, "packettype");
	flags = cJSON_GetObjectItem(j, "flags");
	lat = cJSON_GetObjectItem(j, "lat");
	lon = cJSON_GetObjectItem(j, "lon");
	
	/* make sure all required keys are present */
	if (!((arrivaltime) && (key) && (packettype) && (flags) && (lat) && (lon)))
		goto fail;
	
	/* check types of items */
	if (arrivaltime->type != cJSON_Number
		|| key->type != cJSON_String
		|| packettype->type != cJSON_Number
		|| flags->type != cJSON_Number
		|| lat->type != cJSON_Number
		|| lon->type != cJSON_Number) {
			goto fail;
	}
	
	if (arrivaltime->valueint < expirytime) {
		/* too old */
		goto fail;
	}
	
	keylen = strlen(key->valuestring);
	
	/* ok, we're going to add this one - allocate, fill and push */
	cp = historydb_alloc();
	if (!cp) {
		hlog(LOG_ERR, "historydb_load_entry: cellmalloc failed");
		goto fail;
	}
	
	/* calculate hash */
	h1 = keyhash(key->valuestring, keylen, 0);
	h2 = h1 ^ (h1 >> 13) ^ (h1 >> 26); /* fold hash bits.. */
	i = h2 % HISTORYDB_HASH_MODULO;

	memcpy(cp->key, key->valuestring, keylen);
	cp->key[keylen] = 0; /* zero terminate */
	cp->keylen = keylen;
	cp->hash1 = h1;
	
	cp->lat         = lat->valuedouble;
	cp->coslat      = cosf(cp->lat);
	cp->lon         = lon->valuedouble;
	cp->arrivaltime = arrivaltime->valueint;
	cp->packettype  = packettype->valueint;
	cp->flags       = flags->valueint;

	/* ok, insert it in the hash table */
	cp->next = historydb_hash[i];
	historydb_hash[i] = cp;
	
	cJSON_Delete(j);
	return 1;
	
fail:	
	cJSON_Delete(j);
	
	return 0;
}

int historydb_dump(FILE *fp)
{
	/* Dump the historydb out on text format */
	int i;
	struct history_cell_t *hp;
	time_t expirytime   = tick - lastposition_storetime;
	int ret = 0;

	/* multiple locks ? one for each bucket, or for a subset of buckets ? */
	rwl_rdlock(&historydb_rwlock);

	for ( i = 0; i < HISTORYDB_HASH_MODULO; ++i ) {
		hp = historydb_hash[i];
		for ( ; hp ; hp = hp->next )
			if (hp->arrivaltime > expirytime) {
				if (historydb_dump_entry(fp, hp) < 0) {
					ret = -1;
					goto fail;
				}
			}
	}
	
fail:	
	/* Free the lock */
	rwl_rdunlock(&historydb_rwlock);
	
	return ret;
}

int historydb_load(FILE *fp)
{
	char *s;
	int n = 0;
	int ok = 0;
	char buf[32768];
	
	rwl_wrlock(&historydb_rwlock);
	
	while ((s = fgets(buf, sizeof(buf), fp))) {
		// squelch warning: the json file is read from disk, written by ourself when starting live upgrade
		// coverity[tainted_data]
		if (historydb_load_entry(s) > 0)
			ok++;
		n++;
	}
	
	rwl_wrunlock(&historydb_rwlock);
	
	hlog(LOG_INFO, "Loaded %d of %d historydb entries.", ok, n);
	
	return 0;
}

/* insert... */

int historydb_insert(struct pbuf_t *pb)
{
	int i;
	uint32_t h1, h2;
	int isdead = 0, keylen;
	struct history_cell_t **hp, *cp, *cp1;

	time_t expirytime   = tick - lastposition_storetime;

	char keybuf[CALLSIGNLEN_MAX+2];
	char *s;

	if (!(pb->flags & F_HASPOS)) {
		++historydb_noposcount;
		historydb_nopos(); /* debug thing -- profiling counter */
		return -1; /* No positional data... */
	}

	/* NOTE: Parser does set on MESSAGES the RECIPIENTS
	**       location if such is known! We do not want them...
	**       .. and several other cases where packet has no
	**       positional data in it, but source callsign may
	**       have previous entry with data.
	*/

	/* NOTE2: We could use pb->srcname, and pb->srcname_len here,
	**        but then we would not know if this is a "kill-item"
	*/

	keybuf[CALLSIGNLEN_MAX] = 0;
	if (pb->packettype & T_OBJECT) {
		/* Pick object name  ";item  *" */
		memcpy( keybuf, pb->info_start+1, CALLSIGNLEN_MAX+1);
		keybuf[CALLSIGNLEN_MAX+1] = 0;
		s = strchr(keybuf, '*');
		if (s) *s = 0;
		else {
			s = strchr(keybuf, '_'); // kill an object!
			if (s) {
				*s = 0;
				isdead = 1;
			}
		}
		s = keybuf + strlen(keybuf);
		for ( ; s > keybuf; --s ) {  // tail space padded..
			if (*s == ' ') *s = ' ';
			else break;
		}

	} else if (pb->packettype & T_ITEM) {
		// Pick item name  ") . . . !"  or ") . . . _"
		memcpy( keybuf, pb->info_start+1, CALLSIGNLEN_MAX+1);
		keybuf[CALLSIGNLEN_MAX+1] = 0;
		s = strchr(keybuf, '!');
		if (s) *s = 0;
		else {
			s = strchr(keybuf, '_'); // kill an item!
			if (s) {
				*s = 0;
				isdead = 1;
			}
		}
	} else if (pb->packettype & T_POSITION) {
		// Pick originator callsign
		memcpy( keybuf, pb->data, CALLSIGNLEN_MAX) ;
		s = strchr(keybuf, '>');
		if (s) *s = 0;
	} else {
		historydb_nointerest(); // debug thing -- a profiling counter
		return -1; // Not a packet with positional data, not interested in...
	}
	keylen = strlen(keybuf);

	++historydb_inserts;

	h1 = keyhash(keybuf, keylen, 0);
	h2 = h1 ^ (h1 >> 13) ^ (h1 >> 26); /* fold hash bits.. */
	i = h2 % HISTORYDB_HASH_MODULO;

	cp = cp1 = NULL;
	hp = &historydb_hash[i];

	// multiple locks ? one for each bucket, or for a subset of buckets ?
	rwl_wrlock(&historydb_rwlock);

	// scan the hash-bucket chain, and do incidential obsolete data discard
	while (( cp = *hp )) {
		if (cp->arrivaltime < expirytime) {
			// OLD...
			*hp = cp->next;
			cp->next = NULL;
			historydb_free(cp);
			continue;
		}
		if (cp->hash1 == h1) {
		       // Hash match, compare the key
		    historydb_hashmatch(); // debug thing -- a profiling counter
		    ++historydb_hashmatches;
		    if ( cp->keylen == keylen &&
			 (memcmp(cp->key, keybuf, keylen) == 0) ) {
		  	// Key match!
		    	historydb_keymatch(); // debug thing -- a profiling counter
			++historydb_keymatches;
			if (isdead) {
				// Remove this key..
				*hp = cp->next;
				cp->next = NULL;
				historydb_free(cp);
				continue;
			} else {
				historydb_dataupdate(); // debug thing -- a profiling counter
				// Update the data content
				cp1 = cp;
				cp->lat         = pb->lat;
				cp->coslat      = pb->cos_lat;
				cp->lon         = pb->lng;
				cp->arrivaltime = pb->t;
				cp->packettype  = pb->packettype;
				cp->flags       = pb->flags;
			}
		    }
		} // .. else no match, advance hp..
		hp = &(cp -> next);
	}

	if (!cp1 && !isdead) {
		// Not found on this chain, append it!
		cp = historydb_alloc();
		if (!cp) {
			hlog(LOG_ERR, "historydb: cellmalloc failed");
			rwl_wrunlock(&historydb_rwlock);
			return 1;
		}
		cp->next = NULL;
		memcpy(cp->key, keybuf, keylen);
		cp->key[keylen] = 0; /* zero terminate */
		cp->keylen = keylen;
		cp->hash1 = h1;

		cp->lat         = pb->lat;
		cp->coslat      = pb->cos_lat;
		cp->lon         = pb->lng;
		cp->arrivaltime = pb->t;
		cp->packettype  = pb->packettype;
		cp->flags       = pb->flags;

		*hp = cp; 
	}

	// Free the lock
	rwl_wrunlock(&historydb_rwlock);

	return 1;
}

/* lookup... */

int historydb_lookup(const char *keybuf, const int keylen, struct history_cell_t **result)
{
	int i;
	uint32_t h1, h2;
	struct history_cell_t *cp;

	// validity is 5 minutes shorter than expiration time..
	time_t validitytime   = tick - lastposition_storetime + 5*60;

	++historydb_lookups;

	h1 = keyhash(keybuf, keylen, 0);
	h2 = h1 ^ (h1 >> 13) ^ (h1 >> 26); /* fold hash bits.. */
	i = h2 % HISTORYDB_HASH_MODULO;

	cp = historydb_hash[i];

	// multiple locks ? one for each bucket, or for a subset of buckets ?
	rwl_rdlock(&historydb_rwlock);

	while ( cp ) {
		if ( (cp->hash1 == h1) &&
		     // Hash match, compare the key
		     (cp->keylen == keylen) &&
		     (memcmp(cp->key, keybuf, keylen) == 0)  &&
		     // Key match!
		     (cp->arrivaltime > validitytime)
		     // NOT too old..
		     ) {
			break;
		}
		// Pick next possible item in hash chain
		cp = cp->next;
	}

	// Free the lock
	rwl_rdunlock(&historydb_rwlock);

	// cp variable has the result
	*result = cp;

	if (!cp) return 0;  // Not found anything

	return 1;
}



/*
 *	The  historydb_cleanup()  exists to purge too old data out of
 *	the database at regular intervals.  Call this about once a minute.
 */

void historydb_cleanup(void)
{
	struct history_cell_t **hp, *cp;
	int i;
	long cleaned = 0;

	// validity is 5 minutes shorter than expiration time..
	time_t expirytime   = tick - lastposition_storetime;


	for (i = 0; i < HISTORYDB_HASH_MODULO; ++i) {
		hp = &historydb_hash[i];

		// multiple locks ? one for each bucket, or for a subset of buckets ?
		// .. or should we just lock outside the for(i ...) loop ?
		rwl_wrlock(&historydb_rwlock);

		while (( cp = *hp )) {
			if (cp->arrivaltime < expirytime) {
				// OLD...
				*hp = cp->next;
				cp->next = NULL;
				historydb_free(cp);
				++cleaned;
				continue;
			}
			/* No expiry, just advance the pointer */
			hp = &(cp -> next);
		}

		// Free the lock
		rwl_wrunlock(&historydb_rwlock);
	}
	
	historydb_cleanup_cleaned = cleaned;
	
	// hlog( LOG_DEBUG, "historydb_cleanup() removed %d entries, count now %ld",
	//       cleaned, historydb_cellgauge );
}

/*
 *	cellmalloc status
 */

#ifndef _FOR_VALGRIND_
void historydb_cell_stats(struct cellstatus_t *cellst)
{
	rwl_rdlock(&historydb_rwlock);
	cellstatus(historydb_cells, cellst);
	rwl_rdunlock(&historydb_rwlock);
}
#endif


