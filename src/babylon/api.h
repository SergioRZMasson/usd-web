#pragma once

#include <pxr/base/arch/export.h>

#if defined(PXR_STATIC)
#define USDBABYLON_API
#elif defined(USDBABYLON_EXPORTS)
#define USDBABYLON_API ARCH_EXPORT
#else
#define USDBABYLON_API ARCH_IMPORT
#endif
