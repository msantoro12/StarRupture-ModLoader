// Stands in for the loader's pch.h for the loader sources this test compiles.
//
// $(ProjectDir) comes first on the include path, so their #include "pch.h"
// lands here. It includes the loader's own framework.h, then points the two
// Win32 reads IsKeyHeld makes at test doubles (test_doubles.cpp): the real
// ones read the desktop's keyboard and foreground window, which a test can
// neither set nor share with whoever is at the machine. windows.h is already
// in by then, so its declarations keep their real names.
#pragma once

#include "../../StarRupture-ModLoader-Core/framework.h"

namespace TestDoubles
{
    SHORT AsyncKeyState(int vk);
    HWND  ForegroundWindow();
}

#define GetAsyncKeyState    TestDoubles::AsyncKeyState
#define GetForegroundWindow TestDoubles::ForegroundWindow
