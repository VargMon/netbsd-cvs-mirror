/*	$NetBSD: refclock_shm.c,v 1.1.1.2.2.1 2014/12/25 02:34:38 snj Exp $	*/

/*
 * refclock_shm - clock driver for utc via shared memory
 * - under construction -
 * To add new modes: Extend or union the shmTime-struct. Do not
 * extend/shrink size, because otherwise existing implementations
 * will specify wrong size of shared memory-segment
 * PB 18.3.97
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "ntp_types.h"

#if defined(REFCLOCK) && defined(CLOCK_SHM)

#include "ntpd.h"
#undef fileno
#include "ntp_io.h"
#undef fileno
#include "ntp_refclock.h"
#undef fileno
#include "timespecops.h"
#undef fileno
#include "ntp_stdlib.h"

#undef fileno
#include <ctype.h>
#undef fileno

#ifndef SYS_WINNT
# include <sys/ipc.h>
# include <sys/shm.h>
# include <assert.h>
# include <unistd.h>
# include <stdio.h>
#endif

/*
 * This driver supports a reference clock attached thru shared memory
 */

/*
 * SHM interface definitions
 */
#define PRECISION       (-1)    /* precision assumed (0.5 s) */
#define REFID           "SHM"   /* reference ID */
#define DESCRIPTION     "SHM/Shared memory interface"

#define NSAMPLES        3       /* stages of median filter */

/*
 * Function prototypes
 */
static  int     shm_start       (int unit, struct peer *peer);
static  void    shm_shutdown    (int unit, struct peer *peer);
static  void    shm_poll        (int unit, struct peer *peer);
static  void    shm_timer       (int unit, struct peer *peer);
static	void	shm_peek	(int unit, struct peer *peer);
static	void	shm_clockstats  (int unit, struct peer *peer);
static	void	shm_control	(int unit, const struct refclockstat * in_st,
				 struct refclockstat * out_st, struct peer *peer);

/*
 * Transfer vector
 */
struct  refclock refclock_shm = {
	shm_start,              /* start up driver */
	shm_shutdown,           /* shut down driver */
	shm_poll,		/* transmit poll message */
	shm_control,		/* control settings */
	noentry,		/* not used: init */
	noentry,		/* not used: buginfo */
	shm_timer,              /* once per second */
};

struct shmTime {
	int    mode; /* 0 - if valid is set:
		      *       use values,
		      *       clear valid
		      * 1 - if valid is set:
		      *       if count before and after read of values is equal,
		      *         use values
		      *       clear valid
		      */
	volatile int    count;
	time_t		clockTimeStampSec;
	int		clockTimeStampUSec;
	time_t		receiveTimeStampSec;
	int		receiveTimeStampUSec;
	int		leap;
	int		precision;
	int		nsamples;
	volatile int    valid;
	unsigned	clockTimeStampNSec;	/* Unsigned ns timestamps */
	unsigned	receiveTimeStampNSec;	/* Unsigned ns timestamps */
	int		dummy[8];
};

struct shmunit {
	struct shmTime *shm;	/* pointer to shared memory segment */

	/* debugging/monitoring counters - reset when printed */
	int ticks;		/* number of attempts to read data*/
	int good;		/* number of valid samples */
	int notready;		/* number of peeks without data ready */
	int bad;		/* number of invalid samples */
	int clash;		/* number of access clashes while reading */

	time_t max_delta;	/* difference limit */
	time_t max_delay;	/* age/stale limit */
};


struct shmTime *getShmTime(int);

struct shmTime *getShmTime (int unit) {
#ifndef SYS_WINNT
	int shmid=0;

	/* 0x4e545030 is NTP0.
	 * Big units will give non-ascii but that's OK
	 * as long as everybody does it the same way.
	 */
	shmid=shmget (0x4e545030 + unit, sizeof (struct shmTime),
		      IPC_CREAT | ((unit < 2) ? 0600 : 0666));
	if (shmid == -1) { /* error */
		msyslog(LOG_ERR, "SHM shmget (unit %d): %m", unit);
		return 0;
	}
	else { /* no error  */
		struct shmTime *p = (struct shmTime *)shmat (shmid, 0, 0);
		if (p == (struct shmTime *)-1) { /* error */
			msyslog(LOG_ERR, "SHM shmat (unit %d): %m", unit);
			return 0;
		}
		return p;
	}
#else
	char buf[10];
	LPSECURITY_ATTRIBUTES psec = 0;
	HANDLE shmid = 0;
	SECURITY_DESCRIPTOR sd;
	SECURITY_ATTRIBUTES sa;

