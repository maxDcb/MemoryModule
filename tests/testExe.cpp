#include <windows.h> 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


int APIENTRY WinMain( HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow )
{
    UNREFERENCED_PARAMETER(hInst);
    UNREFERENCED_PARAMETER(hPrev);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    SetEnvironmentVariableA("MEMORYMODULE_TEST_EXE_RAN", "true");
	return 0;
}
