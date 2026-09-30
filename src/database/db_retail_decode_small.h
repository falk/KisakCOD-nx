#pragma once

#ifdef KISAK_RETAIL_LOCALIZE_PROOF_HOST
// The production Switch build force-includes switch_compat.h.  Keep the host
// sanitizer proof self-contained without changing the production ABI.
// Include libc before renaming the engine's random() symbol, matching
// switch_compat.h's ordering.
#include <cstdlib>
#define random switch_random
#define crandom switch_crandom
#define __int8 char
#define __int16 short
#define __int32 int
#define __int64 long long
#define __forceinline inline
typedef unsigned char byte;
#endif

#include "db_retail_zone.h"

// Result values are deliberately specific so the zone boundary can emit a
// useful failure rather than silently substituting a localization default.
enum RetailLocalizeDecodeResult
{
    RETAIL_LOCALIZE_DECODE_OK,
    RETAIL_LOCALIZE_DECODE_BAD_ARGUMENT,
    RETAIL_LOCALIZE_DECODE_BAD_ROOT,
    RETAIL_LOCALIZE_DECODE_UNSUPPORTED_VALUE_REFERENCE,
    RETAIL_LOCALIZE_DECODE_UNSUPPORTED_NAME_REFERENCE,
    RETAIL_LOCALIZE_DECODE_UNTERMINATED_VALUE,
    RETAIL_LOCALIZE_DECODE_UNTERMINATED_NAME,
    RETAIL_LOCALIZE_DECODE_EMPTY_NAME,
    RETAIL_LOCALIZE_DECODE_OUT_OF_ARENA,
};

// Decodes the PC 32-bit LocalizeEntry wire root ({value, name}) and widens it
// directly into the existing runtime LocalizeEntry.  The two strings are
// copied to the zone native arena because the temporary wire block can be
// rewound by the next asset body.
RetailLocalizeDecodeResult RetailDecodeLocalizeEntry(RetailZoneLoadSession *session,
                                                     LocalizeEntry **entry);

// Installs the loader in the shared 33-entry dispatcher.  Registration is
// still performed by RetailZoneLoadSessionDispatchAsset, which delegates to
// the normal DB_AddXAsset localization owner for the active retail zone.
bool RetailZoneInstallLocalizeEntryDecoder(RetailZoneLoadSession *session);

const char *RetailLocalizeDecodeResultName(RetailLocalizeDecodeResult result);
