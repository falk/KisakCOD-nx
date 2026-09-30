// Case shim: the engine spells the Win32 header <Windows.h> (threads.cpp);
// the vendored d3d9-headers ship <windows.h>. This shim keeps that include
// resolving on a case-sensitive Horizon filesystem without touching engine
// sources.
#pragma once
#include <windows.h>
