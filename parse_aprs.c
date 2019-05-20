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
 *	A simple APRS parser for aprsc. Translated from Ham::APRS::FAP
 *	perl module (by OH2KKU).
 *
 *	Only needs to get lat/lng out of the packet, other features would
 *	be unnecessary in this application, and slow down the parser.
 *      ... but lets still classify the packet, output filter needs that.
 *	
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#include "parse_aprs.h"
#include "hlog.h"
#include "filter.h"
#include "historydb.h"
#include "incoming.h"

//#define DEBUG_PARSE_APRS 1
#ifdef DEBUG_PARSE_APRS
#define DEBUG_LOG(...) \
            do { hlog(LOG_DEBUG, __VA_ARGS__); } while (0)
#else
#define DEBUG_LOG(...) { }
#endif

/*
 *	Check if the given character is a valid symbol table identifier
 *	or an overlay character. The set is different for compressed
 *	and uncompressed packets - the former has the overlaid number (0-9)
 *	replaced with n-j.
 */

static int valid_sym_table_compressed(char c)
{
	return (c == '/' || c == '\\' || (c >= 0x41 && c <= 0x5A)
		    || (c >= 0x61 && c <= 0x6A)); /* [\/\\A-Za-j] */
}

static int valid_sym_table_uncompressed(char c)
{
	return (c == '/' || c == '\\' || (c >= 0x41 && c <= 0x5A)
		    || (c >= 0x30 && c <= 0x39)); /* [\/\\A-Z0-9] */
}

/*
 *	Fill the pbuf_t structure with a parsed position and
 *	symbol table & code. Also does range checking for lat/lng
 *	and pre-calculates cosf(lat) for range filters.
 */

static int pbuf_fill_pos(struct pbuf_t *pb, const float lat, const float lng, const char sym_table, const char sym_code)
{
	int bad = 0;
	/* symbol table and code */
	pb->symbol[0] = sym_table;
	pb->symbol[1] = sym_code;
	pb->symbol[2] = 0;
	
	/* Is it perhaps a weather report ? Allow symbol overlays, too. */
	if (sym_code == '_' && valid_sym_table_uncompressed(sym_table))
		pb->packettype |= T_WX;
	if (sym_code == '@' && valid_sym_table_uncompressed(sym_table))
		pb->packettype |= T_WX;	/* Hurricane */

	bad |= (lat < -89.9 && -0.0001 <= lng && lng <= 0.0001);
	bad |= (lat >  89.9 && -0.0001 <= lng && lng <= 0.0001);

	if (-0.0001 <= lat && lat <= 0.0001) {
	  bad |= ( -0.0001 <= lng && lng <= 0.0001);
	  bad |= ( -90.01  <= lng && lng <= -89.99);
	  bad |= (  89.99  <= lng && lng <=  90.01);
	}


	if (bad || lat < -90.0 || lat > 90.0 || lng < -180.0 || lng > 180.0) {
		DEBUG_LOG("\tposition out of range: lat %.5f lng %.5f", lat, lng);
		return 0; /* out of range */
	}
	
	DEBUG_LOG("\tposition ok: lat %.5f lng %.5f", lat, lng);

	/* Pre-calculations for A/R/F/M-filter tests */
	pb->lat     = filter_lat2rad(lat);  /* deg-to-radians */
	pb->cos_lat = cosf(pb->lat);        /* used in range filters */
	pb->lng     = filter_lon2rad(lng);  /* deg-to-radians */
	
	pb->flags |= F_HASPOS;	/* the packet has positional data */

	return 1;
}

/*
 *	Parse symbol from destination callsign
 */

static int get_symbol_from_dstcall_twochar(const char c1, const char c2, char *sym_table, char *sym_code)
{
	//DEBUG_LOG("\ttwochar %c %c", c1, c2);
	if (c1 == 'B') {
		if (c2 >= 'B' && c2 <= 'P') {
			*sym_table = '/';
			*sym_code = c2 - 'B' + '!';
			return 1;
		}
		return 0;
	}
	
	if (c1 == 'P') {
		if (c2 >= '0' && c2 <= '9') {
			*sym_table = '/';
			*sym_code = c2;
			return 1;
		}
		if (c2 >= 'A' && c2 <= 'Z') {
			*sym_table = '/';
			*sym_code = c2;
			return 1;
		}
		return 0;
	}
	
	if (c1 == 'M') {
		if (c2 >= 'R' && c2 <= 'X') {
			*sym_table = '/';
			*sym_code = c2 - 'R' + ':';
			return 1;
		}
		return 0;
	}
	
	if (c1 == 'H') {
		if (c2 >= 'S' && c2 <= 'X') {
			*sym_table = '/';
			*sym_code = c2 - 'S' + '[';
			return 1;
		}
		return 0;
	}
	
	if (c1 == 'L') {
		if (c2 >= 'A' && c2 <= 'Z') {
			*sym_table = '/';
			*sym_code = c2 - 'A' + 'a';
			return 1;
		}
		return 0;
	}
	
	if (c1 == 'J') {
		if (c2 >= '1' && c2 <= '4') {
			*sym_table = '/';
			*sym_code = c2 - '1' + '{';
			return 1;
		}
		return 0;
	}
	
	if (c1 == 'O') {
		if (c2 >= 'B' && c2 <= 'P') {
			*sym_table = '\\';
			*sym_code = c2 - 'B' + '!';
			return 1;
		}
		return 0;
	}
	
	if (c1 == 'A') {
		if (c2 >= '0' && c2 <= '9') {
			*sym_table = '\\';
			*sym_code = c2;
			return 1;
		}
		if (c2 >= 'A' && c2 <= 'Z') {
			*sym_table = '\\';
			*sym_code = c2;
			return 1;
		}
		return 0;
	}
	
	if (c1 == 'N') {
		if (c2 >= 'R' && c2 <= 'X') {
			*sym_table = '\\';
			*sym_code = c2 - 'R' + ':';
			return 1;
		}
		return 0;
	}
	
	if (c1 == 'D') {
		if (c2 >= 'S' && c2 <= 'X') {
			*sym_table = '\\';
			*sym_code = c2 - 'S' + '[';
			return 1;
		}
		return 0;
	}
	
	if (c1 == 'S') {
		if (c2 >= 'A' && c2 <= 'Z') {
			*sym_table = '\\';
			*sym_code = c2 - 'A' + 'a';
			return 1;
		}
		return 0;
	}
	
	if (c1 == 'Q') {
		if (c2 >= '1' && c2 <= '4') {
			*sym_table = '\\';
			*sym_code = c2 - '1' + '{';
			return 1;
		}
		return 0;
	}
	
	return 0;
}

