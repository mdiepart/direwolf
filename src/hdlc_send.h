
/* hdlc_send.h */

int hdlc_send_frame (int chan, unsigned char *fbuf, int flen, int bad_fcs, int fx25_xmit_enable);

int hdlc_send_flags (int chan, int flags, int finish);

void hdlc_send_reset_nrzi(int chan);
/* end hdlc_send.h */


