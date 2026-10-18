#ifndef PORT_STUBS_H
#define PORT_STUBS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Called by every auto-generated stub (stubs_auto.cpp). Logs the symbol name the
 * first time it is hit and returns 0. */
int port_missing_stub(const char* name);

#ifdef __cplusplus
}
#endif

#endif
