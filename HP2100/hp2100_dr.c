/* hp2100_dr.c: HP 2100 12606B/12610B fixed head disk/drum simulator

   Copyright (c) 1993-2004, Robert M. Supnik

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
   ROBERT M SUPNIK BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
   IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   Except as contained in this notice, the name of Robert M Supnik shall not
   be used in advertising or otherwise to promote the sale, use or other dealings
   in this Software without prior written authorization from Robert M Supnik.

   dr		12606B 2770/2771 fixed head disk
		12610B 2773/2774/2775 drum

   07-Oct-04	JDB	Fixed enable/disable from either device
			Fixed sector return in status word
			Provided protected tracks and "Writing Enabled" status bit
			Fixed DMA last word write, incomplete sector fill value
			Added "parity error" status return on writes for 12606
			Added track origin test for 12606
			Added SCP test for 12606
			Fixed 12610 SFC operation
			Added "Sector Flag" status bit
			Added "Read Inhibit" status bit for 12606
			Fixed current-sector determination
			Added PROTECTED, UNPROTECTED, TRACKPROT modifiers
   26-Aug-04	RMS	Fixed CLC to stop operation (from Dave Bryan)
   26-Apr-04	RMS	Fixed SFS x,C and SFC x,C
			Revised boot rom to use IBL algorithm
			Implemented DMA SRQ (follows FLG)
   27-Jul-03	RMS	Fixed drum sizes
			Fixed variable capacity interaction with SAVE/RESTORE
   10-Nov-02	RMS	Added BOOT command

   These head-per-track devices are buffered in memory, to minimize overhead.

   The drum data channel does not have a command flip-flop.  Its control
   flip-flop is not wired into the interrupt chain; accordingly, the
   simulator uses command rather than control for the data channel.  Its
   flag does not respond to SFS, SFC, or STF.
   
   The drum control channel does not have any of the traditional flip-flops.

   The 12606 interface implements two diagnostic tests.  An SFS CC instruction
   will skip if the disk has passed the track origin (sector 0) since the last
   CLF CC instruction, and an SFC CC instruction will skip if the Sector Clock
   Phase (SCP) flip-flop is clear, indicating that the current sector is
   accessible.  The 12610 interface does not support these tests; the SKF signal
   is not driven, so neither SFC CC nor SFS CC will skip.

   The interface implements a track protect mechanism via a switch and a set of
   on-card diodes.  The switch sets the protected/unprotected status, and the
   particular diodes installed indicate the range of tracks (a power of 2) that
   are read-only in the protected mode.

   Somewhat unusually, writing to a protected track completes normally, but the
   data isn't actually written, as the write current is inhibited.  There is no
   "failure" status indication.  Instead, a program must note the lack of
   "Writing Enabled" status before the write is attempted.

   Specifications (2770/2771):
   - 90 sectors per logical track
   - 45 sectors per revolution
   - 64 words per sector
   - 2880 words per revolution
   - 3450 RPM = 17.4 ms/revolution
   - data timing = 6.0 us/word, 375 us/sector
   - inst timing = 4 inst/word, 11520 inst/revolution

   Specifications 2773/2774/2775:
   - 32 sectors per logical track
   - 32 sectors per revolution
   - 64 words per sector
   - 2048 words per revolution
   - 3450 RPM = 17.4 ms/revolution
   - data timing = 8.5 us/word, 550 us/sector
   - inst timing = 6 inst/word, 12288 inst/revolution

   References:
   - 12606B Disc Memory Interface Kit Operating and Service Manual
     (12606-90012, Mar-1970)
   - 12610B Drum Memory Interface Kit Operating and Service Manual
     (12610-9001, Feb-1970)
*/

#include "hp2100_defs.h"
#include <math.h>

/* Constants */

