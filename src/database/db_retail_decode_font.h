#pragma once

#include "db_retail_zone.h"

enum RetailFontDecodeResult
{
    RETAIL_FONT_DECODE_OK,
    RETAIL_FONT_DECODE_BAD_ARGUMENT,
    RETAIL_FONT_DECODE_BAD_ROOT,
    RETAIL_FONT_DECODE_BAD_NAME,
    RETAIL_FONT_DECODE_BAD_MATERIAL,
    RETAIL_FONT_DECODE_BAD_GLYPHS,
    RETAIL_FONT_DECODE_OUT_OF_ARENA,
};

RetailFontDecodeResult RetailDecodeFont(RetailZoneLoadSession *session, Font_s **font);
bool RetailZoneInstallFontDecoder(RetailZoneLoadSession *session);
const char *RetailFontDecodeResultName(RetailFontDecodeResult result);