static int get_symbol_from_dstcall(struct pbuf_t *pb, char *sym_table, char *sym_code)
{
	const char *d_start;
	char type;
	char overlay;
	int sublength;
	int numberid;
	
	/* check that the destination call exists and is of the right size for symbol */
	d_start = pb->srccall_end+1;
	if (pb->dstcall_end_or_ssid - d_start < 5)
		return 0; /* too short */
	
	/* length of the parsed string */
	sublength = pb->dstcall_end_or_ssid - d_start - 3;
	if (sublength > 3)
		sublength = 3;
	
	DEBUG_LOG("get_symbol_from_dstcall: %.*s (%d)", (int)(pb->dstcall_end_or_ssid - d_start), d_start, sublength);
	
	if (strncmp(d_start, "GPS", 3) != 0 && strncmp(d_start, "SPC", 3) != 0 && strncmp(d_start, "SYM", 3) != 0)
		return 0;
	
	// DEBUG_LOG("\ttesting %c %c %c", d_start[3], d_start[4], d_start[5]);
	if (!isalnum(d_start[3]) || !isalnum(d_start[4]))
		return 0;
	
	if (sublength == 3 && !isalnum(d_start[5]))
		return 0;
	
	type = d_start[3];
	
	if (sublength == 3) {
		if (type == 'C' || type == 'E') {
			if (!isdigit(d_start[4]))
				return 0;
			if (!isdigit(d_start[5]))
				return 0;
			numberid = (d_start[4] - 48) * 10 + (d_start[5] - 48);
			
			*sym_code = numberid + 32;
			if (type == 'C')
				*sym_table = '/';
			else
				*sym_table = '\\';
		
			DEBUG_LOG("\tnumeric symbol id in dstcall: %.*s: table %c code %c",
				(int)(pb->dstcall_end_or_ssid - d_start - 3), d_start + 3, *sym_table, *sym_code);
				
			return 1;
		} else {
			/* secondary symbol table, with overlay
			 * Check first that we really are in the secondary symbol table
			 */
			overlay = d_start[5];
			if ((type == 'O' || type == 'A' || type == 'N' ||
				type == 'D' || type == 'S' || type == 'Q')
				&& isalnum(overlay)) {
				return get_symbol_from_dstcall_twochar(d_start[3], d_start[4], sym_table, sym_code);
			}
			return 0;
		}
	} else {
		// primary or secondary table, no overlay
		return get_symbol_from_dstcall_twochar(d_start[3], d_start[4], sym_table, sym_code);
	}
	
	return 0;
}

/*
 *	Parse NMEA position packets.
 */

