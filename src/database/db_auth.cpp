#include <universal/q_shared.h>
#include "database.h"

#include <zlib/zlib.h>

// Note: On XBox it seems to use tomcrypt, and is more involved. 

int32_t __cdecl DB_AuthLoad_InflateInit(z_stream_s *stream, bool isSecure)
{
    iassert(!isSecure);
    // ZLIB_VERSION, not a hardcoded "1.1.4": the vendored zlib is 1.3.1 now
    // and the init macro rejects a mismatched version/size pair.
    return inflateInit_(stream, ZLIB_VERSION, sizeof(z_stream));
}

void __cdecl DB_AuthLoad_InflateEnd(z_stream_s *stream)
{
    inflateEnd(stream);
}

uint32_t __cdecl DB_AuthLoad_Inflate(z_stream_s *stream, int32_t flush)
{
    return inflate(stream, flush);
}

