    #include "D3DApp.h"
#include "SponzaApp.h"
#include <stdexcept>

#pragma comment(lib, "user32.lib")

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE prevInstance,
                   PSTR cmdLine, int showCmd)
{
    try
    {
        SponzaApp theApp(hInstance);

        if (!theApp.Initialize())
            return 0;

        return theApp.Run();
    }
    catch (const std::exception& e)
    {
        MessageBoxA(nullptr, e.what(), "Crash!", MB_OK | MB_ICONERROR);
        return 0;
    }
}