static int parse_aprs_nmea(struct pbuf_t *pb, const char *body, const char *body_end)
{
	float lat, lng;
	const char *latp, *lngp;
	int i, la, lo;
	char lac, loc;
	char sym_table = ' ', sym_code = ' ';
	
	// Parse symbol from destination callsign, first thing before any possible returns
	get_symbol_from_dstcall(pb, &sym_table, &sym_code);
	
	DEBUG_LOG("get_symbol_from_dstcall: %.*s => %c%c",
		 (int)(pb->dstcall_end_or_ssid - pb->srccall_end-1), pb->srccall_end+1, sym_table, sym_code);

	if (memcmp(body,"ULT",3) == 0) {
		/* Ah..  "$ULT..." - that is, Ultimeter 2000 weather instrument */
		pb->packettype |= T_WX;
		return 1;
	}
	
	lat  = lng  = 0.0;
	latp = lngp = NULL;
	
	/* NMEA sentences to understand:
	   $GPGGA  Global Positioning System Fix Data
	   $GPGLL  Geographic Position, Latitude/Longitude Data
	   $GPRMC  Remommended Minimum Specific GPS/Transit Data
	   $GPWPT  Way Point Location ?? (bug in APRS specs ?)
	   $GPWPL  Waypoint Load (not in APRS specs, but in NMEA specs)
	   $PNTS   Seen on APRS-IS, private sentense based on NMEA..
	   $xxTLL  Not seen on radio network, usually $RATLL - Target positions
	           reported by RAdar.
	 */
	 
	if (memcmp(body, "GPGGA,", 6) == 0) {
		/* GPGGA,175059,3347.4969,N,11805.7319,W,2,12,1.0,6.8,M,-32.1,M,,*7D
		//   v=1, looks fine
		// GPGGA,000000,5132.038,N,11310.221,W,1,09,0.8,940.0,M,-17.7,,
		//   v=1, timestamp odd, coords look fine
		// GPGGA,,,,,,0,00,,,,,,,*66
		//   v=0, invalid
		// GPGGA,121230,4518.7931,N,07322.3202,W,2,08,1.0,40.0,M,-32.4,M,,*46
		//   v=2, looks valid ?
		// GPGGA,193115.00,3302.50182,N,11651.22581,W,1,08,01.6,00465.90,M,-32.891,M,,*5F
		// $GPGGA,hhmmss.dd,xxmm.dddd,<N|S>,yyymm.dddd,<E|W>,v,
		//        ss,d.d,h.h,M,g.g,M,a.a,xxxx*hh<CR><LF>
		*/
		
		latp = body+6; // over the keyword
		while (latp < body_end && *latp != ',')
			latp++; // scan over the timestamp
		if (*latp == ',')
			latp++; // .. and into latitude.
		lngp = latp;
		while (lngp < body_end && *lngp != ',')
			lngp++;
		if (*lngp == ',')
			lngp++;
		if (*lngp != ',')
			lngp++;
		if (*lngp == ',')
			lngp++;
			
		/* latp, and lngp  point to start of latitude and longitude substrings
		// respectively.
		*/
	
	} else if (memcmp(body, "GPGLL,", 6) == 0) {
		/* $GPGLL,xxmm.dddd,<N|S>,yyymm.dddd,<E|W>,hhmmss.dd,S,M*hh<CR><LF>  */
		latp = body+6; // over the keyword
		lngp = latp;
		while (lngp < body_end && *lngp != ',') // over latitude
			lngp++;
		if (*lngp == ',')
			lngp++; // and lat designator
		if (*lngp != ',')
			lngp++; // and lat designator
		if (*lngp == ',')
			lngp++;
		/* latp, and lngp  point to start of latitude and longitude substrings
		// respectively
		*/
	} else if (memcmp(body, "GPRMC,", 6) == 0) {
		/* $GPRMC,hhmmss.dd,S,xxmm.dddd,<N|S>,yyymm.dddd,<E|W>,s.s,h.h,ddmmyy,d.d, <E|W>,M*hh<CR><LF>
		// ,S, = Status:  'A' = Valid, 'V' = Invalid
		// 
		// GPRMC,175050,A,4117.8935,N,10535.0871,W,0.0,324.3,100208,10.0,E,A*3B
		// GPRMC,000000,V,0000.0000,0,00000.0000,0,000,000,000000,,*01/It wasn't me :)
		//    invalid..
		// GPRMC,000043,V,4411.7761,N,07927.0448,W,0.000,0.0,290697,10.7,W*57
		// GPRMC,003803,A,3347.1727,N,11812.7184,W,000.0,000.0,140208,013.7,E*67
		// GPRMC,050058,A,4609.1143,N,12258.8184,W,0.000,0.0,100208,18.0,E*5B
		*/
		
		latp = body+6; // over the keyword
		while (latp < body_end && *latp != ',')
			latp++; // scan over the timestamp
		if (*latp == ',')
			latp++; // .. and into VALIDITY
		if (*latp != 'A' && *latp != 'V')
			return 0; // INVALID !
		if (*latp != ',')
			latp++;
		if (*latp == ',')
			latp++;
		
		/* now it points to latitude substring */
		lngp = latp;
		while (lngp < body_end && *lngp != ',')
			lngp++;
		
		if (*lngp == ',')
			lngp++;
		if (*lngp != ',')
			lngp++;
		if (*lngp == ',')
			lngp++;
		
		/* latp, and lngp  point to start of latitude and longitude substrings
		// respectively.
		*/
		
	} else if (memcmp(body, "GPWPL,", 6) == 0) {
		/* $GPWPL,4610.586,N,00607.754,E,4*70
		// $GPWPL,4610.452,N,00607.759,E,5*74
		*/
		latp = body+6;
		
	} else if (memcmp(body, "PNTS,1,", 7) == 0) { /* PNTS version 1 */
		/* $PNTS,1,0,11,01,2002,231932,3539.687,N,13944.480,E,0,000,5,Roppongi UID RELAY,000,1*35
		// $PNTS,1,0,14,01,2007,131449,3535.182,N,13941.200,E,0,0.0,6,Oota-Ku KissUIDigi,000,1*1D
		// $PNTS,1,0,17,02,2008,120824,3117.165,N,13036.481,E,49,059,1,Kagoshima,000,1*71
		// $PNTS,1,0,17,02,2008,120948,3504.283,N,13657.933,E,00,000.0,6,,000,1*36
		// 
		// From Alinco EJ-41U Terminal Node Controller manual:
		// 
		// 5-4-7 $PNTS
		// This is a private-sentence based on NMEA-0183.  The data contains date,
		// time, latitude, longitude, moving speed, direction, altitude plus a short
		// message, group codes, and icon numbers. The EJ-41U does not analyze this
		// format but can re-structure it.
		// The data contains the following information:
		//  l $PNTS Starts the $PNTS sentence
		//  l version
		//  l the registered information. [0]=normal geographical location data.
		//    This is the only data EJ-41U can re-structure. [s]=Initial position
		//    for the course setting [E]=ending position for the course setting
		//    [1]=the course data between initial and ending [P]=the check point
		//    registration [A]=check data when the automatic position transmission
		//    is set OFF [R]=check data when the course data or check point data is
		//    received.
		//  l dd,mm,yyyy,hhmmss: Date and time indication.
		//  l Latitude in DMD followed by N or S
		//  l Longitude in DMD followed by E or W
		//  l Direction: Shown with the number 360 degrees divided by 64.
		//    00 stands for true north, 16 for east. Speed in Km/h
		//  l One of 15 characters [0] to [9], [A] to [E].
		//    NTSMRK command determines this character when EJ-41U is used.
		//  l A short message up to 20 bites. Use NTSMSG command to determine this message.
		//  l A group code: 3 letters with a combination of [0] to [9], [A] to [Z].
		//    Use NTSGRP command to determine.
		//  l Status: [1] for usable information, [0] for non-usable information.
		//  l *hh<CR><LF> the check-sum and end of PNTS sentence.
		*/

		if (body+55 > body_end) return 0; /* Too short.. */
		latp = body+7; /* Over the keyword */
		/* Accept any registered information code */
		if (*latp++ == ',') return 0;
		if (*latp++ != ',') return 0;
		/* Scan over date+time info */
		while (*latp != ',' && latp <= body_end) ++latp;
		if (*latp == ',') ++latp;
		while (*latp != ',' && latp <= body_end) ++latp;
		if (*latp == ',') ++latp;
		while (*latp != ',' && latp <= body_end) ++latp;
		if (*latp == ',') ++latp;
		while (*latp != ',' && latp <= body_end) ++latp;
		if (*latp == ',') ++latp;
		/* now it points to latitude substring */
		lngp = latp;
		while (lngp < body_end && *lngp != ',')
			lngp++;
		
		if (*lngp == ',')
			lngp++;
		if (*lngp != ',')
			lngp++;
		if (*lngp == ',')
			lngp++;
		
		/* latp, and lngp  point to start of latitude and longitude substrings
		// respectively.
		*/
#if 1
	} else if (memcmp(body, "GPGSA,", 6) == 0 ||
		   memcmp(body, "GPVTG,", 6) == 0 ||
		   memcmp(body, "GPGSV,", 6) == 0) {
		/* Recognized but ignored */
		return 1;
#endif
	}
	
	if (!latp || !lngp) {
		hlog_packet(LOG_DEBUG, pb->data, pb->packet_len-2, "Unknown NMEA: ");
		return 0; /* Well..  Not NMEA frame */
	}

	// DEBUG_LOG("NMEA parsing: %.*s", (int)(body_end - body), body);
	// DEBUG_LOG("     lat=%.10s   lng=%.10s", latp, lngp);

	i = sscanf(latp, "%2d%f,%c,", &la, &lat, &lac);
	if (i != 3)
		return 0; // parse failure
	
	i = sscanf(lngp, "%3d%f,%c,", &lo, &lng, &loc);
	if (i != 3)
		return 0; // parse failure
	
	if (lac != 'N' && lac != 'S' && lac != 'n' && lac != 's')
		return 0; // bad indicator value
	if (loc != 'E' && loc != 'W' && loc != 'e' && loc != 'w')
		return 0; // bad indicator value
		
	// DEBUG_LOG("   lat: %c %2d %7.4f   lng: %c %2d %7.4f",
	//                 lac, la, lat, loc, lo, lng);

	lat = (float)la + lat/60.0;
	lng = (float)lo + lng/60.0;
	
	if (lac == 'S' || lac == 's')
		lat = -lat;
	if (loc == 'W' || loc == 'w')
		lng = -lng;
	
	pb->packettype |= T_POSITION;
	
	return pbuf_fill_pos(pb, lat, lng, sym_table, sym_code);
}

