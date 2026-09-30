#pragma once
#include <cstdint>
#include "db_retail_zone.h"
enum RetailTechniqueSetDecodeResult { RETAIL_TECHNIQUE_SET_OK, RETAIL_TECHNIQUE_SET_BAD_ARGUMENT, RETAIL_TECHNIQUE_SET_TRUNCATED, RETAIL_TECHNIQUE_SET_UNSUPPORTED_REFERENCE, RETAIL_TECHNIQUE_SET_ALLOCATION_FAILED, RETAIL_TECHNIQUE_SET_REGISTRATION_FAILED };
RetailTechniqueSetDecodeResult DB_RetailDecodeTechniqueSet(RetailZoneLoadSession *, uint32_t, const char *, MaterialTechniqueSet **);