#define DR_NUMWD	64				/* words/sector */
#define DR_FNUMSC	90				/* fhd sec/track */
#define DR_DNUMSC	32				/* drum sec/track */
#define DR_NUMSC	((drc_unit.flags & UNIT_DR)? DR_DNUMSC: DR_FNUMSC)
#define DR_SIZE		(512 * DR_DNUMSC * DR_NUMWD)	/* initial size */
#define DR_FTIME	4				/* fhd per-word time */
#define DR_DTIME	6				/* drum per-word time */
#define DR_OVRHEAD	5				/* overhead words at track start */
#define UNIT_V_PROT	(UNIT_V_UF + 0)			/* track protect */
#define UNIT_V_SZ	(UNIT_V_UF + 1)			/* disk vs drum */
#define UNIT_M_SZ	017				/* size */
#define UNIT_PROT	(1 << UNIT_V_PROT)
#define UNIT_SZ		(UNIT_M_SZ << UNIT_V_SZ)
#define UNIT_DR		(1 << UNIT_V_SZ)		/* low order bit */
#define  SZ_180K	000				/* disks */
#define  SZ_360K	002
#define  SZ_720K	004
#define  SZ_1024K	001				/* drums: default size */
#define  SZ_1536K	003
#define  SZ_384K	005
#define  SZ_512K	007
#define  SZ_640K	011
#define  SZ_768K	013
#define  SZ_896K	015
#define DR_GETSZ(x)	(((x) >> UNIT_V_SZ) & UNIT_M_SZ)

/* Command word */

#define CW_WR		0100000				/* write vs read */
#define CW_V_FTRK	7				/* fhd track */
#define CW_M_FTRK	0177
#define CW_V_DTRK	5				/* drum track */
#define CW_M_DTRK	01777
#define MAX_TRK		(((drc_unit.flags & UNIT_DR)? CW_M_DTRK: CW_M_FTRK) + 1)
#define CW_GETTRK(x)	((drc_unit.flags & UNIT_DR)? \
				(((x) >> CW_V_DTRK) & CW_M_DTRK): \
				(((x) >> CW_V_FTRK) & CW_M_FTRK))
#define CW_PUTTRK(x)	((drc_unit.flags & UNIT_DR)? \
				(((x) & CW_M_DTRK) << CW_V_DTRK): \
				(((x) & CW_M_FTRK) << CW_V_FTRK))
#define CW_V_FSEC	0				/* fhd sector */
#define CW_M_FSEC	0177
#define CW_V_DSEC	0				/* drum sector */
#define CW_M_DSEC	037
#define CW_GETSEC(x)	((drc_unit.flags & UNIT_DR)? \
				(((x) >> CW_V_DSEC) & CW_M_DSEC): \
				(((x) >> CW_V_FSEC) & CW_M_FSEC))
#define CW_PUTSEC(x)	((drc_unit.flags & UNIT_DR)? \
				(((x) & CW_M_DSEC) << CW_V_DSEC): \
				(((x) & CW_M_FSEC) << CW_V_FSEC))

/* Status register, ^ = dynamic */

#define DRS_V_NS	8				/* ^next sector */
#define DRS_M_NS	0177
#define DRS_SEC		0100000				/* ^sector flag */
#define DRS_RDY		0000200				/* ^ready */
#define DRS_RIF		0000100				/* ^read inhibit */
#define DRS_SAC		0000040				/* sector coincidence */
#define DRS_ABO		0000010				/* abort */
#define DRS_WEN		0000004				/* ^write enabled */
#define DRS_PER		0000002				/* parity error */
#define DRS_BSY		0000001				/* ^busy */

#define CALC_SCP(x)	(((int32) fmod ((x) / (double) dr_time,  \
			    (double) (DR_NUMWD))) >= (DR_NUMWD - 3))

extern UNIT cpu_unit;
extern uint16 *M;
extern uint32 PC;
extern uint32 dev_cmd[2], dev_ctl[2], dev_flg[2], dev_fbf[2], dev_srq[2];

