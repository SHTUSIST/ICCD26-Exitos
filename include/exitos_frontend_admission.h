/* Process-lifetime admission gate shared by interception frontends. */
#ifndef EXITOS_FRONTEND_ADMISSION_H
#define EXITOS_FRONTEND_ADMISSION_H

/* A frontend publishes all borrowed configuration/context pointers before
 * enabling admission.  Every entry point that may dereference those pointers
 * must hold one admission until it has stopped using them. */
void exitos_frontend_admission_enable(void);
int  exitos_frontend_admission_enter(void);
void exitos_frontend_admission_leave(void);

/* Close admission, then wait for every caller admitted before the close to
 * leave.  The gate itself has static DSO lifetime, so a racing late entrant can
 * safely observe the close even after the owned context has been destroyed. */
void exitos_frontend_admission_quiesce(void);

#endif