	snprintf(buf, sizeof(buf), "NTP%d", unit);
	if (unit >= 2) { /* world access */
		if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION)) {
			msyslog(LOG_ERR,"SHM InitializeSecurityDescriptor (unit %d): %m", unit);
			return 0;
		}
		if (!SetSecurityDescriptorDacl(&sd, 1, 0, 0)) {
			msyslog(LOG_ERR, "SHM SetSecurityDescriptorDacl (unit %d): %m", unit);
			return 0;
		}
		sa.nLength=sizeof (SECURITY_ATTRIBUTES);
		sa.lpSecurityDescriptor = &sd;
		sa.bInheritHandle = 0;
		psec = &sa;
	}
	shmid = CreateFileMapping ((HANDLE)0xffffffff, psec, PAGE_READWRITE,
				 0, sizeof (struct shmTime), buf);
	if (!shmid) { /*error*/
		char buf[1000];

		FormatMessage (FORMAT_MESSAGE_FROM_SYSTEM,
			       0, GetLastError (), 0, buf, sizeof (buf), 0);
		msyslog(LOG_ERR, "SHM CreateFileMapping (unit %d): %s", unit, buf);
		return 0;
	} else {
		struct shmTime *p = (struct shmTime *) MapViewOfFile (shmid,
								    FILE_MAP_WRITE, 0, 0, sizeof (struct shmTime));
		if (p == 0) { /*error*/
			char buf[1000];

			FormatMessage (FORMAT_MESSAGE_FROM_SYSTEM,
				       0, GetLastError (), 0, buf, sizeof (buf), 0);
			msyslog(LOG_ERR,"SHM MapViewOfFile (unit %d): %s", unit, buf) 
			return 0;
		}
		return p;
	}
#endif
}
/*
 * shm_start - attach to shared memory
 */
static int
shm_start(
	int unit,
	struct peer *peer
	)
{
	struct refclockproc *pp;
	struct shmunit *up;

	pp = peer->procptr;
	pp->io.clock_recv = noentry;
	pp->io.srcclock = peer;
	pp->io.datalen = 0;
	pp->io.fd = -1;

	up = emalloc_zero(sizeof(*up));

	up->shm = getShmTime(unit);

	/*
	 * Initialize miscellaneous peer variables
	 */
	memcpy((char *)&pp->refid, REFID, 4);
	if (up->shm != 0) {
		pp->unitptr = up;
		up->shm->precision = PRECISION;
		peer->precision = up->shm->precision;
		up->shm->valid = 0;
		up->shm->nsamples = NSAMPLES;
		pp->clockdesc = DESCRIPTION;
		/* items to be changed later in 'shm_control()': */
		up->max_delay = 5;
		up->max_delta = 4*3600;
		return 1;
	} else {
		free(up);
		pp->unitptr = NULL;
		return 0;
	}
}


/*
 * shm_control - configure flag1/time2 params
 *
 * These are not yet available during 'shm_start', so we have to do any
 * pre-computations we want to avoid during regular poll/timer callbacks
 * in this callback.
 */
static void
shm_control(
	int                         unit,
	const struct refclockstat * in_st,
	struct refclockstat       * out_st,
	struct peer               * peer
	)
{
	struct refclockproc *pp;
	struct shmunit *up;

	pp = peer->procptr;
	up = pp->unitptr;

	if (NULL == up)
		return;
	if (pp->sloppyclockflag & CLK_FLAG1)
		up->max_delta = 0;
	else if (pp->fudgetime2 < 1. || pp->fudgetime2 > 86400.)
		up->max_delta = 4*3600;
	else
		up->max_delta = (time_t)floor(pp->fudgetime2 + 0.5);
}


/*
 * shm_shutdown - shut down the clock
 */
static void
shm_shutdown(
	int unit,
	struct peer *peer
	)
{
	struct refclockproc *pp;
	struct shmunit *up;

	pp = peer->procptr;
	up = pp->unitptr;

	if (NULL == up)
		return;
#ifndef SYS_WINNT
	/* HMS: shmdt() wants char* or const void * */
	(void) shmdt ((char *)up->shm);
#else
	UnmapViewOfFile (up->shm);
#endif
	free(up);
}


/*
 * shm_timer - called every second
 */
static  void
shm_timer(int unit, struct peer *peer)
{
	shm_peek(unit, peer);
}


/*
 * shm_poll - called by the transmit procedure
 */