int32 drc_cw = 0;					/* fnc, addr */
int32 drc_sta = 0;					/* status */
int32 drc_run = 0;					/* run flip-flop */
int32 drd_ibuf = 0;					/* input buffer */
int32 drd_obuf = 0;					/* output buffer */
int32 drd_ptr = 0;					/* sector pointer */
int32 drc_pcount = 1;					/* number of prot tracks */
int32 dr_stopioe = 1;					/* stop on error */
int32 dr_time = DR_DTIME;				/* time per word */

static int32 sz_tab[16] = {
 184320, 1048576, 368640, 1572864, 737280, 393216, 0, 524288,
 0, 655360, 0, 786432,  0, 917504, 0, 0 };

DEVICE drd_dev, drc_dev;
int32 drdio (int32 inst, int32 IR, int32 dat);
int32 drcio (int32 inst, int32 IR, int32 dat);
t_stat drc_svc (UNIT *uptr);
t_stat drc_reset (DEVICE *dptr);
t_stat drc_attach (UNIT *uptr, char *cptr);
t_stat drc_boot (int32 unitno, DEVICE *dptr);
int32 dr_incda (int32 trk, int32 sec, int32 ptr);
int32 dr_seccntr (double simtime);
t_stat dr_set_prot (UNIT *uptr, int32 val, char *cptr, void *desc);
t_stat dr_set_size (UNIT *uptr, int32 val, char *cptr, void *desc);

/* DRD data structures

   drd_dev	device descriptor
   drd_unit	unit descriptor
   drd_reg	register list
*/

DIB dr_dib[] = {
	{ DRD, 0, 0, 0, 0, 0, &drdio },
	{ DRC, 0, 0, 0, 0, 0, &drcio }  };

#define drd_dib dr_dib[0]
#define drc_dib dr_dib[1]

UNIT drd_unit[] = {
	{ UDATA (NULL, 0, 0) },
	{ UDATA (NULL, UNIT_DIS, 0) }  };

#define TMR_ORG		0				/* origin timer */
#define TMR_INH		1				/* inhibit timer */

REG drd_reg[] = {
	{ ORDATA (IBUF, drd_ibuf, 16) },
	{ ORDATA (OBUF, drd_obuf, 16) },
	{ FLDATA (CMD, drd_dib.cmd, 0) },
	{ FLDATA (CTL, drd_dib.ctl, 0) },
	{ FLDATA (FLG, drd_dib.flg, 0) },
	{ FLDATA (FBF, drd_dib.fbf, 0) },
	{ FLDATA (SRQ, drd_dib.srq, 0) },
	{ ORDATA (BPTR, drd_ptr, 6) },
	{ ORDATA (DEVNO, drd_dib.devno, 6), REG_HRO },
	{ NULL }  };

MTAB drd_mod[] = {
	{ MTAB_XTD | MTAB_VDV, 1, "DEVNO", "DEVNO",
		&hp_setdev, &hp_showdev, &drd_dev },
	{ 0 }  };

DEVICE drd_dev = {
	"DRD", drd_unit, drd_reg, drd_mod,
	2, 0, 0, 0, 0, 0,
	NULL, NULL, &drc_reset,
	NULL, NULL, NULL,
	&drd_dib, DEV_DISABLE };

/* DRC data structures

   drc_dev	device descriptor
   drc_unit	unit descriptor
   drc_mod	unit modifiers
   drc_reg	register list
*/

UNIT drc_unit =
	{ UDATA (&drc_svc, UNIT_FIX+UNIT_ATTABLE+UNIT_BUFABLE+
		UNIT_MUSTBUF+UNIT_DR+UNIT_BINK, DR_SIZE) };

