#ifndef OS_STDBOOL_H
#define OS_STDBOOL_H

// C23 made bool, true and false keywords
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
typedef _Bool bool;
#define true  1
#define false 0
#endif

#endif