static void
shm_poll(
	int unit,
	struct peer *peer
	)
{
	struct refclockproc *pp;
	struct shmunit *up;
	int major_error;

	pp = peer->procptr;
	up = pp->unitptr;

	pp->polls++;

	/* get dominant reason if we have no samples at all */
	major_error = max(up->notready, up->bad);
	major_error = max(major_error, up->clash);

        /*
         * Process median filter samples. If none received, see what
         * happened, tell the core and keep going.
         */
        if (pp->coderecv != pp->codeproc) {
		/* have some samples, everything OK */
		pp->lastref = pp->lastrec;
		refclock_receive(peer);
	} else if (NULL == up->shm) { /* is this possible at all? */
		/* we're out of business without SHM access */
		refclock_report(peer, CEVNT_FAULT);
	} else if (major_error == up->clash) {
		/* too many collisions is like a bad signal */
                refclock_report(peer, CEVNT_PROP);
	} else if (major_error == up->bad) {
		/* too much stale/bad/garbled data */
                refclock_report(peer, CEVNT_BADREPLY);
	} else {
		/* in any other case assume it's just a timeout */
                refclock_report(peer, CEVNT_TIMEOUT);
        }
	/* shm_clockstats() clears the tallies, so it must be last... */
	shm_clockstats(unit, peer);
}

/*
 * shm_peek - try to grab a sample
 */