static int parse_aprs_telem(struct pbuf_t *pb, const char *body, const char *body_end)
{
	// float lat = 0.0, lng = 0.0;

	DEBUG_LOG("parse_aprs_telem");

	//pbuf_fill_pos(pb, lat, lng, 0, 0);
	return 0;
}

/*
 *	Parse a MIC-E position packet
 *
 *	APRS PROTOCOL REFERENCE 1.0.1 Chapter 10, page 42 (52 in PDF)
 */

static int parse_aprs_mice(struct pbuf_t *pb, const unsigned char *body, const unsigned char *body_end)
{
	float lat = 0.0, lng = 0.0;
	unsigned int lat_deg = 0, lat_min = 0, lat_min_frag = 0, lng_deg = 0, lng_min = 0, lng_min_frag = 0;
	const char *d_start;
	char dstcall[7];
	char *p;
	char sym_table, sym_code;
	int posambiguity = 0;
	int i;
	
	DEBUG_LOG("parse_aprs_mice: %.*s", pb->packet_len-2, pb->data);
	
	/* check packet length */
	if (body_end - body < 8)
		return 0;
	
	/* check that the destination call exists and is of the right size for mic-e */
	d_start = pb->srccall_end+1;
	if (pb->dstcall_end_or_ssid - d_start != 6)
		return 0; /* eh...? */
	
	/* validate destination call:
	 * A-K characters are not used in the last 3 characters
	 * and MNO are never used
	 */
	
	for (i = 0; i < 3; i++)
		if (!((d_start[i] >= '0' && d_start[i] <= '9')
			|| (d_start[i] >= 'A' && d_start[i] <= 'L')
			|| (d_start[i] >= 'P' && d_start[i] <= 'Z')))
				return 0;
	
	for (i = 3; i < 6; i++)
		if (!((d_start[i] >= '0' && d_start[i] <= '9')
			|| (d_start[i] == 'L')
			|| (d_start[i] >= 'P' && d_start[i] <= 'Z')))
				return 0;
	
	DEBUG_LOG("\tpassed dstcall format check");
	
	/* validate information field (longitude, course, speed and
	 * symbol table and code are checked). Not bullet proof..
	 *
	 *   0          1          23            4          5          6              7
	 * /^[\x26-\x7f][\x26-\x61][\x1c-\x7f]{2}[\x1c-\x7d][\x1c-\x7f][\x21-\x7b\x7d][\/\\A-Z0-9]/
	 */
	if (body[0] < 0x26 || (unsigned char)body[0] > 0x7f) return 0;
	if (body[1] < 0x26 || (unsigned char)body[1] > 0x61) return 0;
	if (body[2] < 0x1c || (unsigned char)body[2] > 0x7f) return 0;
	if (body[3] < 0x1c || (unsigned char)body[3] > 0x7f) return 0;
	if (body[4] < 0x1c || (unsigned char)body[4] > 0x7d) return 0;
	if (body[5] < 0x1c || (unsigned char)body[5] > 0x7f) return 0;
	if ((body[6] < 0x21 || (unsigned char)body[6] > 0x7b)
		&& (unsigned char)body[6] != 0x7d) return 0;
	if (!valid_sym_table_uncompressed(body[7])) return 0;
	
	DEBUG_LOG("\tpassed info format check");
	
	/* make a local copy, we're going to modify it */
	strncpy(dstcall, d_start, 6);
	dstcall[6] = 0;
	
	/* First do the destination callsign
	 * (latitude, message bits, N/S and W/E indicators and long. offset)
	 *
	 * Translate the characters to get the latitude
	 */
	 
	//fprintf(stderr, "\tuntranslated dstcall: %s\n", dstcall);
	for (p = dstcall; *p; p++) {
		if (*p >= 'A' && *p <= 'J')
			*p -= 'A' - '0';
		else if (*p >= 'P' && *p <= 'Y')
			*p -= 'P' - '0';
		else if (*p == 'K' || *p == 'L' || *p == 'Z')
			*p = '_';
	}
	//fprintf(stderr, "\ttranslated dstcall: %s\n", dstcall);
	
	/* position ambiquity is going to get ignored now, it's not needed in this application. */
	if (dstcall[5] == '_') { dstcall[5] = '5'; posambiguity = 1; }
	if (dstcall[4] == '_') { dstcall[4] = '5'; posambiguity = 2; }
	if (dstcall[3] == '_') { dstcall[3] = '5'; posambiguity = 3; }
	if (dstcall[2] == '_') { dstcall[2] = '3'; posambiguity = 4; }
	if (dstcall[1] == '_' || dstcall[0] == '_') { return 0; } /* cannot use posamb here */
	
	/* convert to degrees, minutes and decimal degrees, and then to a float lat */
	if (sscanf(dstcall, "%2u%2u%2u",
	    &lat_deg, &lat_min, &lat_min_frag) != 3) {
		DEBUG_LOG("\tsscanf failed");
		return 0;
	}
	lat = (float)lat_deg + (float)lat_min / 60.0 + (float)lat_min_frag / 6000.0;
	
	/* check the north/south direction and correct the latitude if necessary */
	if (d_start[3] <= 0x4c)
		lat = 0 - lat;
	
	/* Decode the longitude, the first three bytes of the body after the data
	 * type indicator. First longitude degrees, remember the longitude offset.
	 */
	lng_deg = body[0] - 28;
	if (d_start[4] >= 0x50)
		lng_deg += 100;
	if (lng_deg >= 180 && lng_deg <= 189)
		lng_deg -= 80;
	else if (lng_deg >= 190 && lng_deg <= 199)
		lng_deg -= 190;
	
	/* Decode the longitude minutes */
	lng_min = body[1] - 28;
	if (lng_min >= 60)
		lng_min -= 60;
		
	/* ... and minute decimals */
	lng_min_frag = body[2] - 28;
	
	/* apply position ambiguity to longitude */
	switch (posambiguity) {
	case 0:
		/* use everything */
		lng = (float)lng_deg + (float)lng_min / 60.0
			+ (float)lng_min_frag / 6000.0;
		break;
	case 1:
		/* ignore last number of lng_min_frag */
		lng = (float)lng_deg + (float)lng_min / 60.0
			+ (float)(lng_min_frag - lng_min_frag % 10 + 5) / 6000.0;
		break;
	case 2:
		/* ignore lng_min_frag */
		lng = (float)lng_deg + ((float)lng_min + 0.5) / 60.0;
		break;
	case 3:
		/* ignore lng_min_frag and last number of lng_min */
		lng = (float)lng_deg + (float)(lng_min - lng_min % 10 + 5) / 60.0;
		break;
	case 4:
		/* minute is unused -> add 0.5 degrees to longitude */
		lng = (float)lng_deg + 0.5;
		break;
	default:
		return 0;
	}
	
	/* check the longitude E/W sign */
	if (d_start[5] >= 0x50)
		lng = 0 - lng;
	
	/* save the symbol table and code */
	sym_code = body[6];
	sym_table = body[7];
	
	/* ok, we're done */
	/*
	fprintf(stderr, "\tlat %u %u.%u (%.4f) lng %u %u.%u (%.4f)\n",
	 	lat_deg, lat_min, lat_min_frag, lat,
	 	lng_deg, lng_min, lng_min_frag, lng);
	fprintf(stderr, "\tsym '%c' '%c'\n", sym_table, sym_code);
	*/
	
	return pbuf_fill_pos(pb, lat, lng, sym_table, sym_code);
}