REG drc_reg[] = {
	{ DRDATA (PCNT, drc_pcount, 10), REG_HIDDEN | PV_LEFT },
	{ ORDATA (CW, drc_cw, 16) },
	{ ORDATA (STA, drc_sta, 16) },
	{ FLDATA (RUN, drc_run, 0) },
	{ FLDATA (CMD, drc_dib.cmd, 0) },
	{ FLDATA (CTL, drc_dib.ctl, 0) },
	{ FLDATA (FLG, drc_dib.flg, 0) },
	{ FLDATA (FBF, drc_dib.fbf, 0) },
	{ FLDATA (SRQ, drc_dib.srq, 0) },
	{ DRDATA (TIME, dr_time, 24), REG_NZ + PV_LEFT },
	{ FLDATA (STOP_IOE, dr_stopioe, 0) },
	{ ORDATA (DEVNO, drc_dib.devno, 6), REG_HRO },
	{ DRDATA (CAPAC, drc_unit.capac, 24), REG_HRO },
	{ NULL }  };

MTAB drc_mod[] = {
	{ UNIT_DR, 0, "disk", NULL, NULL },
	{ UNIT_DR, UNIT_DR, "drum", NULL, NULL },
	{ UNIT_SZ, (SZ_180K << UNIT_V_SZ), NULL, "180K", &dr_set_size },
	{ UNIT_SZ, (SZ_360K << UNIT_V_SZ), NULL, "360K", &dr_set_size },
	{ UNIT_SZ, (SZ_720K << UNIT_V_SZ), NULL, "720K", &dr_set_size },
	{ UNIT_SZ, (SZ_384K << UNIT_V_SZ), NULL, "384K", &dr_set_size },
	{ UNIT_SZ, (SZ_512K << UNIT_V_SZ), NULL, "512K", &dr_set_size },
	{ UNIT_SZ, (SZ_640K << UNIT_V_SZ), NULL, "640K", &dr_set_size },
	{ UNIT_SZ, (SZ_768K << UNIT_V_SZ), NULL, "768K", &dr_set_size },
	{ UNIT_SZ, (SZ_896K << UNIT_V_SZ), NULL, "896K", &dr_set_size },
	{ UNIT_SZ, (SZ_1024K << UNIT_V_SZ), NULL, "1024K", &dr_set_size },
	{ UNIT_SZ, (SZ_1536K << UNIT_V_SZ), NULL, "1536K", &dr_set_size },
	{ UNIT_PROT, UNIT_PROT, "protected", "PROTECTED", NULL },
	{ UNIT_PROT, 0, "unprotected", "UNPROTECTED", NULL },
	{ MTAB_XTD | MTAB_VDV | MTAB_VAL, 0, "tracks protected", "TRACKPROT",
		&dr_set_prot, NULL, &drc_reg[0] },
	{ MTAB_XTD | MTAB_VDV, 1, "DEVNO", "DEVNO",
		&hp_setdev, &hp_showdev, &drd_dev },
	{ 0 }  };

DEVICE drc_dev = {
	"DRC", &drc_unit, drc_reg, drc_mod,
	1, 8, 21, 1, 8, 16,
	NULL, NULL, &drc_reset,
	&drc_boot, &drc_attach, NULL,
	&drc_dib, DEV_DISABLE };

/* IOT routines */

int32 drdio (int32 inst, int32 IR, int32 dat)
{
int32 devd, t;

devd = IR & I_DEVMASK;					/* get device no */
switch (inst) {						/* case on opcode */
case ioOTX:						/* output */
	drd_obuf = dat;
	break;
case ioMIX:						/* merge */
	dat = dat | drd_ibuf;
	break;
case ioLIX:						/* load */
	dat = drd_ibuf;
	break;
case ioCTL:						/* control clear/set */
	if (IR & I_AB) {				/* CLC */
	    clrCMD (devd);				/* clr "ctl" */
	    clrFSR (devd);				/* clr flg */
	    if (!drc_run) sim_cancel (&drc_unit);	/* cancel curr op */
	    drc_sta = drc_sta & ~DRS_SAC;  }		/* clear SAC flag */
	else if (!CMD (devd)) {				/* STC, not set? */
	    setCMD (devd);				/* set "ctl" */
	    if (drc_cw & CW_WR) { setFSR (devd); }	/* prime DMA */
	    drc_sta = 0;				/* clr status */
	    drd_ptr = 0;				/* clear sec ptr */
	    sim_cancel (&drc_unit);			/* cancel curr op */
	    t = CW_GETSEC (drc_cw) - dr_seccntr (sim_gtime());
	    if (t <= 0) t = t + DR_NUMSC;
	    sim_activate (&drc_unit, t * DR_NUMWD * dr_time);  }
	break;
default:
	break;  }
if (IR & I_HC) { clrFSR (devd); }			/* H/C option */
return dat;
}

