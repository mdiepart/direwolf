
/* hdlc_send.h */

int hdlc_send_frame (int chan, unsigned char *fbuf, int flen, int bad_fcs, int fx25_xmit_enable);

int hdlc_send_flags (int chan, int flags, int finish);

void hdlc_send_reset_nrzi(int chan);

int hdlc_send_sync_word (int chan, int nsync, int syncword);

/* end hdlc_send.h */


