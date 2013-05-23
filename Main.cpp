#include "GitMonitor.h"

#ifdef WIN32
#pragma comment(lib, "lua-5.2.1.lib")
#endif

int main(int argc, char** argv)
{
	char* config = argc > 1 ? argv[1] : NULL;
	GitMonitor::InitWindowHandle();
	GitMonitor monitor;
	monitor.Start(config);
	char c = getchar();
	return 0;
}
