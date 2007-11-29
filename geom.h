  /* static structure for disk file suffixes and config data */

#ifdef HOBBY
#define MAXDRIVES 1
#define MAXCTRL 1     /* 1 controller supported at device address '26 */

#define NUMGEOM 7

  static struct {
    short model;
    char suffix[5];
    short heads;
    short spt;
    short maxtrack;
  } geom[NUMGEOM] = {
     1,  "80M",  5,   9,  823,
     4,  "68M",  3,   9, 1119,
     5, "158M",  7,   9, 1119,
     6, "160M", 10,   9,  821,
    10,  "84M",  5,   8, 1015,   /* MODEL_4714 */
    11,  "60M",  4,   7, 1020,   /* MODEL_4711 */
    12, "120M",  8,   7, 1020,   /* MODEL_4715 */
};

  static int geomcksum = 358767254;
#else

#define NUMGEOM 24
#define MAXDRIVES 8
#define MAXCTRL 8

  static struct {
    short model;
    char suffix[5];
    short heads;
    short spt;
    short maxtrack;
  } geom[NUMGEOM] = {
     1,  "80M",  5,   9,  823,
     1, "300M", 19,   9,  823,
     0,  "CMD", 20,   9,  823,
     4,  "68M",  3,   9, 1119,
     5, "158M",  7,   9, 1119,
     6, "160M", 10,   9,  821,
     7, "675M", 40,   9,  841,
     7, "600M", 40,   9,  841,
     9, "315M", 19,   9,  823,   /* MODEL_4475 */
    10,  "84M",  5,   8, 1015,   /* MODEL_4714 */
    11,  "60M",  4,   7, 1020,   /* MODEL_4711 */
    12, "120M",  8,   7, 1020,   /* MODEL_4715 */
    13, "496M", 24,  14,  711,   /* MODEL_4735 */
    14, "258M", 17,   6, 1220,   /* MODEL_4719 */
    15, "770M", 23,  19,  848,   /* MODEL_4845 */
    17, "328A", 12,   8, 1641,   /* MODEL_4721 */
    17, "328B", 31, 254,   20,   /* MODEL_4721 (7210 SCSI controller) */
    18, "817M", 15,  19, 1379,   /* MODEL_4860 */
    19, "673M", 31, 254,   42,   /* MODEL_4729 */
    20, "213M", 31, 254,   14,   /* MODEL_4730 */
    22, "421M", 31, 254,   26,   /* MODEL_4731 */
    23, "1.3G", 31, 254,   82,   /* MODEL_4732 */
    24, "1G",   31, 254,   65,   /* MODEL_4734 */
    25, "2G",   31, 254,  122,   /* MODEL_4736 */
};

  static int geomcksum = 17176324;
#endif
