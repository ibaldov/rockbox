#ifndef _CTYPE_H_
#ifdef __cplusplus
extern "C" {
#endif
#define _CTYPE_H_

#include "_ansi.h"

int _EXFUN(isalnum, (int __c));
int _EXFUN(isalpha, (int __c));
int _EXFUN(iscntrl, (int __c));
int _EXFUN(isdigit, (int __c));
int _EXFUN(isgraph, (int __c));
int _EXFUN(islower, (int __c));
int _EXFUN(isprint, (int __c));
int _EXFUN(ispunct, (int __c));
int _EXFUN(isspace, (int __c));
int _EXFUN(isupper, (int __c));
int _EXFUN(isxdigit,(int __c));
int _EXFUN(tolower, (int __c));
int _EXFUN(toupper, (int __c));

#ifndef __STRICT_ANSI__
int _EXFUN(isascii, (int __c));
int _EXFUN(toascii, (int __c));
int _EXFUN(_tolower, (int __c));
int _EXFUN(_toupper, (int __c));
#endif

#define _U      01
#define _L      02
#define _N      04
#define _S      010
#define _P      020
#define _C      040
#define _X      0100
#define _B      0200

#ifdef PLUGIN
#define _ctype_ (rb->_rbctype_)
#else
extern const unsigned char _ctype_[257];
#endif

#ifndef __cplusplus
#define isalpha(c)      ((_ctype_+1)[(unsigned char)(c)]&(_U|_L))
#define isupper(c)      ((_ctype_+1)[(unsigned char)(c)]&_U)
#define islower(c)      ((_ctype_+1)[(unsigned char)(c)]&_L)
/*#define isdigit(c)      ((_ctype_+1)[(unsigned char)(c)]&_N)*/
#define isxdigit(c)     ((_ctype_+1)[(unsigned char)(c)]&(_X|_N))
#define isspace(c)      ((_ctype_+1)[(unsigned char)(c)]&_S)
#define ispunct(c)      ((_ctype_+1)[(unsigned char)(c)]&_P)
#define isalnum(c)      ((_ctype_+1)[(unsigned char)(c)]&(_U|_L|_N))
#define isprint(c)      ((_ctype_+1)[(unsigned char)(c)]&(_P|_U|_L|_N|_B))
#define isgraph(c)      ((_ctype_+1)[(unsigned char)(c)]&(_P|_U|_L|_N))
#define iscntrl(c)      ((_ctype_+1)[(unsigned char)(c)]&_C)

/* should be slightly faster without array access */
#define isdigit(c)      (((unsigned int) (c) - '0') < 10)
/* Non-gcc versions will get the library versions, and will be
   slightly slower */
#ifdef __GNUC__
# define toupper(c) \
        __extension__ ({ int __x = (unsigned char) (c); islower(__x) ? (__x - 'a' + 'A') : __x;})
# define tolower(c) \
        __extension__ ({ int __x = (unsigned char) (c); isupper(__x) ? (__x - 'A' + 'a') : __x;})
#endif
#endif /* !__cplusplus */

#ifndef __STRICT_ANSI__
#define isascii(c)      ((unsigned char)(c)<=0177)
#define toascii(c)      ((c)&0177)
#endif

#ifdef __cplusplus
}
#endif
#endif /* _CTYPE_H_ */
