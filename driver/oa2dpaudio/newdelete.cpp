/*****************************************************************************
* NewDelete.cpp -  CPP placement new and delete operators implementation
*****************************************************************************
* Copyright (c) Microsoft Corporation All Rights Reserved
*
* Module Name:
*
* NewDelete.cpp
*
* Abstract:
*
*   Definition of placement new and delete operators.
*
*/
/*
 * Derived from the SYSVAD sample (Windows-driver-samples/audio/sysvad,
 * commit 2dc3fd3a, MS-PL — see LICENSE.MS-PL in this folder). Modified for
 * OpenA2DP: plain ExFreePool in delete, a matching placement delete; no
 * (size, flags) overload, which would collide with sized delete on 64-bit.
 */
extern "C" {
#include <wdm.h>
}

#include "newdelete.h"
#include "oa2dpaudio.h"

#pragma code_seg()

PVOID operator new(size_t iSize, POOL_FLAGS poolFlags, ULONG tag)
{
    return ExAllocatePool2(poolFlags, iSize, tag);
}

/* Only used if a constructor fails during placement new (never: no
 * exceptions in kernel mode). */
void __cdecl operator delete(PVOID pVoid, POOL_FLAGS poolFlags, ULONG tag)
{
    UNREFERENCED_PARAMETER(poolFlags);
    UNREFERENCED_PARAMETER(tag);
    if (pVoid)
        ExFreePool(pVoid);
}

void __cdecl operator delete(_Pre_maybenull_ __drv_freesMem(Mem) PVOID pVoid)
{
    if (pVoid)
        ExFreePool(pVoid);
}

void __cdecl operator delete(_Pre_maybenull_ __drv_freesMem(Mem) PVOID pVoid, _In_ size_t cbSize)
{
    UNREFERENCED_PARAMETER(cbSize);
    if (pVoid)
        ExFreePool(pVoid);
}

void __cdecl operator delete[](_Pre_maybenull_ __drv_freesMem(Mem) PVOID pVoid)
{
    if (pVoid)
        ExFreePool(pVoid);
}

void __cdecl operator delete[](_Pre_maybenull_ __drv_freesMem(Mem) PVOID pVoid, _In_ size_t cbSize)
{
    UNREFERENCED_PARAMETER(cbSize);
    if (pVoid)
        ExFreePool(pVoid);
}