/*
 *	Parse a compressed APRS position packet
 *
 *	APRS PROTOCOL REFERENCE 1.0.1 Chapter 9, page 36 (46 in PDF)
 */

static int parse_aprs_compressed(struct pbuf_t *pb, const char *body, const char *body_end)
{
	char sym_table, sym_code;
	int i;
	int lat1, lat2, lat3, lat4, lng1, lng2, lng3, lng4;
	double lat = 0.0, lng = 0.0;
	
	DEBUG_LOG("parse_aprs_compressed");
	
	/* A compressed position is always 13 characters long.
	 * Make sure we get at least 13 characters and that they are ok.
	 * Also check the allowed base-91 characters at the same time.
	 */ 
	
	if (body_end - body < 13)
		return 0; /* too short. */
	
	sym_table = body[0]; /* has been validated before entering this function */
	sym_code = body[9];
	
	/* base-91 check */
	for (i = 1; i <= 8; i++)
		if (body[i] < 0x21 || body[i] > 0x7b)
			return 0;
	
	// fprintf(stderr, "\tpassed length and format checks, sym %c%c\n", sym_table, sym_code);
	
	/* decode */
	lat1 = (body[1] - 33);
	lat2 = (body[2] - 33);
	lat3 = (body[3] - 33);
	lat4 = (body[4] - 33);

	lat1 = ((((lat1 * 91) + lat2) * 91) + lat3) * 91 + lat4;

	lng1 = (body[5] - 33);
	lng2 = (body[6] - 33);
	lng3 = (body[7] - 33);
	lng4 = (body[8] - 33);

	lng1 = ((((lng1 * 91) + lng2) * 91) + lng3) * 91 + lng4;

	/* calculate latitude and longitude */

	lat =   90.0F - ((float)(lat1) / 380926.0F);
	lng = -180.0F + ((float)(lng1) / 190463.0F);
	
	return pbuf_fill_pos(pb, lat, lng, sym_table, sym_code);
}

/*
 *	Parse an uncompressed "normal" APRS packet
 *
 *	APRS PROTOCOL REFERENCE 1.0.1 Chapter 8, page 32 (42 in PDF)
 */

