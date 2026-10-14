/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    NewDelete.h

Abstract:

    Declaration of placement new and delete operators.

--*/
/*
 * Derived from the SYSVAD sample (Windows-driver-samples/audio/sysvad,
 * commit 2dc3fd3a, MS-PL — see LICENSE.MS-PL in this folder). Modified for
 * OpenA2DP: plain ExFreePool in delete, a matching placement delete; no
 * (size, flags) overload, which would collide with sized delete on 64-bit.
 */
#pragma once

PVOID operator new(size_t iSize, POOL_FLAGS poolFlags, ULONG tag);
void __cdecl operator delete(PVOID pVoid, POOL_FLAGS poolFlags, ULONG tag);
void __cdecl operator delete(_Pre_maybenull_ __drv_freesMem(Mem) PVOID pVoid);
void __cdecl operator delete(_Pre_maybenull_ __drv_freesMem(Mem) PVOID pVoid, _In_ size_t cbSize);
void __cdecl operator delete[](_Pre_maybenull_ __drv_freesMem(Mem) PVOID pVoid);
void __cdecl operator delete[](_Pre_maybenull_ __drv_freesMem(Mem) PVOID pVoid, _In_ size_t cbSize);