static void
shm_peek(
	int unit,
	struct peer *peer
	)
{
	struct refclockproc *pp;
	struct shmunit *up;

	/* access order is important for lock-free SHM access; we
	** enforce order by treating the whole structure volatile.
	**
	** IMPORTANT NOTE: This does not protect from reordering on CPU
	** level, and it does nothing for cache consistency and
	** visibility of changes by other cores. We need atomic and/or
	** fence instructions for that.
	*/
	volatile struct shmTime *shm;

	struct timespec tvr;
	struct timespec tvt;
	l_fp tsrcv;
	l_fp tsref;
	unsigned int c;
	unsigned int cns_new, rns_new;
	int cnt;

	/* for formatting 'a_lastcode': */
	struct calendar cd;
	time_t tt, now;
	vint64 ts;

	/*
	 * This is the main routine. It snatches the time from the shm
	 * board and tacks on a local timestamp.
	 */
	pp = peer->procptr;
	up = pp->unitptr;
	up->ticks++;
	if (up->shm == 0) {
		/* try to map again - this may succeed if meanwhile some-
		body has ipcrm'ed the old (unaccessible) shared mem segment */
		up->shm = getShmTime(unit);
	}
	shm = up->shm;
	if (shm == 0) {
		DPRINTF(1, ("%s: no SHM segment\n",
			    refnumtoa(&peer->srcadr)));
		return;
	}
	if ( ! shm->valid) {
		DPRINTF(1, ("%s: SHM not ready\n",
			    refnumtoa(&peer->srcadr)));
		up->notready++;
		return;
	}

	switch (shm->mode) {
	case 0:
		tvr.tv_sec	= shm->receiveTimeStampSec;
		tvr.tv_nsec	= shm->receiveTimeStampUSec * 1000;
		rns_new		= shm->receiveTimeStampNSec;
		tvt.tv_sec	= shm->clockTimeStampSec;
		tvt.tv_nsec	= shm->clockTimeStampUSec * 1000;
		cns_new		= shm->clockTimeStampNSec;

		/* Since the following comparisons are between unsigned
		** variables they are always well defined, and any
		** (signed) underflow will turn into very large unsigned
		** values, well above the 1000 cutoff.
		**
		** Note: The usecs *must* be a *truncated*
		** representation of the nsecs. This code will fail for
		** *rounded* usecs, and the logic to deal with
		** wrap-arounds in the presence of rounded values is
		** much more convoluted.
		*/
		if (   ((cns_new - (unsigned)tvt.tv_nsec) < 1000)
		    && ((rns_new - (unsigned)tvr.tv_nsec) < 1000)) {
			tvt.tv_nsec = cns_new;
			tvr.tv_nsec = rns_new;
		}
		/* At this point tvr and tvt contains valid ns-level
		** timestamps, possibly generated by extending the old
		** us-level timestamps
		*/
 		DPRINTF(2, ("%s: SHM type 0 sample\n",
			    refnumtoa(&peer->srcadr)));
		break;

	case 1:
		cnt = shm->count;

		tvr.tv_sec	= shm->receiveTimeStampSec;
		tvr.tv_nsec	= shm->receiveTimeStampUSec * 1000;
		rns_new		= shm->receiveTimeStampNSec;
		tvt.tv_sec	= shm->clockTimeStampSec;
		tvt.tv_nsec	= shm->clockTimeStampUSec * 1000;
		cns_new		= shm->clockTimeStampNSec;
		if (cnt != shm->count) {
			DPRINTF(1, ("%s: type 1 access clash\n",
				    refnumtoa(&peer->srcadr)));
			msyslog (LOG_NOTICE, "SHM: access clash in shared memory");
			up->clash++;
			return;
		}
		
		/* See the case above for an explanation of the
		** following test.
		*/
		if (   ((cns_new - (unsigned)tvt.tv_nsec) < 1000)
		    && ((rns_new - (unsigned)tvr.tv_nsec) < 1000)) {
			tvt.tv_nsec = cns_new;
			tvr.tv_nsec = rns_new;
		}
		/* At this point tvr and tvt contains valid ns-level
		** timestamps, possibly generated by extending the old
		** us-level timestamps
		*/
 		DPRINTF(2, ("%s: SHM type 1 sample\n",
			    refnumtoa(&peer->srcadr)));
		break;

	default:
 		DPRINTF(1, ("%s: SHM type blooper, mode=%d\n",
			    refnumtoa(&peer->srcadr), shm->mode));
		up->bad++;
		msyslog (LOG_ERR, "SHM: bad mode found in shared memory: %d",
			 shm->mode);
		return;
	}
	shm->valid = 0;

	/* format the last time code in human-readable form into
	 * 'pp->a_lastcode'. Someone claimed: "NetBSD has incompatible
	 * tv_sec". I can't find a base for this claim, but we can work
	 * around that potential problem. BTW, simply casting a pointer
	 * is a receipe for disaster on some architectures.
	 */
	tt = (time_t)tvt.tv_sec;
	ts = time_to_vint64(&tt);
	ntpcal_time_to_date(&cd, &ts);
		
	/* add ntpq -c cv timecode in ISO 8601 format */
	c = snprintf(pp->a_lastcode, sizeof(pp->a_lastcode),
		     "%04u-%02u-%02uT%02u:%02u:%02u.%09ldZ",
		     cd.year, cd.month, cd.monthday,
		     cd.hour, cd.minute, cd.second,
		     (long)tvt.tv_nsec);
	pp->lencode = (c < sizeof(pp->a_lastcode)) ? c : 0;

	/* check 1: age control of local time stamp */
	time(&now);
	tt = now - tvr.tv_sec;
	if (tt < 0 || tt > up->max_delay) {
		DPRINTF(1, ("%s:SHM stale/bad receive time, delay=%llds\n",
			    refnumtoa(&peer->srcadr), (long long)tt));
		up->bad++;
		msyslog (LOG_ERR, "SHM: stale/bad receive time, delay=%llds",
			 (long long)tt);
		return;
	}

	/* check 2: delta check */
	tt = tvr.tv_sec - tvt.tv_sec - (tvr.tv_nsec < tvt.tv_nsec);
	if (tt < 0)
		tt = -tt;
	if (up->max_delta > 0 && tt > up->max_delta) {
		DPRINTF(1, ("%s: SHM diff limit exceeded, delta=%llds\n",
			    refnumtoa(&peer->srcadr), (long long)tt));
		up->bad++;
		msyslog (LOG_ERR, "SHM: difference limit exceeded, delta=%llds\n",
			 (long long)tt);
		return;
	}

	/* if we really made it to this point... we're winners! */
	DPRINTF(2, ("%s: SHM feeding data\n",
		    refnumtoa(&peer->srcadr)));
	tsrcv = tspec_stamp_to_lfp(tvr);
	tsref = tspec_stamp_to_lfp(tvt);
	pp->leap = shm->leap;
	peer->precision = shm->precision;
	refclock_process_offset(pp, tsref, tsrcv, pp->fudgetime1);
	up->good++;
}

/*
 * shm_clockstats - dump and reset counters
 */
static void shm_clockstats(
	int unit,
	struct peer *peer
	)
{
	struct refclockproc *pp;
	struct shmunit *up;
	char logbuf[64];
	unsigned int llen;

	pp = peer->procptr;
	up = pp->unitptr;

	if (pp->sloppyclockflag & CLK_FLAG4) {
		/* if snprintf() returns a negative values on errors
		** (some older ones do) make sure we are NUL
		** terminated. Using an unsigned result does the trick.
		*/
		llen = snprintf(logbuf, sizeof(logbuf),
				"%3d %3d %3d %3d %3d",
				up->ticks, up->good, up->notready,
				up->bad, up->clash);
		logbuf[min(llen, sizeof(logbuf)-1)] = '\0';
		record_clock_stats(&peer->srcadr, logbuf);
	}
	up->ticks = up->good = up->notready = up->bad = up->clash = 0;

}

#else
NONEMPTY_TRANSLATION_UNIT
#endif /* REFCLOCK */