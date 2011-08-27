#ifndef HOBBY

/* 
  AMLC I/O operations:

  OCP '0054 - stop clock
  OCP '0154 - single step clock

  OCP '1234 - set normal/DMT mode
  OCP '1354 - set diagnostic/DMQ mode
  OCP '1554 - enable interrupts
  OCP '1654 - disable interrupts
  OCP '1754 - initialize AMLC

  SKS '0454 - skip if NOT interrupting

  INA '0054 - input data set sense bit for all lines (read clear to send)
  INA '0754 - input AMLC status and clear (always skips)
  INA '1154 - input AMLC controller ID
  INA '1454 - input DMA/C channel number
  INA '1554 - input DMT/DMQ base address
  INA '1654 - input interrupt vector address

  OTA '0054 - output line number for INA '0054 (older models only)
  OTA '0154 - output line configuration for 1 line
  OTA '0254 - output line control for 1 line
  OTA '1454 - output DMA/C channel number
  OTA '1554 - output DMT/DMQ base address
  OTA '1654 - output interrupt vector address
  OTA '1754 - output programmable clock constant

  Primos AMLC usage:

  OCP '17xx
  - initialize controller
  - clear all registers and flip-flops
  - start line clock
  - clear all line control bits during the 1st line scan
  - responds not ready until 1 line scan is complete

  INA '11xx
  - read AMLC ID
  - emulator always returns '20054 (DMQ, 16 lines)

  OTA '17xx
  - set AMLC programmable clock constant
  - ignored by the emulator

  OTA '14xx
  - set DMC channel address for double input buffers
  - emulator stores in a structure

  OCP '13xx
  - set dmq mode (used to be "set diagnostic mode")
  - ignored by the emulator

  OTA '15xx
  - set DMT Base Address
  - also used for DMQ address?
  - emulator stores in a structure

  OTA '16xx
  - set interrupt vector address
  - emulator stores in a structure

  OTA '01xx
  - set line configuration
  - emulator ignores: all lines are 8-bit raw

  OTA '02xx
  - set line control
  - emulator ignores: all lines are enabled

  OCP '15xx/'16xx
  - enable/disable interrupts
  - emulator stores in a structure
*/

/* this macro closes an AMLC connection - used in several places */

#define AMLC_CLOSE_LINE \
  /* printf("em: closing AMLC line %d on device '%o\n", lx, device); */ \
  write(dc[dx].fd[lx], "\r\nPrime session disconnected\r\n", 30); \
  close(dc[dx].fd[lx]); \
  dc[dx].fd[lx] = -1; \
  dc[dx].dss &= ~BITMASK16(lx+1); \
  dc[dx].connected &= ~BITMASK16(lx+1);

/* macro to setup the next AMLC poll */

#define AMLC_SET_POLL \
  if ((dc[dx].ctinterrupt || dc[dx].xmitenabled || (dc[dx].recvenabled & dc[dx].connected))) \
    if (devpoll[device] == 0 || devpoll[device] > AMLCPOLL*gvp->instpermsec/pollspeedup) \
      devpoll[device] = AMLCPOLL*gvp->instpermsec/pollspeedup;  /* setup another poll */

