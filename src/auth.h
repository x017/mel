#ifndef MEL_AUTH_H
#define MEL_AUTH_H
/* Returns 1 if PAM accepts the password for user, 0 otherwise. */
int auth_check(const char *service, const char *user, const char *password);
#endif
