#ifndef NLS_H
#define NLS_H

/*
 * Rufus does not translate the messages of ms-sys. Its libintl.h stub is gone, as
 * it hid the one of glibc from the C++ standard library of the Qt front end.
 */
#define _(String) (String)
#define gettext_noop(String) (String)
#define N_(String) gettext_noop(String)

/* Init Native language support */
void nls_init(void);

#endif
