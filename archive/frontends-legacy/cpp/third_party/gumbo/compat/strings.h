/*
 * MSVC compatibility shim for Gumbo 0.10.1.
 *
 * Gumbo includes <strings.h> and calls strcasecmp()/strncasecmp(), which are
 * POSIX APIs.  MSVC exposes the equivalent _stricmp()/_strnicmp() in
 * <string.h>.  This header is only found because its directory is passed with
 * /I before the Gumbo sources (see build.ps1 and CMakeLists.txt).
 */
#ifndef GUMBO_COMPAT_STRINGS_H_
#define GUMBO_COMPAT_STRINGS_H_

#include <string.h>

#ifndef strcasecmp
#define strcasecmp _stricmp
#endif

#ifndef strncasecmp
#define strncasecmp _strnicmp
#endif

#endif /* GUMBO_COMPAT_STRINGS_H_ */