static int parse_aprs_uncompressed(struct pbuf_t *pb, const char *body, const char *body_end)
{
	char posbuf[20];
	unsigned int lat_deg = 0, lat_min = 0, lat_min_frag = 0, lng_deg = 0, lng_min = 0, lng_min_frag = 0;
	float lat, lng;
	char lat_hemi, lng_hemi;
	char sym_table, sym_code;
	int issouth = 0;
	int iswest = 0;
	
	DEBUG_LOG("parse_aprs_uncompressed");
	
	if (body_end - body < 19) {
		DEBUG_LOG("\ttoo short");
		return 0;
	}
	
	/* make a local copy, so we can overwrite it at will. */
	memcpy(posbuf, body, 19);
	posbuf[19] = 0;
	// fprintf(stderr, "\tposbuf: %s\n", posbuf);
	
	/* position ambiquity is going to get ignored now, it's not needed in this application. */
	/* lat */
	if (posbuf[2] == ' ') posbuf[2] = '3';
	if (posbuf[3] == ' ') posbuf[3] = '5';
	if (posbuf[5] == ' ') posbuf[5] = '5';
	if (posbuf[6] == ' ') posbuf[6] = '5';
	/* lng */
	if (posbuf[12] == ' ') posbuf[12] = '3';
	if (posbuf[13] == ' ') posbuf[13] = '5';
	if (posbuf[15] == ' ') posbuf[15] = '5';
	if (posbuf[16] == ' ') posbuf[16] = '5';
	
	// fprintf(stderr, "\tafter filling amb: %s\n", posbuf);
	/* 3210.70N/13132.15E# */
	if (sscanf(posbuf, "%2u%2u.%2u%c%c%3u%2u.%2u%c%c",
	    &lat_deg, &lat_min, &lat_min_frag, &lat_hemi, &sym_table,
	    &lng_deg, &lng_min, &lng_min_frag, &lng_hemi, &sym_code) != 10) {
		DEBUG_LOG("\tsscanf failed");
		return 0;
	}
	
	if (!valid_sym_table_uncompressed(sym_table))
		sym_table = 0;
	
	if (lat_hemi == 'S' || lat_hemi == 's')
		issouth = 1;
	else if (lat_hemi != 'N' && lat_hemi != 'n')
		return 0; /* neither north or south? bail out... */
	
	if (lng_hemi == 'W' || lng_hemi == 'w')
		iswest = 1;
	else if (lng_hemi != 'E' && lng_hemi != 'e')
		return 0; /* neither west or east? bail out ... */
	
	if (lat_deg > 89 || lng_deg > 179)
		return 0; /* too large values for lat/lng degrees */
	
	lat = (float)lat_deg + (float)lat_min / 60.0 + (float)lat_min_frag / 6000.0;
	lng = (float)lng_deg + (float)lng_min / 60.0 + (float)lng_min_frag / 6000.0;
	
	/* Finally apply south/west indicators */
	if (issouth)
		lat = 0.0 - lat;
	if (iswest)
		lng = 0.0 - lng;
	
	// fprintf(stderr, "\tlat %u %u.%u %c (%.3f) lng %u %u.%u %c (%.3f)\n",
	// 	lat_deg, lat_min, lat_min_frag, (int)lat_hemi, lat,
	// 	lng_deg, lng_min, lng_min_frag, (int)lng_hemi, lng);
	// fprintf(stderr, "\tsym '%c' '%c'\n", sym_table, sym_code);

	return pbuf_fill_pos(pb, lat, lng, sym_table, sym_code);
}

/*
 *	Parse an APRS object 
 *
 *	APRS PROTOCOL REFERENCE 1.0.1 Chapter 11, page 58 (68 in PDF)
 */

static int parse_aprs_object(struct pbuf_t *pb, const char *body, const char *body_end)
{
	int i;
	int namelen = -1;
	
	pb->packettype |= T_OBJECT;
	
	DEBUG_LOG("parse_aprs_object");
	
	/* check that the object name ends with either * or _ */
	if (body[9] != '*' && body[9] != '_') {
		DEBUG_LOG("\tinvalid object kill character");
		return 0;
	}
	
	/* check that the timestamp ends with one of the valid timestamp type IDs */
	char tz_end = body[16];
	if (tz_end != 'z' && tz_end != 'h' && tz_end != '/') {
		DEBUG_LOG("\tinvalid object timestamp type character");
		return 0;
	}
	
	/* check object's name - scan for non-printable characters and the last
	 * non-space character
	 */
	for (i = 0; i < 9; i++) {
		if (body[i] < 0x20 || body[i] > 0x7e) {
			DEBUG_LOG("\tobject name has unprintable characters");
			return 0; /* non-printable */
		}
		if (body[i] != ' ')
			namelen = i;
	}
	
	if (namelen < 0) {
		DEBUG_LOG("\tobject has empty name");
		return 0;
	}
	
	pb->srcname = body;
	pb->srcname_len = namelen+1;
	
	DEBUG_LOG("object name: '%.*s'", pb->srcname_len, pb->srcname);
	
	/* Forward the location parsing onwards */
	if (valid_sym_table_compressed(body[17]))
		return parse_aprs_compressed(pb, body + 17, body_end);
	
	if (body[17] >= '0' && body[17] <= '9')
		return parse_aprs_uncompressed(pb, body + 17, body_end);
	
	DEBUG_LOG("no valid position in object");
	
	return 0;
}

/*
 *	Parse an APRS item
 *
 *	APRS PROTOCOL REFERENCE 1.0.1 Chapter 11, page 59 (69 in PDF)
 */

static int parse_aprs_item(struct pbuf_t *pb, const char *body, const char *body_end)
{
	int i;
	
	pb->packettype |= T_ITEM;
	
	DEBUG_LOG("parse_aprs_item");
	
	/* check item's name - scan for non-printable characters and the
	 * ending character ! or _
	 */
	for (i = 0; i < 9 && body[i] != '!' && body[i] != '_'; i++) {
		if (body[i] < 0x20 || body[i] > 0x7e) {
			DEBUG_LOG("\titem name has unprintable characters");
			return 0; /* non-printable */
		}
	}
	
	if (body[i] != '!' && body[i] != '_') {
		DEBUG_LOG("\titem name ends with neither ! or _");
		return 0;
	}
	
	if (i < 3 || i > 9) {
		DEBUG_LOG("\titem name has invalid length");
		return 0;
	}
	
	pb->srcname = body;
	pb->srcname_len = i;
	
	//fprintf(stderr, "\titem name: '%.*s'\n", pb->srcname_len, pb->srcname);
	
	/* Forward the location parsing onwards */
	i++;
	if (valid_sym_table_compressed(body[i]))
		return parse_aprs_compressed(pb, body + i, body_end);
	
	if (body[i] >= '0' && body[i] <= '9')
		return parse_aprs_uncompressed(pb, body + i, body_end);
	
	DEBUG_LOG("\tno valid position in item");
	
	return 0;
}

/* forward declaration to allow recursive calls */
static int parse_aprs_body(struct pbuf_t *pb, const char *info_start);

/*
 *	Parse a 3rd-party packet.
 *	Requires the } > : sequence from src>dst,network,gate: to
 *	detect a packet as a 3rd-party packet. If the sequence is not found,
 *	returns 0 for no match (but do not drop packet).
 *	If the sequence is found, require the packet to match the 3rd-party
 *	packet spec from APRS101.PDF, and validate callsigns too.
 */
 