int32 drcio (int32 inst, int32 IR, int32 dat)
{
int32 sec;

switch (inst) {						/* case on opcode */
case ioFLG:						/* flag clear/set */
	if ((IR & I_HC) && !(drc_unit.flags & UNIT_DR)) {	/* CLF disk */
	    sec = dr_seccntr (sim_gtime ());		/* current sector */
	    sim_cancel (&drd_unit[TMR_ORG]);		/* sched origin tmr */
	    sim_activate (&drd_unit[TMR_ORG],
		(DR_FNUMSC - sec) * DR_NUMWD * dr_time);   }
	break;
case ioSFC:						/* skip flag clear */
	if (drc_unit.flags & UNIT_DR) break;		/* 12610 never skips */
	if (!(CALC_SCP (sim_gtime())))			/* nearing end of sector? */
	    PC = (PC + 1) & VAMASK;			/* skip if SCP clear */
	break;
case ioSFS:						/* skip flag set */
	if (drc_unit.flags & UNIT_DR) break;		/* 12610 never skips */
	if (!sim_is_active (&drd_unit[TMR_ORG]))	/* passed origin? */
		PC = (PC + 1) & VAMASK;			/* skip if origin seen */
	break;
case ioOTX:						/* output */
	if (!(drc_unit.flags & UNIT_DR)) {		/* disk? */
	    sim_cancel (&drd_unit[TMR_INH]);		/* schedule inhibit timer */
	    sim_activate (&drd_unit[TMR_INH], DR_FTIME * DR_NUMWD);  }
	drc_cw = dat;					/* get control word */
	break;
case ioLIX:						/* load */
	dat = 0;
case ioMIX:						/* merge */
	dat = dat | drc_sta;				/* static bits */
	if (!(drc_unit.flags & UNIT_PROT) ||		/* not protected? */
	     (CW_GETTRK(drc_cw) >= drc_pcount))		/* or not in range? */
	    dat = dat | DRS_WEN;			/* set wrt enb status */
	if (drc_unit.flags & UNIT_ATT) {		/* attached? */
	    dat = dat | (dr_seccntr (sim_gtime()) << DRS_V_NS) | DRS_RDY;
	    if (sim_is_active (&drc_unit))		/* op in progress? */
		dat = dat | DRS_BSY;
	    if (CALC_SCP (sim_gtime()))			/* SCP ff set? */
		dat = dat | DRS_SEC;			/* set sector flag */
	    if (sim_is_active (&drd_unit[TMR_INH]) &&	/* inhibit timer on? */
		!(drc_cw & CW_WR))
		dat = dat | DRS_RIF;  }			/* set read inh flag */
	break;
default:
	break;  }
return dat;
}

/* Unit service */

