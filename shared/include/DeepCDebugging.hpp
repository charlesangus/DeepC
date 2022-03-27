#pragma once

//#define DEEPC_DEBUG
//#define DEEPC_DEBUG_PRINT

#ifdef DEEPC_DEBUG
#pragma optimize("", off)
#endif

#if defined(DEEPC_DEBUG) && defined(DEEPC_DEBUG_PRINT)
#define DEEPC_PRINT(...) printf(__VA_ARGS__);
#else
#define DEEPC_PRINT(...);
#endif