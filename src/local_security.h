#ifndef HP_LOCAL_SECURITY_H
#define HP_LOCAL_SECURITY_H
#include <stdbool.h>
#include <cups/cups.h>
#define HP_AUTH_HEX_SIZE 64
#define HP_AUTH_FILE "access-token"
#define HP_AUTH_USER "hp1010"
bool hp_auth_load(const char *directory, bool create, char token[HP_AUTH_HEX_SIZE + 1]);
/* Build a CUPS request in memory; the credential never enters argv or stdout. */
ipp_t *hp_queue_auth_request(const char *token, bool enable);
int hp_secure_queue(const char *directory, bool enable);
#endif