static int parse_aprs_3rdparty(struct pbuf_t *pb, const char *info_start)
{
	const char *body;
	const char *src_end;
	const char *s;
	const char *path_start;
	const char *dstcall_end;
	int pathlen;
	
	/* ignore the CRLF in the end of the body */
	s = info_start + 1;
	
	/* find the end of the third-party inner header */
	body = memchr(s, ':', pb->packet_len - 2 - (s-pb->data));
	
	/* if not found, bail out */
	if (!body)
		return 0;
	
	pathlen = body - s;
	
	/* look for the '>' */
	src_end = memchr(s, '>', pathlen < CALLSIGNLEN_MAX+1 ? pathlen : CALLSIGNLEN_MAX+1);
	if (!src_end)
		return 0;	// No ">" in packet start..
	
	path_start = src_end+1;
	if (path_start >= body)	// We're already at the path end
		return INERR_INV_3RD_PARTY;
	
	if (check_invalid_src_dst(s, src_end - s) != 0)
		return INERR_INV_SRCCALL; /* invalid or too long for source callsign */
	
	dstcall_end = path_start;
	while (dstcall_end < body && *dstcall_end != ',' && *dstcall_end != ':')
		dstcall_end++;
	
	if (check_invalid_src_dst(path_start, dstcall_end - path_start))
		return INERR_INV_DSTCALL; /* invalid or too long for destination callsign */
	
	/* check if there are invalid callsigns in the digipeater path before Q,
	 * require at least two elements to be present (network ID, gateway callsign)
	 */
	if (check_path_calls(dstcall_end, body) < 2)
		return INERR_INV_3RD_PARTY;
	
	/* Ok, fill "name" parameter in packet with the 3rd-party packet
	 * srccall, so that filtering can match against it. This will be
	 * overwritten by object/item names.
	 */
	pb->srcname = s;
	pb->srcname_len = src_end - s;
	
	/* for now, just parse the inner packet content to learn it's type
	 * and coordinates, etc
	 */	
	return parse_aprs_body(pb, body+1);
}

/*
 *	Parse APRS message slightly (only as much as is necessary for packet forwarding)
 */

static const char *disallow_msg_recipients[] = {
	/* old aprsd status messages:
	 * W5xx>JAVA,qAU,WB5AOH::javaMSG  :Foo_bar Linux APRS Server: 192.168.10.55 connected 2 users online.
	 */
	"javaMSG", /* old aprsd */
	"JAVATITLE", /* old aprsd */
	"JAVATITL2", /* old aprsd */
	"USERLIST", /* old aprsd */
	/* Status messages from APRS+SA, blocked in javap:
	 * OK1xx>APRS,qAR,OK1Dxxx-1::KIPSS    :KipSS Login:OK1xxx-1{2
	 * OK1xx>ID,WIDE,qAR,OK1xxx-1::INFO     :KipSS Node QRT
	 */
	"KIPSS",
	NULL
};

static int preparse_aprs_message(struct pbuf_t *pb, const char *body, int body_len)
{
	// quick and loose way to identify NWS and SKYWARN messages
	// they do apparently originate from "WXSRV", but that is not
	// guaranteed thing...
	if (memcmp(body,"NWS-",4) == 0) // as seen on specification
		pb->packettype |= T_NWS;
	if (memcmp(body,"NWS_",4) == 0) // as seen on data
		pb->packettype |= T_NWS;
	if (memcmp(body,"SKY",3) == 0)  // as seen on specification
		pb->packettype |= T_NWS;
	
	// Is it perhaps TELEMETRY related "message" ?
	if ( body[9] == ':' && body_len >= 10 + 6 &&
	    ( memcmp( body+10, "PARM.", 5 ) == 0 ||
		memcmp( body+10, "UNIT.", 5 ) == 0 ||
		memcmp( body+10, "EQNS.", 5 ) == 0 ||
		memcmp( body+10, "BITS.", 5 ) == 0 )) {
			pb->packettype &= ~T_MESSAGE;
			pb->packettype |= T_TELEMETRY;
			// Fall through to recipient location lookup
	}
	
	// Or perhaps a DIRECTED QUERY ?
	/* It might not be a bright idea to mark all messages starting with ?
	 * queries instead of messages and making them NOT match the
	 * filter message.
	 * ALSO: General (non-directed) queries are DROPPED by aprsc.
	 * Do not mark DIRECTED QUERIES as queries - we don't want to drop them.
	if (body[9] == ':' && body[10] == '?') {
		pb->packettype &= ~T_MESSAGE;
		pb->packettype |=  T_QUERY;
		// Fall through to recipient location lookup
	}
	*/
	
	/* Collect message recipient */
	int i;
	
	for (i = 0; i < CALLSIGNLEN_MAX; ++i) {
		// the recipient address is space padded
		// to 9 chars, while our historydb is not.
		if (body[i] == ' ' || body[i] == ':' || body[i] == 0)
			break;
	}
	
	pb->dstname = body;
	pb->dstname_len = i;
	
	if (check_call_match(disallow_msg_recipients, body, i))
		return INERR_DIS_MSG_DST;
	
	return 0;
}

/*
 *	Parse the body of an APRS packet
 */
 
