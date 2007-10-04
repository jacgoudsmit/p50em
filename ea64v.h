/* this version is derived from the flowchart in the preliminary P400
   release notes */

static inline ea_t ea64v (unsigned short inst, ea_t earp) {

  ea_t ea;                                       /* full seg/word va */
  unsigned short ea_s;                           /* eff address segno */
  unsigned short ea_r;                           /* eff address ring */
  unsigned short ea_w;                           /* eff address wordno */
  unsigned short br;
  unsigned short i;
  unsigned short x;
  unsigned short y;
  unsigned short xok;
  unsigned short a;
  unsigned short ixy;
  unsigned short m;
  unsigned short rph,rpl;


  x = ((inst & 036000) != 032000) ? (inst & 040000) : 0;

  /* earp is usually = RPH/RPL in the register file, except for the
     case of an XEC instruction; in that case, earp points to 1
     after the instruction being executed */

  rph = earp >> 16;
  rpl = earp & 0xFFFF;
  //TRACE(T_EAV, " inst=%o, rph=%o, rpl=%o\n", inst, rph, rpl);

  ea_s = rph;

  /* first check for direct short-form - the most commonly occurring case */

  if ((inst & 0101000) == 0) {
    ea_w = (inst & 0777);
    if (x) {
      TRACE(T_EAV, " Postindex, old ea_w=%o, X='%o/%d\n", ea_w, crs[X], *(short *)(crs+X));
      ea_w += crs[X];
      TRACE(T_EAV, " Postindex, new ea_w=%o\n", ea_w);
    }
    if (inst & 0400) {
      TRACE(T_EAV, " Short LB relative, LB=%o/%o\n", crs[LBH], crs[LBL]);
      eap = &gvp->brp[LBBR];
#if 1
      ea_s = crs[LBH] | (ea_s & RINGMASK16);
      ea_w += crs[LBL];
      return MAKEVA(ea_s, ea_w);
#else
      ea_w += crs[LBL];
      return (((*(int *)(crs+LBH) | (earp & RINGMASK32)) & 0xFFFF0000) | ea_w);
#endif
    }
    if (ea_w >= gvp->livereglim) {
      eap = &gvp->brp[SBBR];
      ea_s = crs[SBH] | (ea_s & RINGMASK16);
      ea_w += crs[SBL];
      TRACE(T_EAV, " Short SB relative, SB=%o/%o\n", crs[SBH], crs[SBL]);
      return MAKEVA(ea_s, ea_w);
    }
    TRACE(T_EAV, " Live register '%o\n", ea_w);
    return 0x80000000 | ea_w;
  }

  i = inst & 0100000;           /* indirect is bit 1 (left/MS bit) */

  if (inst & 001000)                             /* sector bit 7 set? */
    if ((inst & 0740) != 0400) {                 /* PC relative? */
      ea_w = rpl + (((short) (inst << 7)) >> 7);   /* yes, sign extend D */
      TRACE(T_EAV, " PC relative, P=%o, new ea_w=%o\n", rpl, ea_w);
      eap = &gvp->brp[RPBR];
    }
    else 
      goto labB;                                 /* special cases */
  else if (i) {
    eap = &gvp->brp[S0BR];
    ea_w = (inst & 0777);                        /* sector 0 */
    TRACE(T_EAV, " Sector 0, new ea_w=%o\n", ea_w);
    if (ea_w < 0100 && x) {                      /* preindex by X */
      TRACE(T_EAV, " Preindex, ea_w=%o, X='%o/%d\n", ea_w, crs[X], *(short *)(crs+X));
      ea_w += crs[X];
      TRACE(T_EAV, " Preindex, new ea_w=%o\n", ea_w);
      x = 0;
    }
  } else 
    fatal("goto labA?");

  if (i) {
    if (ea_w >= gvp->livereglim) {
      TRACE(T_EAV, " Indirect, ea_s=%o, ea_w=%o\n", ea_s, ea_w);
      ea_w = get16(MAKEVA(ea_s, ea_w));
    } else {
      TRACE(T_EAV, " Indirect through live register '%o\n", ea_w);
      ea_w = get16trap(ea_w);
    }
    TRACE(T_EAV, " After indirect, new ea_w=%o\n", ea_w);
  }
  if (x) {
    TRACE(T_EAV, " Postindex, old ea_w=%o, X='%o/%d\n", ea_w, crs[X], *(short *)(crs+X));
    ea_w += crs[X];
    TRACE(T_EAV, " Postindex, new ea_w=%o\n", ea_w);
  }

  if (ea_w >= gvp->livereglim) {
    if ((ea_w ^ RPL) & 0xFC00)
      eap = &gvp->brp[PBBR];
    else
      eap = &gvp->brp[RPBR];
    return MAKEVA(ea_s, ea_w);
  }

  TRACE(T_EAV, " Live register '%o\n", ea_w);
  return 0x80000000 | ea_w;


  /* here for long, 2-word, V-mode memory reference */

labB:
  a = iget16(RP);
  INCRP;
  TRACE(T_EAV, " 2-word format, a=%o\n", a);
  ixy = (i >> 13) | (x >> 13) | ((inst & 020) >> 4);
  xok = (inst & 036000) != 032000;        /* true if indexing is okay */

  br = (inst & 3);
  eap = &gvp->brp[br];

#ifndef NOTRACE
  int opcode;

  opcode = ((inst & 036000) != 032000) ? ((inst & 036000) >> 4) : ((inst & 076000) >> 4);
  opcode |= ((inst >> 2) & 3);         /* opcode extension */
  TRACE(T_EAV, " new opcode=%5#0o, y=%d, br=%d, ixy=%d, xok=%d\n", opcode, (y != 0), br, ixy, xok);
#endif

  ea_s = crs[PBH+br*2] | (ea_s & RINGMASK16);
  ea_w = crs[PBL+br*2] + a;

  if (xok)
    if (ixy == 1 || ixy == 4)
      ea_w += crs[Y];
    else if (ixy == 2 || ixy == 6)
      ea_w += crs[X];

  /* if this is a PB% address, use RPBR instead if it's in range */

  if (br == 0 && ((((ea_s & 0x8FFF) << 16) | (ea_w & 0xFC00)) == gvp->brp[RPBR].vpn))
    eap = &gvp->brp[RPBR];

  if (ixy >= 3) {
    ea = MAKEVA(ea_s, ea_w);
    TRACE(T_EAV, " Long indirect, ea=%o/%o, ea_s=%o, ea_w=%o\n", ea>>16, ea&0xFFFF, ea_s, ea_w);
    m = get16(ea);
    if (m & 0x8000)
      fault(POINTERFAULT, m, ea);
    ea_s = m | (ea_s & RINGMASK16);
    ea_w = get16(INCVA(ea,1));
    TRACE(T_EAV, " After indirect, ea_s=%o, ea_w=%o\n", ea_s, ea_w);

    /* when passing stack variables, callee references will be
       SB%+20,*, which may still be in the same page.  Don't switch to
       UNBR if the new ea is still in the current page */

    if ((((ea_s & 0x8FFF) << 16) | (ea_w & 0xFC00)) != (eap->vpn & 0x0FFFFFFF))
      eap = &gvp->brp[UNBR];
  }
  if (xok)
    if (ixy == 5) {
      TRACE(T_EAV, " Postindex, old ea_w=%o, Y='%o/%d\n", ea_w, crs[Y], *(short *)(crs+Y));
      ea_w += crs[Y];
    } else if (ixy == 7) {
      TRACE(T_EAV, " Postindex, old ea_w=%o, X='%o/%d\n", ea_w, crs[X], *(short *)(crs+X));
      ea_w += crs[X];
    }
  return MAKEVA(ea_s, ea_w);
}
