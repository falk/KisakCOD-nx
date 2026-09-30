#pragma once

#include "db_retail_zone.h"

enum RetailStringTableDecodeResult
{
    RETAIL_STRINGTABLE_DECODE_OK,
    RETAIL_STRINGTABLE_DECODE_BAD_ARGUMENT,
    RETAIL_STRINGTABLE_DECODE_BAD_ROOT,
    RETAIL_STRINGTABLE_DECODE_BAD_NAME,
    RETAIL_STRINGTABLE_DECODE_BAD_VALUES,
    RETAIL_STRINGTABLE_DECODE_OUT_OF_ARENA,
};

// Android Load_StringTable: unlike every other retail asset here, the
// 16-byte root {name, columnCount, rowCount, values} lives in block 4, not
// block 0 -- there is no separate temp-block root for StringTable.  A
// nonzero values slot streams a contiguous rowCount * columnCount pointer
// table, and each nonzero, inline (-1) cell then owns one XString
// immediately following the table, in cell order; any other per-cell value
// would be an alias this decoder does not resolve yet.
RetailStringTableDecodeResult RetailDecodeStringTable(RetailZoneLoadSession *session,
                                                       StringTable **out);

// Installs the  loader in the shared dispatcher.
bool RetailZoneInstallStringTableDecoder(RetailZoneLoadSession *session);

const char *RetailStringTableDecodeResultName(RetailStringTableDecodeResult result);
