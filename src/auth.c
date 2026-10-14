/* mel - PAM authentication. We never touch password hashes ourselves. */
#define _GNU_SOURCE
#include "auth.h"
#include <security/pam_appl.h>
#include <stdlib.h>
#include <string.h>

static int conv(int n, const struct pam_message **msg, struct pam_response **resp, void *data)
{
    const char *pw = data;
    struct pam_response *r = calloc(n, sizeof *r);
    if (!r) return PAM_BUF_ERR;
    for (int i = 0; i < n; i++) {
        switch (msg[i]->msg_style) {
        case PAM_PROMPT_ECHO_OFF:
        case PAM_PROMPT_ECHO_ON:
            r[i].resp = strdup(pw);
            if (!r[i].resp) goto fail;
            break;
        case PAM_ERROR_MSG: case PAM_TEXT_INFO: break;
        default: goto fail;
        }
    }
    *resp = r;
    return PAM_SUCCESS;
fail:
    for (int i = 0; i < n; i++)
        if (r[i].resp) { explicit_bzero(r[i].resp, strlen(r[i].resp)); free(r[i].resp); }
    free(r);
    return PAM_CONV_ERR;
}

int auth_check(const char *service, const char *user, const char *password)
{
    pam_handle_t *h = NULL;
    struct pam_conv c = { conv, (void *)password };
    int ok = 0;
    if (pam_start(service, user, &c, &h) != PAM_SUCCESS) return 0;
    if (pam_authenticate(h, 0) == PAM_SUCCESS) {
        int rc = pam_acct_mgmt(h, 0);
        ok = (rc == PAM_SUCCESS || rc == PAM_NEW_AUTHTOK_REQD);
    }
    pam_end(h, PAM_SUCCESS);
    return ok;
}
