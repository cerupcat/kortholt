#include "m_pd.h"

// External setup function declarations
extern void bandlimited_tilde_setup(void);
extern void ntof_setup(void);
extern void fton_setup(void);
extern void history_setup(void);
extern void expr_setup(void);
extern void helmholtz_tilde_setup(void);
extern void abl_link_tilde_setup(void);

// Function to initialize all externals
void externals_setup(void)
{
    bandlimited_tilde_setup();
    ntof_setup();
    fton_setup();
    history_setup();
    expr_setup();
    helmholtz_tilde_setup();
    abl_link_tilde_setup();
}