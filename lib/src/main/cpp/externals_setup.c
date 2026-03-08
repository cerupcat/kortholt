#include "m_pd.h"

// External setup function declarations
extern void bandlimited_tilde_setup(void);
extern void ntof_setup(void);
extern void fton_setup(void);
extern void history_setup(void);
extern void expr_setup(void);
extern void helmholtz_tilde_setup(void);
extern void abl_link_tilde_setup(void);

// Function to initialize all externals.
// Guarded to run only once — libpd's global symbol table does not tolerate
// duplicate class registrations from repeated class_new() calls, which can
// corrupt internal hash chains and trigger infinite recursion in message
// dispatch (SIGSEGV in libpd.so).
void externals_setup(void)
{
    static int initialized = 0;
    if (initialized) return;
    initialized = 1;

    bandlimited_tilde_setup();
    ntof_setup();
    fton_setup();
    history_setup();
    expr_setup();
    helmholtz_tilde_setup();
    abl_link_tilde_setup();
}