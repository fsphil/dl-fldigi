#ifndef LOOKUPCALL_H
#define LOOKUPCALL_H

extern void CALLSIGNquery();
enum qrz_query_t { QRZ_EXIT = -1, QRZNONE, QRZCD, QRZNET, QRZHTML, HAMCALLNET, HAMCALLHTML };

#endif