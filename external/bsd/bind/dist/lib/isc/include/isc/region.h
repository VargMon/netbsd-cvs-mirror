/*	$NetBSD: region.h,v 1.3.6.2 2014/12/25 17:54:29 msaitoh Exp $	*/

/*
 * Copyright (C) 2004-2007, 2013  Internet Systems Consortium, Inc. ("ISC")
 * Copyright (C) 1998-2002  Internet Software Consortium.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/* Id: region.h,v 1.25 2007/06/19 23:47:18 tbox Exp  */

#ifndef ISC_REGION_H
#define ISC_REGION_H 1

/*! \file isc/region.h */

#include <isc/types.h>
#include <isc/lang.h>

struct isc_region {
	unsigned char *	base;
	unsigned int	length;
};

struct isc_textregion {
	char *		base;
	unsigned int	length;
};

/* XXXDCL questionable ... bears discussion.  we have been putting off
 * discussing the region api.
 */
struct isc_constregion {
	const void *	base;
	unsigned int	length;
};

struct isc_consttextregion {
	const char *	base;
	unsigned int	length;
};

/*@{*/
/*!
 * The region structure is not opaque, and is usually directly manipulated.
 * Some macros are defined below for convenience.
 */

#define isc_region_consume(r,l) \
	do { \
		isc_region_t *_r = (r); \
		unsigned int _l = (l); \
		INSIST(_r->length >= _l); \
		_r->base += _l; \
		_r->length -= _l; \
	} while (/*CONSTCOND*/0)

#define isc_textregion_consume(r,l) \
	do { \
		isc_textregion_t *_r = (r); \
		unsigned int _l = (l); \
		INSIST(_r->length >= _l); \
		_r->base += _l; \
		_r->length -= _l; \
	} while (/*CONSTCOND*/0)

#define isc_constregion_consume(r,l) \
	do { \
		isc_constregion_t *_r = (r); \
		unsigned int _l = (l); \
		INSIST(_r->length >= _l); \
		_r->base += _l; \
		_r->length -= _l; \
	} while (/*CONSTCOND*/0)
/*@}*/

ISC_LANG_BEGINDECLS

int
isc_region_compare(isc_region_t *r1, isc_region_t *r2);
/*%<
 * Compares the contents of two regions 
 *
 * Requires: 
 *\li	'r1' is a valid region
 *\li	'r2' is a valid region
 *
 * Returns:
 *\li	 < 0 if r1 is lexicographically less than r2
 *\li	 = 0 if r1 is lexicographically identical to r2
 *\li	 > 0 if r1 is lexicographically greater than r2
 */

ISC_LANG_ENDDECLS

#endif /* ISC_REGION_H */