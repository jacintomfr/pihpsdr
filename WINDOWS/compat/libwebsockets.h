/* Windows (MinGW64) shim for <libwebsockets.h>.
 * WHY: the compat layer redirects read()/write()/close()/open() with macros
 * (see win_compat_fd.h). libwebsockets' own inline VFS helpers call struct
 * members named read/write/close/open, which those macros would mangle.
 * Hide the macros while the real header is parsed. */
#pragma once
#pragma push_macro("open")
#pragma push_macro("close")
#pragma push_macro("read")
#pragma push_macro("write")
#undef open
#undef close
#undef read
#undef write
#include_next <libwebsockets.h>
#pragma pop_macro("open")
#pragma pop_macro("close")
#pragma pop_macro("read")
#pragma pop_macro("write")