t_stat drc_svc (UNIT *uptr)
{
int32 devd, trk, sec;
uint32 da;
uint16 *bptr = uptr->filebuf;

if ((uptr->flags & UNIT_ATT) == 0) {
	drc_sta = DRS_ABO;
	return IORETURN (dr_stopioe, SCPE_UNATT);  }

devd = drd_dib.devno;					/* get dch devno */
trk = CW_GETTRK (drc_cw);
sec = CW_GETSEC (drc_cw);
da = ((trk * DR_NUMSC) + sec) * DR_NUMWD;
drc_sta = drc_sta | DRS_SAC;
drc_run = 1;						/* set run ff */

if (drc_cw & CW_WR) {					/* write? */
	if ((da < uptr->capac) && (sec < DR_NUMSC)) {
	    bptr[da + drd_ptr] = drd_obuf;
	    if (((uint32) (da + drd_ptr)) >= uptr->hwmark)
		uptr->hwmark = da + drd_ptr + 1;  }
	drd_ptr = dr_incda (trk, sec, drd_ptr);		/* inc disk addr */
	if (CMD (devd)) {				/* dch active? */
	    setFSR (devd);				/* set dch flg */
	    sim_activate (uptr, dr_time);  }		/* sched next word */
	else {						/* done */
	    if (drd_ptr)				/* need to fill? */
		for ( ; drd_ptr < DR_NUMWD; drd_ptr++)
		    bptr[da + drd_ptr] = drd_obuf;	/* fill with last word */
	    if (!(drc_unit.flags & UNIT_DR))		/* disk? */
		drc_sta = drc_sta | DRS_PER;		/* parity bit sets on write */
	    drc_run = 0;  }				/* clear run ff */
	}						/* end write */
else {							/* read */
	if (CMD (devd)) {				/* dch active? */
	    if ((da >= uptr->capac) || (sec >= DR_NUMSC)) drd_ibuf = 0;
	    else drd_ibuf = bptr[da + drd_ptr];
	    drd_ptr = dr_incda (trk, sec, drd_ptr);
	    setFSR (devd);				/* set dch flg */
	    sim_activate (uptr, dr_time);  }		/* sched next word */
	else drc_run = 0;				/* clear run ff */
	}
return SCPE_OK;
}

/* Increment current disk address */

int32 dr_incda (int32 trk, int32 sec, int32 ptr)
{
ptr = ptr + 1;						/* inc pointer */
if (ptr >= DR_NUMWD) {					/* end sector? */
	ptr = 0;					/* new sector */
	sec = sec + 1;					/* adv sector */
	if (sec >= DR_NUMSC) {				/* end track? */
	    sec = 0;					/* new track */
	    trk = trk + 1;				/* adv track */
	    if (trk >= MAX_TRK) trk = 0;  }		/* wraps at max */
	drc_cw = (drc_cw & CW_WR) | CW_PUTTRK (trk) | CW_PUTSEC (sec);
	}
return ptr;
}

/* Read the sector counter

   The hardware sector counter contains the number of the next sector that will
   pass under the heads (so it is one ahead of the current sector).  For the
   duration of the last sector of the track, the sector counter contains 90 for
   the 12606 and 0 for the 12610.  The sector counter resets to 0 at track
   origin and increments at the start of the first sector.  Therefore, the
   counter value ranges from 0-90 for the 12606 and 0-31 for the 12610.  The 0
   state is quite short in the 12606 and long in the 12610, relative to the
   other sector counter states.

   The simulated sector counter is calculated from the simulation time, based on
   the time per word and the number of words per track.
*/

int32 dr_seccntr (double simtime)
{
int32 curword;

curword = (int32) fmod (simtime / (double) dr_time,
			(double) (DR_NUMWD * DR_NUMSC + DR_OVRHEAD));
if (curword <= DR_OVRHEAD) return 0;
else return ((curword - DR_OVRHEAD) / DR_NUMWD +
	     ((drc_unit.flags & UNIT_DR)? 0: 1));
}

/* Reset routine */

t_stat drc_reset (DEVICE *dptr)
{
hp_enbdis_pair (dptr,					/* make pair cons */
	(dptr == &drd_dev)? &drc_dev: &drd_dev);
drc_sta = drc_cw = drd_ptr = 0;
drc_dib.cmd = drd_dib.cmd = 0;				/* clear cmd */
drc_dib.ctl = drd_dib.ctl = 0;				/* clear ctl */
drc_dib.fbf = drd_dib.fbf = 0;				/* clear fbf */
drc_dib.flg = drd_dib.flg = 0;				/* clear flg */
drc_dib.srq = drd_dib.srq = 0;				/* srq follows flg */
sim_cancel (&drc_unit);
sim_cancel (&drd_unit[TMR_ORG]);
sim_cancel (&drd_unit[TMR_INH]);
return SCPE_OK;
}

