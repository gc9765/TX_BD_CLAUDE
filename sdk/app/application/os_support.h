#ifndef _OS_SUPPORT_H_
#define _OS_SUPPORT_H_

#include "basic_include.h"
#include "custom_mem.h"

#define aup_printf os_printf

#define aup_memcpy 	os_memcpy
#define aup_memset 	os_memset
#define aup_abs 	os_abs

#ifdef PSRAM_HEAP
#define aup_malloc  custom_malloc_psram
#define aup_calloc  custom_calloc_psram
#define aup_realloc custom_realloc_psram
#define aup_free    custom_free_psram
#else
#define aup_malloc  custom_malloc
#define aup_calloc  custom_calloc
#define aup_realloc custom_realloc
#define aup_free    custom_free
#endif

#endif
