#pragma once

#include "db_retail_zone.h"

// Result values are deliberately specific so the zone boundary can emit a
// useful failure rather than silently substituting an empty file.
enum RetailRawFileDecodeResult
{
    RETAIL_RAWFILE_DECODE_OK,
    RETAIL_RAWFILE_DECODE_BAD_ARGUMENT,
    RETAIL_RAWFILE_DECODE_BAD_ROOT,
    RETAIL_RAWFILE_DECODE_BAD_NAME,
    RETAIL_RAWFILE_DECODE_BAD_BUFFER,
    RETAIL_RAWFILE_DECODE_OUT_OF_ARENA,
};

// Android Load_RawFile: 12-byte root {name, len, buffer} in block 0, the
// name XString in block 4, and (for any nonzero buffer slot -- a presence
// marker, not an encoded alias) exactly len + 1 raw bytes following it.
RetailRawFileDecodeResult RetailDecodeRawFile(RetailZoneLoadSession *session, RawFile **out);

// Installs the  loader in the shared dispatcher.
bool RetailZoneInstallRawFileDecoder(RetailZoneLoadSession *session);

const char *RetailRawFileDecodeResultName(RetailRawFileDecodeResult result);

// Shared XString slot resolution for small live roots (RawFile, MapEnts):
// the -1 inline form copies the NUL-terminated string at the wire block-4
// cursor; any other nonzero ref decodes as a block-4 alias and copies the
// string already sitting at that offset in the session's persistent zone
// blocks; zero binds null.
bool RetailDecodeInlineOrNullString(RetailZoneLoadSession *session, uint32_t ref,
                                    const char **out);