/* Attach routine */

t_stat drc_attach (UNIT *uptr, char *cptr)
{
int32 sz = sz_tab[DR_GETSZ (uptr->flags)];

if (sz == 0) return SCPE_IERR;
uptr->capac = sz;
return attach_unit (uptr, cptr);
}

/* Set protected track count */

t_stat dr_set_prot (UNIT *uptr, int32 val, char *cptr, void *desc)
{
int32 count;
t_stat status;

if (cptr == NULL) return SCPE_ARG;
count = (int32) get_uint (cptr, 10, 768, &status);
if (status != SCPE_OK) return status;
else switch (count) {
	case 1:
	case 2:
	case 4:
	case 8:
	case 16:
	case 32:
	case 64:
	case 128:
		drc_pcount = count;
		break;
	case 256:
	case 512:
	case 768:
		if (drc_unit.flags & UNIT_DR) drc_pcount = count;
		else return SCPE_ARG;
		break;
	default:
		return SCPE_ARG;  }
return SCPE_OK;
}

/* Set size routine */

t_stat dr_set_size (UNIT *uptr, int32 val, char *cptr, void *desc)
{
int32 sz;
int32 szindex;

if (val < 0) return SCPE_IERR;
if ((sz = sz_tab[szindex = DR_GETSZ (val)]) == 0) return SCPE_IERR;
if (uptr->flags & UNIT_ATT) return SCPE_ALATT;
uptr->capac = sz;
if (szindex & UNIT_DR) dr_time = DR_DTIME;		/* drum */
else {
    dr_time = DR_FTIME;					/* disk */
    if (drc_pcount > 128) drc_pcount = 128;  }		/* max prot track count */
return SCPE_OK;
}

/* Fixed head disk/drum bootstrap routine (disc subset of disc/paper tape loader) */

#define BOOT_BASE	056
#define BOOT_START	060

static const uint16 dr_rom[IBL_LNT - BOOT_BASE] = {
	0020010,		/*DMA 20000+DC */
	0000000,		/*    0 */
	0107700,		/*    CLC 0,C */
	0063756,		/*    LDA DMA		; DMA ctrl */
	0102606,		/*    OTA 6 */
	0002700,		/*    CLA,CCE */
	0102611,		/*    OTA CC		; trk = sec = 0 */
	0001500,		/*    ERA		; A = 100000 */
	0102602,		/*    OTA 2		; DMA in, addr */
	0063777,		/*    LDA M64 */
	0102702,		/*    STC 2 */
	0102602,		/*    OTA 2		; DMA wc = -64 */
	0103706,		/*    STC 6,C		; start DMA */
	0067776,		/*    LDB JSF		; get JMP . */
	0074077,		/*    STB 77		; in base page */
	0102710,		/*    STC DC		; start disc */
	0024077,		/*JSF JMP 77		; go wait */
	0177700 };		/*M64 -100 */

t_stat drc_boot (int32 unitno, DEVICE *dptr)
{
int32 i, dev, ad;
uint16 wd;

if (unitno != 0) return SCPE_NOFNC;			/* only unit 0 */
dev = drd_dib.devno;					/* get data chan dev */
ad = ((MEMSIZE - 1) & ~IBL_MASK) & VAMASK;		/* start at mem top */
for (i = BOOT_BASE; i < IBL_LNT; i++) {			/* copy bootstrap */
	wd = dr_rom[i - BOOT_BASE];			/* get word */
	if (((wd & I_NMRMASK) == I_IO) &&		/* IO instruction? */
	    ((wd & I_DEVMASK) >= 010) &&		/* dev >= 10? */
	    (I_GETIOOP (wd) != ioHLT))			/* not a HALT? */
	    M[ad + i] = (wd + (dev - 010)) & DMASK;
	else M[ad + i] = wd;  }
PC = ad + BOOT_START;	
return SCPE_OK;
}