int devamlc (int class, int func, int device) {

#define MAXLINES 128
#define MAXBOARDS 8
#define MAXROOM 1024

  /* AMLC poll rate (ms).  Max data rate = queue size*1000/AMLCPOLL
     The max AMLC output queue size is 1023 (octal 2000), so a poll
     rate of 33 (1000/33 = 30 times per second) will generate about
     31,000 chars per second.  This rate may be further boosted if
     there are lines DMQ buffers with 255 or more characters. */

#define AMLCPOLL 100

  /* DSSCOUNTDOWN is the number of carrier status requests that should
     occur before polling real serial devices.  Primos does a carrier
     check 5x per second.  All this is really good for is disconnecting
     logged-out terminals, so we poll the real status every 5 seconds. */

#define DSSCOUNTDOWN 25

#if 1
  #define QAMLC 020000   /* this is to enable QAMLC/DMQ functionality */
#else
  #define QAMLC 0
#endif

  /* connection types for each line.  This _doesn't_ imply the line is
     actually connected, ie, an AMLC line may be tied to a specific 
     serial device (like a USB->serial gizmo), but the USB device may
     not be plugged in.  The connection type would be CT_SERIAL but
     the line's fd would be zero and the "connected" bit would be 0 */
     
#define CT_SOCKET 1
#define CT_SERIAL 2

  /* terminal states needed to process telnet connections */

#define TS_DATA 0      /* data state, looking for IAC */
#define TS_IAC 1       /* have seen initial IAC */
#define TS_SUBOPT 2    /* inside a suboption */
#define TS_OPTION 3    /* inside an option */

  static short inited = 0;
  static int pollspeedup = 1;
  static int baudtable[16] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};
  static char ttymsg[1024];
  static int ttymsglen = 0;
  static int tsfd;                      /* socket fd for terminal server */

  static struct {
    unsigned short deviceid;            /* this board's device ID */
    unsigned short dmcchan;             /* DMC channel (for input) */
    unsigned short baseaddr;            /* DMT/Q base address (for output) */
    unsigned short intvector;           /* interrupt vector */
    unsigned short intenable;           /* interrupts enabled? */
    unsigned short interrupting;        /* am I interrupting? */
    unsigned short xmitenabled;         /* 1 bit per line */
    unsigned short recvenabled;         /* 1 bit per line */
    unsigned short ctinterrupt;         /* 1 bit per line */
    unsigned short dss;                 /* 1 bit per line */
    unsigned short connected;           /* 1 bit per line */
    unsigned short dedicated;           /* 1 bit per line */
    unsigned short dsstime;             /* countdown to dss poll */
             short fd[16];              /* Unix fd, 1 per line */
    unsigned short tstate[16];          /* telnet state */
    unsigned short room[16];            /* room (chars) left in input buffer */
    unsigned short lconf[16];           /* line configuration word */
    unsigned short ctype[16];           /* connection type for each line */
    unsigned short modemstate[16];      /* Unix modem state bits (serial) */
    unsigned short recvlx;              /* next line to check for recv data */
    unsigned short pclock;              /* programmable clock */
    char dmqmode;                       /* 0=DMT, 1=DMQ */
    char bufnum;                        /* 0=1st input buffer, 1=2nd */
    char eor;                           /* 1=End of Range on input */
  } dc[MAXBOARDS];

  int dx, dx2, lx, lcount;
  unsigned short utempa;
  unsigned int utempl;
  ea_t qcbea, dmcea, dmcbufbegea, dmcbufendea;
  unsigned short dmcnw;
  int dmcpair;
  int optval;
  int tsflags;
  struct sockaddr_in addr;
  int fd;
  unsigned int addrlen;
  unsigned char buf[1024];      /* max size of DMQ buffer */
  int i, n, maxn, n2, nw;
  fd_set fds;
  struct timeval timeout;
  unsigned char ch;
  int state;
  int msgfd;
  int allbusy;
  unsigned short qtop, qbot, qtemp;
  unsigned short qseg, qmask, qents;
  ea_t qentea;
  char line[100];
  int lc;
  FILE *cfgfile;
  char devname[32];
  int baud;
  struct termios terminfo;
  int modemstate;
  int maxxmit;
  int tcpoptval;

  /* save this board's device id in the dc[] array so we can tell
     what order the boards should be in.  This is necessary to find
     the clock line: the line that controls the AMLC poll rate.  The
     last line on the last defined board is the clock line.  The
     emulator also assumes that there are no gaps in the AMLC board
     configuration, which I think is also a Primos requirement, ie,
     you can't have board '54 and '52, with '53 missing. */

  switch (device) {
  case 054: dx = 0; break;
  case 053: dx = 1; break;
  case 052: dx = 2; break;
  case 035: dx = 3; break;
  case 015: dx = 4; break;
  case 016: dx = 5; break;
  case 017: dx = 6; break;
  case 032: dx = 7; break;
  default:
    fprintf(stderr, "devamlc: non-AMLC device id '%o ignored\n", device);
    return -1;
  }

  switch (class) {

  case -1:

    /* this part of initialization only occurs once, no matter how
       many AMLC boards are configured.  Parts of the amlc device
       context that are emulator-specfic need to be initialized here,
       only once, because Primos may issue the OCP to initialize an
       AMLC board more than once.  This would interfere with the
       dedicated serial line setup. */

    if (!inited) {

      /* initially, we don't know about any AMLC boards */

      for (dx2=0; dx2<MAXBOARDS; dx2++) {
	dc[dx2].deviceid = 0;
	for (lx = 0; lx < 16; lx++) {
	  dc[dx2].connected = 0;
	  dc[dx2].dedicated = 0;
	  for (lx = 0; lx < 16; lx++) {
	    dc[dx2].fd[lx] = -1;
	    dc[dx2].tstate[lx] = TS_DATA;
	    dc[dx2].room[lx] = 64;
	    dc[dx2].lconf[lx] = 0;
	    dc[dx2].ctype[lx] = CT_SOCKET;
	    dc[dx2].modemstate[lx] = 0;
	  }
	  dc[dx2].recvlx = 0;
	}
      }

      /* read the AMLC file, to see if any lines should be connected to
	 real serial devices.  This file has the format:
	   <line #> <Unix device name>
         The entries can be in any order.  If the line number begins with
	 a zero, it is assumed to be octal.  If no zero, then decimal.
      */

      if ((cfgfile = fopen("amlc", "r")) == NULL) {
	if (errno != ENOENT)
	  printf("em: error opening amlc config file: %s", strerror(errno));
      } else {
	lc = 0;
	while (fgets(line, sizeof(line), cfgfile) != NULL) {
	  lc++;
	  line[sizeof(devname)] = 0;   /* don't let sscanf overwrite anything */
	  if (line[0] == '0')
	    n = sscanf(line, "%o %s", &i, devname);
	  else
	    n = sscanf(line, "%d %s", &i, devname);
	  if (n != 2) {
	    printf("em: Can't parse amlc config file line #%d: %s\n", lc, line);
	    continue;
	  }
	  if (i < 0 || i >= MAXLINES) {
	    printf("em: amlc line # '%o (%d) out of range in amlc config file at line #%d: %s\n", i, i, lc, line);
	    continue;
	  }
	  //printf("devamlc: lc=%d, line '%o (%d) set to device %s\n", lc, i, i, devname);
	  dx2 = i/16;
	  lx = i & 0xF;
#if 1
	  if ((fd = open(devname, O_RDWR | O_NONBLOCK | O_EXLOCK)) == -1) {
#else
	  if ((fd = open(devname, O_RDWR | O_NONBLOCK)) == -1) {
#endif
	    printf("em: error connecting AMLC line '%o (%d) to device %s: %s\n", i, i, devname, strerror(errno));
	    continue;
	  }
	  printf("em: connected AMLC line '%o (%d) to device %s\n", i, i, devname);
	  dc[dx2].fd[lx] = fd;
	  dc[dx2].connected |= BITMASK16(lx+1);
	  dc[dx2].dedicated |= BITMASK16(lx+1);
	  dc[dx2].ctype[lx] = CT_SERIAL;
	}
	fclose(cfgfile);
      }

      /* start listening for incoming telnet connections */

      if (tport != 0) {
	tsfd = socket(AF_INET, SOCK_STREAM, 0);
	if (tsfd == -1) {
	  perror("socket failed for AMLC");
	  fatal(NULL);
	}
	optval = 1;
	if (setsockopt(tsfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval))) {
	  perror("setsockopt failed for AMLC");
	  fatal(NULL);
	}
	addr.sin_family = AF_INET;
	addr.sin_port = htons(tport);
	addr.sin_addr.s_addr = INADDR_ANY;
	if(bind(tsfd, (struct sockaddr *)&addr, sizeof(addr))) {
	  perror("bind: unable to bind for AMLC");
	  fatal(NULL);
	}
	if(listen(tsfd, 10)) {
	  perror("listen failed for AMLC");
	  fatal(NULL);
	}
	if ((tsflags = fcntl(tsfd, F_GETFL)) == -1) {
	  perror("unable to get ts flags for AMLC");
	  fatal(NULL);
	}
	tsflags |= O_NONBLOCK;
	if (fcntl(tsfd, F_SETFL, tsflags) == -1) {
	  perror("unable to set ts flags for AMLC");
	  fatal(NULL);
	}
      } else
	fprintf(stderr, "-tport is zero, AMLC devices not started\n");
      inited = 1;
    }

    /* this part of initialization occurs for every AMLC board */

    if (!inited || tport == 0)
      return -1;

    dc[dx].deviceid = device;
    return 0;

  case 0:
    TRACE(T_INST, " OCP '%02o%02o\n", func, device);
    //printf(" OCP '%02o%02o\n", func, device);

    if (func == 012) {            /* set normal (DMT) mode */
      dc[dx].dmqmode = 0;

    } else if (func == 013 && QAMLC) {     /* set diagnostic (DMQ) mode */
      dc[dx].dmqmode = 1;

    } else if (func == 015) {     /* enable interrupts */
      dc[dx].intenable = 1;

    } else if (func == 016) {     /* disable interrupts */
      dc[dx].intenable = 0;

    } else if (func == 017) {     /* initialize AMLC */
      //printf("devamlc: Initializing controller '%d, dx=%d\n", device, dx);
      dc[dx].dmcchan = 0;
      dc[dx].baseaddr = 0;
      dc[dx].intvector = 0;
      dc[dx].intenable = 0;
      dc[dx].interrupting = 0;
      dc[dx].xmitenabled = 0;
      dc[dx].recvenabled = 0;
      dc[dx].ctinterrupt = 0;
      dc[dx].dss = 0;      /* NOTE: 1=asserted in emulator, 0=asserted on Prime */
      dc[dx].dsstime = DSSCOUNTDOWN;
      dc[dx].pclock = 0;
      dc[dx].dmqmode = 0;
      dc[dx].bufnum = 0;
      dc[dx].eor = 0;

    } else {
      printf("Unimplemented OCP device '%02o function '%02o\n", device, func);
      fatal(NULL);
    }
    break;

  case 1:
    TRACE(T_INST, " SKS '%02o%02o\n", func, device);

    if (func == 04) {             /* skip if not interrupting */
      if (!dc[dx].interrupting)
	IOSKIP;

    } else {
      printf("Unimplemented SKS device '%02o function '%02o\n", device, func);
      fatal(NULL);
    }
    break;

  case 2:
    TRACE(T_INST, " INA '%02o%02o\n", func, device);

    /* XXX: this constant is redefined because of a bug in the
       OSX Prolific USB serial driver at version 1.2.1r2.  They should be
       turning on bit 0100, but are turning on 0x0100. */
#define TIOCM_CD 0x0100    

    if (func == 00) {              /* input Data Set Sense (carrier) */
      if (dc[dx].dedicated) {      /* any serial connections? */
	if (--dc[dx].dsstime == 0) {
	  dc[dx].dsstime = DSSCOUNTDOWN;
	  for (lx = 0; lx < 16; lx++) {  /* yes, poll them */
	    if (dc[dx].ctype[lx] == CT_SERIAL) {
	      if (ioctl(dc[dx].fd[lx], TIOCMGET, &modemstate))
		perror("devamlc: unable to get modem state");
	      else if (modemstate & TIOCM_CD)
		dc[dx].dss |= BITMASK16(lx+1);
	      else
		dc[dx].dss &= ~BITMASK16(lx+1);
	      if (modemstate != dc[dx].modemstate[lx]) {
		//printf("devamlc: line %d modemstate was '%o now '%o\n", lx, dc[dx].modemstate[lx], modemstate);
		dc[dx].modemstate[lx] = modemstate;
	      }
	    }
	  }
	}
      }
      //printf("devamlc: dss for device '%o = 0x%x\n", device, dc[dx].dss);
      crs[A] = ~dc[dx].dss;      /* to the outside world, 1 = no carrier */
      IOSKIP;

    } else if (func == 07) {       /* input AMLC status */
      crs[A] = 040000 | (dc[dx].bufnum<<8) | (dc[dx].intenable<<5) | (dc[dx].dmqmode<<4);
      if (dc[dx].eor) {
	crs[A] |= 0100000;
	dc[dx].eor = 0;
      }
      if (dc[dx].ctinterrupt)
	if (dc[dx].ctinterrupt & 0xfffe)
	  crs[A] |= 0xcf;          /* multiple char time interrupt */
	else
	  crs[A] |= 0x8f;          /* last line cti */
      dc[dx].interrupting = 0;
      //printf("INA '07%02o returns 0x%x\n", device, crs[A]);
      IOSKIP;

    } else if (func == 011) {      /* input ID */
      crs[A] = QAMLC | 054;
      IOSKIP;

    } else {
      printf("Unimplemented INA device '%02o function '%02o\n", device, func);
      fatal(NULL);
    }
    break;

  case 3:
    TRACE(T_INST, " OTA '%02o%02o\n", func, device);

    /* func 00 = Output Line # to read Data Set Status, only implemented
       on 2-board AMLC sets with full data set control (uncommon) */

    /* func 01 = Set Line Configuration.  Primos issues this on
       logged-out lines to drop DTR so it can test carrier.  It also
       drops DTR (configurable) after logout to physically disconnect
       the line, either telnet or over a modem */

    if (func == 01) {              /* set line configuration */
      lx = crs[A] >> 12;
      //printf("OTA '01%02o: AMLC line %d new config = '%o, old config = '%o\n", device, lx, crs[A] & 0xFFF, dc[dx].lconf[lx] & 0xFFF);

      switch (dc[dx].ctype[lx]) {

      case CT_SOCKET:
	if (!(crs[A] & 0x400) && dc[dx].fd[lx] >= 0) {  /* if DTR drops, disconnect */
	  AMLC_CLOSE_LINE;
	}
	break;

	
      case CT_SERIAL:
	    
	/* setup line characteristics if they have changed (check for
	   something other than DTR changing) */

	fd = dc[dx].fd[lx];
	if ((crs[A] ^ dc[dx].lconf[lx]) & ~02000) {
	  //printf("devamlc: serial config changed!\n");
	  if (tcgetattr(fd, &terminfo) == -1) {
	    fprintf(stderr, "devamlc: unable to get terminfo for device '%o line %d", device, lx);
	    break;
	  }
	  memset(&terminfo, 0, sizeof(terminfo));
	  cfmakeraw(&terminfo);
	  baud = baudtable[(crs[A] >> 6) & 7];
	  cfsetspeed(&terminfo, baud);
	  terminfo.c_cflag = CREAD | CLOCAL;		// turn on READ and ignore modem control lines
	  switch (crs[A] & 3) {              /* data bits */
	  case 0:
	    terminfo.c_cflag |= CS5;
	    break;
	  case 1:
	    terminfo.c_cflag |= CS7;    /* this is not a typo! */
	    break;
	  case 2:
	    terminfo.c_cflag |= CS6;    /* this is not a typo! */
	    break;
	  case 3:
	    terminfo.c_cflag |= CS8;
	    break;
	  }
	  if (crs[A] & 020)
	    terminfo.c_cflag |= CSTOPB;
	  if (!(crs[A] & 010)) {
	    terminfo.c_cflag |= PARENB;
	    if (!(crs[A] & 4))
	      terminfo.c_cflag |= PARODD;
	  }

#if 0
	  /* on the Prime AMLC, flow control is done in software and is
	     specified with the AMLC "lword" - an AMLDIM (software)
	     variable.  To enable Unix xon/xoff, RTS/CTS, and DTR flow
	     control, bits 5 & 7 of the lconf word have been taken over by
	     the emulator.  Bit 6 is the state of DTR.  The values for
	     bits 5-7 are:

	     0x0 - no low-level flow control, like the Prime
	     0x1 - xon/xoff flow control (2413 becomes 3413)
	     1x0 - cts/rts flow control (2413 becomes 6413)
	     1x1 - dsr/dtr flow control (2413 becomes 7413)

	     NOTE: bit 11 also appears to be free, but Primos doesn't 
	     let it flow through to the AMLC controller. :(
	  */

	  switch ((crs[A] >> 9) & 7) {
	  case 1: case 3:
	    terminfo.c_iflag |= IXON | IXOFF;
	    terminfo.c_cc[VSTART] = 0x11;
	    terminfo.c_cc[VSTOP] = 0x13;
	    break;
	  case 4: case 6:
	    terminfo.c_cflag |= CCTS_OFLOW | CRTS_IFLOW;
	    break;
	  case 5: case 7:
	    terminfo.c_cflag |= CDSR_OFLOW | CDTR_IFLOW;
	    break;
	  }
#else
	  /* for now, use bit 7: 2413 -> 3413 to enable Unix
	     (hardware) xon/xoff.  This is much more responsive than
	     Primos xon/xoff, but can't be used for user terminals
	     because software (Emacs) may want to disable xon/xoff,
	     and since it is really an lword (software) control set
	     with DUPLX$, the hardware control word we're using won't
	     get changed.  And xon/xoff will still be enabled.  This
	     Unix xon/xoff feature is good for serial I/O devices,
	     where it stays enabled. */

	  if (crs[A] & 01000) {
	    terminfo.c_iflag |= IXON | IXOFF;
	    terminfo.c_cc[VSTART] = 0x11;
	    terminfo.c_cc[VSTOP] = 0x13;
	  }
#endif

#if 0
	  printf("em: set terminfo: iFlag %x  oFlag %x  cFlag %x  lFlag %x  speed %d\n",
		 terminfo.c_iflag,
		 terminfo.c_oflag,
		 terminfo.c_cflag,
		 terminfo.c_lflag,
		 terminfo.c_ispeed);
#endif
	  if (tcsetattr(fd, TCSANOW, &terminfo) == -1) {
	    fprintf(stderr, "devamlc: unable to set terminal attributes for device %s\n", devname);
	    perror("devamlc error");
	  }
	}

	/* set DTR high (02000) or low if it has changed */

	if ((crs[A] ^ dc[dx].lconf[lx]) & 02000) {
	  //printf("devamlc: DTR state changed\n");
	  ioctl(fd, TIOCMGET, &modemstate);
	  if (crs[A] & 02000)
	    modemstate |= TIOCM_DTR;
	  else {
	    modemstate &= ~TIOCM_DTR;
	    dc[dx].dsstime = 1;
	  }
	  ioctl(fd, TIOCMSET, &modemstate);
	}
	break;

      default:
	fatal("devamlc: unrecognized connection type");
      }

      /* finally, update line config */

      dc[dx].lconf[lx] = crs[A];
      IOSKIP;

    } else if (func == 02) {      /* set line control */
      lx = (crs[A]>>12);
      //printf("OTA '02%02o: AMLC line %d control = %x\n", device, lx, crs[A]);
      if (crs[A] & 040)           /* character time interrupt enable/disable */
	dc[dx].ctinterrupt |= BITMASK16(lx+1);
      else
	dc[dx].ctinterrupt &= ~BITMASK16(lx+1);
      if (crs[A] & 010)           /* transmit enable/disable */
	dc[dx].xmitenabled |= BITMASK16(lx+1);
      else
	dc[dx].xmitenabled &= ~BITMASK16(lx+1);
      if (crs[A] & 01)            /* receive enable/disable */
	dc[dx].recvenabled |= BITMASK16(lx+1);
      else
	dc[dx].recvenabled &= ~BITMASK16(lx+1);
      AMLC_SET_POLL;
      IOSKIP;

    } else if (func == 03) {      /* set room in input buffer */
      lx = (crs[A]>>12);
      dc[dx].room[lx] = crs[A] & 0xFFF;
      //printf("OTA '03%02o: AMLC line %d, room=%d, A=0x%04x\n", device, lx, dc[dx].room[lx], crs[A]);
      IOSKIP;

    } else if (func == 014) {      /* set DMA/C channel (for input) */
      dc[dx].dmcchan = crs[A] & 0x7ff;
      //printf("OTA '14%02o: AMLC chan = %o\n", device, dc[dx].dmcchan);
      if (!(crs[A] & 0x800))
	fatal("Can't run AMLC in DMA mode!");
#if 0
	    dmcea = dc[dx].dmcchan;
	  dmcpair = get32io(dmcea);
	  dmcbufbegea = dmcpair>>16;
	  dmcbufendea = dmcpair & 0xffff;
	  dmcnw = dmcbufendea - dmcbufbegea + 1;
	  printf("AMLC: dmcnw=%d\n", dmcnw);
#endif
      IOSKIP;

    } else if (func == 015) {      /* set DMT/DMQ base address (for output) */
      dc[dx].baseaddr = crs[A];
      IOSKIP;

    } else if (func == 016) {      /* set interrupt vector */
      dc[dx].intvector = crs[A];
      IOSKIP;

    } else if (func == 017) {      /* set programmable clock constant */
      dc[dx].pclock = crs[A];
      /* XXX: reprogram baud rate for lines that use the programmable clock */
      IOSKIP;

    } else {
      printf("Unimplemented OTA device '%02o function '%02o, A='%o\n", device, func, crs[A]);
      fatal(NULL);
    }
    break;

  case 4:

    maxxmit = 0;

    //printf("poll device '%o, cti=%x, xmit=%x, recv=%x, dss=%x\n", device, dc[dx].ctinterrupt, dc[dx].xmitenabled, dc[dx].recvenabled, dc[dx].dss);
    
    /* check for 1 new telnet connection on each AMLC poll */

    addrlen = sizeof(addr);
    while ((fd = accept(tsfd, (struct sockaddr *)&addr, &addrlen)) == -1 && errno == EINTR)
      ;
    if (fd == -1) {
      if (errno != EWOULDBLOCK) {
	perror("accept error for AMLC");
      }
    } else {
      allbusy = 1;
      for (i=0; dc[i].deviceid && i<MAXBOARDS; i++)
	for (lx=0; lx<16; lx++) {
	  /* NOTE: don't allow connections on clock line */
	  if (lx == 15 && (i+1 == MAXBOARDS || !dc[i+1].deviceid))
	      break;
	  if (dc[i].fd[lx] < 0 && dc[i].ctype[lx] == CT_SOCKET) {
	    allbusy = 0;
	    dc[i].dss |= BITMASK16(lx+1);
	    dc[i].connected |= BITMASK16(lx+1);
	    dc[i].fd[lx] = fd;
	    dc[i].tstate[lx] = TS_DATA;
	    dc[i].room[lx] = MAXROOM;
	    dc[i].ctype[lx] = CT_SOCKET;
	    //printf("em: AMLC connection, fd=%d, device='%o, line=%d\n", fd, dc[i].deviceid, lx);
	    goto endconnect;
	  }
	}
endconnect:
      if (allbusy) {
	warn("No free AMLC connection");
	write(fd, "\rAll AMLC lines are in use!\r\n", 29);
	close(fd);
      } else {

	if ((tsflags = fcntl(fd, F_GETFL)) == -1) {
	  perror("unable to get ts flags for AMLC line");
	}
	tsflags |= O_NONBLOCK;
	if (fcntl(fd, F_SETFL, tsflags) == -1) {
	  perror("unable to set ts flags for AMLC line");
	}

#if 0
	tcpoptval = 1;
	if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &tcpoptval, sizeof(tcpoptval)) == -1)
	  perror("unable to set TCP_NODELAY");
#endif

	/* these Telnet commands put the connecting telnet client
	   into character-at-a-time mode and binary mode.  Since
	   these probably display garbage for other connection
	   methods, this stuff might be better off in a very thin
	   connection server */
	
	buf[0] = 255;   /* IAC */
	buf[1] = 251;   /* will */
	buf[2] = 1;     /* echo */
	buf[3] = 255;   /* IAC */
	buf[4] = 251;   /* will */
	buf[5] = 3;     /* supress go ahead */
	buf[6] = 255;   /* IAC */
	buf[7] = 253;   /* do */
	buf[8] = 0;     /* binary mode */
	write(fd, buf, 9);

	/* send out the ttymsg greeting */

	if ((msgfd = open("ttymsg", O_RDONLY, 0)) >= 0) {
	  ttymsglen = read(msgfd, ttymsg, sizeof(ttymsg)-1);
	  if (ttymsglen >= 0) {
	    ttymsg[ttymsglen] = 0;
	    write(fd, ttymsg, ttymsglen);
	  }
	  close(msgfd);
	} else if (errno != ENOENT) {
	  perror("Unable to open ttymsg file");
	}
      }
    }

    /* do a transmit scan loop for every line.  This loop is fairly
       efficient because Primos turns off xmitenable on DMQ lines
       after a short time w/o any output.

       NOTE: it's important to do xmit processing even if a line is
       not currently connected, to drain the tty output buffer.
       Otherwise, the DMQ buffer fills, this stalls the tty output
       buffer, and when the next connection occurs on this line, the
       output buffer from the previous terminal session will be
       displayed to the new user. */

    if (dc[dx].connected || dc[dx].xmitenabled) {
      for (lx = 0; lx < 16; lx++) {

	if (dc[dx].xmitenabled & BITMASK16(lx+1)) {
	  n = 0;
	  if (dc[dx].dmqmode) {
	    qcbea = dc[dx].baseaddr + lx*4;
	    if (dc[dx].connected & BITMASK16(lx+1)) {

	      /* this line is connected, determine max chars to write
	         XXX: maxn should scale, depending on the actual line
		 throughput and AMLC poll rate */

	      qtop = get16io(qcbea);
	      qbot = get16io(qcbea+1);
	      if (qtop == qbot)
		continue;        /* queue is empty, try next line */
	      qseg = get16io(qcbea+2);
	      qmask = get16io(qcbea+3);
	      qents = (qbot-qtop) & qmask;
	      maxn = sizeof(buf);
	      /* XXX: for FTDI USB->serial chip, optimal request size
		 is a multiple of 62 bytes, ie, 992 bytes, not 1K */
	      if (qents < maxn)
		maxn = qents;
	      qentea = MAKEVA(qseg & 0xfff, qtop);

	      /* pack DMQ characters into a buffer & fix parity
		 XXX: turning off the high bit at this low level
		 precludes the use of TTY8BIT mode... */

	      n = 0;
	      for (i=0; i < maxn; i++) {
		utempa = MEM[qentea];
		qentea = (qentea & ~qmask) | ((qentea+1) & qmask);
		//printf("Device %o, line %d, entry=%o (%c)\n", device, lx, utempa, utempa & 0x7f);
		buf[n++] = utempa & 0x7F;
	      }
	    } else {         /* no line connected, just drain queue */
	      //printf("Draining output queue on line %d\n", lx);
	      put16io(get16io(qcbea), qcbea+1);
	    }
	  } else {  /* DMT */
	    utempa = get16io(dc[dx].baseaddr + lx);
	    if (utempa != 0) {
	      if ((utempa & 0x8000) && (dc[dx].connected & BITMASK16(lx+1))) {
		//printf("Device %o, line %d, entry=%o (%c)\n", device, lx, utempa, utempa & 0x7f);
		buf[n++] = utempa & 0x7F;
	      }
	    }

	    /* would need to setup DMT xmit poll here, and/or look for
	       char time interrupt.  In practice, DMT isn't used when
	       the AMLC device is configured as a QAMLC */
	  }

	  /* n chars have been packed into buf; see how many we can send */

	  if (n > 0) {
	    nw = write(dc[dx].fd[lx], buf, n);
	    if (nw != n) printf("devamlc: tried %d, wrote %d on line %d\n", n, nw, lx);
	    //printf("devamlc: XMIT tried %d, wrote %d\n", n, nw);
	    if (nw > 0) {

	      /* nw chars were sent; for DMQ, update the queue head
		 top to reflect nw dequeued entries.  For DMT, clear
		 the dedicated cell (only writes 1 char at a time).

		 XXX: Might be good to keep write stats here, to
		 decide how many chars to dequeue above and/or how
		 often to do writes.  There's overhead if large DMQ
		 buffers are used and Unix buffers get full so writes
		 can't complete */

	      if (dc[dx].dmqmode) {
		qtop = (qtop & ~qmask) | ((qtop+nw) & qmask);
		put16io(qtop, qcbea);
	      } else
		put16io(0, dc[dx].baseaddr + lx);
	      if (nw > maxxmit)
		maxxmit = nw;
	    } else if (nw == -1)
	      if (errno == EAGAIN || errno == EWOULDBLOCK)
		;

	      /* on Mac OSX, USB serial ports (Prolific chip) return
	         ENXIO - 'Device not configured" when USB is
	         unplugged.  If it is plugged back in, new device names
	         are created, so there's not much we can do except close
	         the fd. :( */

	      else if (errno == ENXIO && dc[dx].ctype[lx] == CT_SERIAL) {
		warn("USB serial device unplugged!  Reboot host.");
		AMLC_CLOSE_LINE;

	      } else if (errno == EPIPE || errno == ECONNRESET) {
		AMLC_CLOSE_LINE;

	      } else {
		perror("Writing to AMLC");
		fprintf(stderr," %d bytes written, but %d bytes sent\n", n, nw);
	      }
	  }
	}
      }
    }

    /* process input, but only as much as will fit into the DMC
       buffer.  

       Because the size of the AMLC tumble tables is limited, this
       could pose a denial of service issue.  Input is processed in a
       round to be more fair with this limited resource.  Better yet,
       the tumble table space could be apportioned for each line with
       data waiting to be read.

       The AMLC tumble tables should never overflow, because we only
       read as many characters from the socket buffers as will fit in
       the tumble tables. However, the tty line buffers may overflow,
       causing data from the terminal to be dropped. To help avoid
       this, a new OTA "set room left" has been implemented.  If there
       is no room left in the tty input buffer, don't read any more
       characters from the socket for that line.  In case Primos has
       not been modified to use the room left feature, it is
       initialized to MAXROOM for each line, so that at most MAXROOM
       characters are read from a line during a poll.  If MAXROOM is
       small, like 64, this also makes overflow less likely if the
       line input buffer is set to '200 or so with AMLBUF.  To
       optimize transfers to the Prime over AMLC lines, MAXROOM is set
       much higher, to 1024 */

    if (!dc[dx].eor) {
      if (dc[dx].bufnum)
	dmcea = dc[dx].dmcchan ^ 2;
      else
	dmcea = dc[dx].dmcchan;
      dmcpair = get32io(dmcea);
      dmcbufbegea = dmcpair>>16;
      dmcbufendea = dmcpair & 0xffff;
      dmcnw = dmcbufendea - dmcbufbegea + 1;
      lx = dc[dx].recvlx;
      for (lcount = 0; lcount < 16 && dmcnw > 0; lcount++) {
	if ((dc[dx].connected & dc[dx].recvenabled & BITMASK16(lx+1))
	    && dc[dx].room[lx] >= 8) {

	  /* dmcnw is the # of characters left in the dmc buffer, but
	     there may be further size/space restrictions for this line */

	  n2 = dmcnw;
	  if (n2 > sizeof(buf))
	    n2 = sizeof(buf);
	  if (n2 > dc[dx].room[lx])
	    n2 = dc[dx].room[lx];
          if (n2 > MAXROOM)        /* don't let 1 line hog the resource */
	    n2 = MAXROOM;

	  while ((n = read(dc[dx].fd[lx], buf, n2)) == -1 && errno == EINTR)
	    ;
	  //printf("processing recv on device %o, line %d, b#=%d, n2=%d, n=%d\n", device, lx, dc[dx].bufnum, n2, n);

	  /* zero length read means the fd has been closed */

	  if (n == 0) {
	    n = -1;
	    errno = EPIPE;
	  }
	  if (n == -1) {
	    n = 0;
	    if (errno == EAGAIN || errno == EWOULDBLOCK)
	      ;
	    else if (errno == EPIPE || errno == ECONNRESET || errno == ENXIO) {
	      AMLC_CLOSE_LINE;
	    } else {
	      perror("Reading AMLC");
	    }
	  }

	  /* very primitive support here for telnet - only enough to
	     ignore commands sent by the telnet client.  Telnet
	     commands could be split across reads and AMLC interrupts,
	     so a small state machine is used for each line.

	     XXX: need to respond to remote telnet server commands...

	     For direct serial connections, the line stays in TS_DATA
	     state so no telnet processing occurs. */

	  if (n > 0) {
	    //printf("devamlc: RECV dx=%d, lx=%d, b=%d, tried=%d, read=%d\n", dx, lx, dc[dx].bufnum, n2, n);
	    state = dc[dx].tstate[lx];
	    for (i=0; i<n; i++) {
	      ch = buf[i];
	      switch (state) {
	      case TS_DATA:
		if (ch == 255 && dc[dx].ctype[lx] == CT_SOCKET)
		  state = TS_IAC;
		else {
    storech:
		  utempa = lx<<12 | 0x0200 | ch;
		  put16io(utempa, dmcbufbegea);
		  //printf("******* stored character %o (%c) at %o\n", utempa, utempa&0x7f, dmcbufbegea);
		  dmcbufbegea = INCVA(dmcbufbegea, 1);
		  dmcnw--;
		}
		break;
	      case TS_IAC:
		switch (ch) {
		case 255:
		  state = TS_DATA;
		  goto storech;
		case 251:   /* will */
		case 252:   /* won't */
		case 253:   /* do */
		case 254:   /* don't */
		  state = TS_OPTION;
		  break;
		case 250:   /* begin suboption */
		  state = TS_SUBOPT;
		  break;
		default:    /* ignore other chars after IAC */
		  state = TS_DATA;
		}
		break;
	      case TS_SUBOPT:
		if (ch == 255)
		  state = TS_IAC;
		break;
	      case TS_OPTION:
	      default:
		state = TS_DATA;
	      }
	    }
	    dc[dx].tstate[lx] = state;
	  }
	}
	lx = (lx+1) & 0xF;
      }
      dc[dx].recvlx = lx;
      if (dmcbufbegea-1 > dmcbufendea)
	fatal("AMLC tumble table overflowed?");
      put16io(dmcbufbegea, dmcea);
      if (dmcbufbegea > dmcbufendea) {          /* end of range has occurred */
	dc[dx].bufnum = 1-dc[dx].bufnum;
	dc[dx].eor = 1;
      }
    }

    /* time to interrupt?

       XXX: might be a bug here: with multiple controllers, maybe only
       the last controller (with ctinterrupt set) will get high
       performance (higher poll rates) while others will get the
       standard poll rate.  If a non-clock-line controller wants
       faster polling, we probably need to generate an interrupt on
       the clock line controller to cause AMLDIM to refill the
       buffers, or force the ctinterrupt status bit to be returned
       on the next status request.
    */

    if (dc[dx].intenable && (dc[dx].ctinterrupt || dc[dx].eor)) {
      if (gvp->intvec == -1) {
	gvp->intvec = dc[dx].intvector;
	dc[dx].interrupting = 1;
      } else
	devpoll[device] = 100;         /* come back soon! */
    }

    /* conditions to setup another poll:
       - any board with ctinterrupt set on any line is polled
       - any board with xmitenabled on any line is polled (to drain output)
       - any board with recvenabled on a connected line is polled
       - the device must not already be set for a poll

       NOTE: there is always at least one board with ctinterrupt set
       (the last board), so it will always be polling and checking
       for new incoming connections */


    /* the largest DMQ buffer size is 1023 chars.  If any line's DMQ
       buffer is getting filled completely, then we need to poll
       faster to increase throughput.  If the max queue size falls
       below 256, then decrease the interrupt rate.  Anywhere between
       256-1022, leave the poll rate alone.

       XXX NOTE: polling faster only causes AMLDIM to fill buffers
       faster for the last AMLC board (with ctinterrupt set).
    */

#if 1
    if (maxxmit >= 1023) {
      if (pollspeedup < 8) {
	pollspeedup++;
	//printf("%d ", pollspeedup);
	//fflush(stdout);
      }
    } else if (pollspeedup > 1 && maxxmit < 256) {
      pollspeedup--;
      //printf("%d ", pollspeedup);
      //fflush(stdout);
    }

#endif
    AMLC_SET_POLL;
    break;
  }
}
#endif