static int parse_aprs_body(struct pbuf_t *pb, const char *info_start)
{
	char packettype, poschar;
	int paclen;
	const char *body;
	const char *body_end;
	const char *pos_start;

	/* the following parsing logic has been translated from Ham::APRS::FAP
	 * Perl module to C
	 */
	
	/* length of the info field: length of the packet - length of header - CRLF */
	paclen = pb->packet_len - (pb->info_start - pb->data) - 2;
	if (paclen < 1) return 0; /* Empty frame */

	/* Check the first character of the packet and determine the packet type */
	packettype = *info_start;
	
	/* failed parsing */
	// fprintf(stderr, "parse_aprs (%d):\n", paclen);
	// fwrite(info_start, paclen, 1, stderr);
	// fprintf(stderr, "\n");
	
	/* body is right after the packet type character */
	body = info_start + 1;
	/* ignore the CRLF in the end of the body */
	body_end = pb->data + pb->packet_len - 2;
	
	switch (packettype) {
	/* the following are obsolete mic-e types: 0x1c 0x1d 
	 * case 0x1c:
	 * case 0x1d:
	 */
	case 0x27: /* ' */
	case 0x60: /* ` */
		/* could be mic-e, minimum body length 9 chars */
		if (paclen >= 9) {
			pb->packettype |= T_POSITION;
			return parse_aprs_mice(pb, (unsigned char *)body, (unsigned char *)body_end);
		}
		return 0;

	/* normal aprs plaintext packet types: !=/@
	 * (! might also be a wx packet, so we check for it first and then fall through to
	 * the position packet code)
	 */
	case '!':
		if (info_start[1] == '!') { /* Ultimeter 2000 */
			pb->packettype |= T_WX;
			return 0;
		}
		/* intentionally missing break here */
	case '=':
	case '/':
	case '@':
		/* check that we won't run over right away */
		if (body_end - body < 10)
			return 0;
		/* Normal or compressed location packet, with or without
		 * timestamp, with or without messaging capability
		 *
		 * ! and / have messaging, / and @ have a prepended timestamp
		 */
		pb->packettype |= T_POSITION;
		if (packettype == '/' || packettype == '@') {
			/* With a prepended timestamp, jump over it. */
			body += 7;
		}
		poschar = *body;
		if (valid_sym_table_compressed(poschar)) { /* [\/\\A-Za-j] */
		    	/* compressed position packet */
			if (body_end - body >= 13)
				return parse_aprs_compressed(pb, body, body_end);
			
		} else if (poschar >= 0x30 && poschar <= 0x39) { /* [0-9] */
			/* normal uncompressed position */
			if (body_end - body >= 19)
				return parse_aprs_uncompressed(pb, body, body_end);
		}
		return 0;

	case '$':
		if (body_end - body > 10) {
			// Is it OK to declare it as position packet ?
			return parse_aprs_nmea(pb, body, body_end);
		}
		return 0;

	case ':':
		if (paclen >= 11) {
			pb->packettype |= T_MESSAGE;
			return preparse_aprs_message(pb, body, paclen-1);
		}
		return 0;

	case ';':
		if (body_end - body > 29)
			return parse_aprs_object(pb, body, body_end);
		return 0;

	case '>':
		pb->packettype |= T_STATUS;
		return 0;

	case '<':
		pb->packettype |= T_STATCAPA;
		return 0;

	case '?':
		pb->packettype |= T_QUERY;
		return 0;

	case ')':
		if (body_end - body > 18) {
			return parse_aprs_item(pb, body, body_end);
		}
		return 0;
	
	case 'D':
		/* we drop DX cluster packets, they start with "DX de " */
		if (strncmp(body, "X de ", 5) == 0)
			return INERR_DIS_DX;
		break;

	case 'T':
		if (body_end - body > 18) {
			pb->packettype |= T_TELEMETRY;
			return parse_aprs_telem(pb, body, body_end);
		}
		return 0;

	case '#': /* Peet Bros U-II Weather Station */
	case '*': /* Peet Bros U-I  Weather Station */
	case '_': /* Weather report without position */
		pb->packettype |= T_WX;
		return 0;

	case '{':
		pb->packettype |= T_USERDEF;
		return 0;
        
        case '}':
		pb->packettype |= T_3RDPARTY;
		return parse_aprs_3rdparty(pb, info_start);

	default:
		break;
	}

	/* When all else fails, try to look for a !-position that can
	 * occur anywhere within the 40 first characters according
	 * to the spec.  (X1J TNC digipeater bugs...)
	 */
	pos_start = memchr(body, '!', body_end - body);
	if ((pos_start) && pos_start - body <= 39) {
		poschar = *pos_start;
		if (valid_sym_table_compressed(poschar)) { /* [\/\\A-Za-j] */
		    	/* compressed position packet */
		    	if (body_end - pos_start >= 13)
		    		return parse_aprs_compressed(pb, pos_start, body_end);
			return 0;
		} else if (poschar >= 0x30 && poschar <= 0x39) { /* [0-9] */
			/* normal uncompressed position */
			if (body_end - pos_start >= 19)
				return parse_aprs_uncompressed(pb, pos_start, body_end);
			return 0;
		}
	}
	
	return 0;
}



/*
 *	Try to parse an APRS packet.
 *	Returns 1 if position was parsed successfully,
 *	0 if parsing failed, < 0 if packet should be dropped.
 *
 *	Does also front-end part of the output filter's
 *	packet type classification job.
 *
 * TODO: Recognize TELEM packets in !/=@ packets too!
 *
 */

int parse_aprs(struct pbuf_t *pb)
{
	if (!pb->info_start)
		return 0;

	pb->packettype = T_ALL;

	/* T_CW detection - CW\d+, DW\d+, EW\d+ callsigns
	 * only used for our custom t/c CWOP filter which nobody uses
	 */
	const char *d  = pb->data;
	if (d[1] == 'W' && (d[0] >= 'C' && d[0] <= 'E')) {
		int i;
		for (i = 2; i < pb->packet_len; i++) {
			if (d[i] < '0' || d[i] > '9')
				break;
		}
		if (d[i] == '>')
			pb->packettype |= T_CWOP;
	}
	
	return parse_aprs_body(pb, pb->info_start);
}

/*
 *	Parse an aprs text message (optional, only done to messages addressed to
 *	SERVER
 */

int parse_aprs_message(struct pbuf_t *pb, struct aprs_message_t *am)
{
	const char *p;
	
	memset(am, 0, sizeof(*am));
	
	if (!(pb->packettype & T_MESSAGE))
		return -1;
		
	if (pb->info_start[10] != ':')
		return -2;
	
	am->body = pb->info_start + 11;
	/* -2 for the CRLF already in place */
	am->body_len = pb->packet_len - 2 - (pb->info_start - pb->data);
	
	/* search for { looking backwards from the end of the packet,
	 * it separates the msgid
	 */
	p = am->body + am->body_len - 1;
	while (p > am->body && *p != '{')
		p--;
	
	if (*p == '{') {
		am->msgid = p+1;
		am->msgid_len = pb->packet_len - 2 - (am->msgid - pb->data);
		am->body_len = p - am->body;
	}
	
	/* check if this is an ACK */
	if ((!am->msgid_len) && am->body_len > 3
	    && am->body[0] == 'a' && am->body[1] == 'c' && am->body[2] == 'k') {
		am->is_ack = 1;
		am->msgid = am->body + 3;
		am->msgid_len = am->body_len - 3;
		am->body_len = 0;
		return 0;
	}
	
	return 0;
}
