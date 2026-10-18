// port_symname.cpp (w5, debug): PDB symbol name for an address (dbghelp), for
// port_bosstrace.cpp, kept apart so that file can include game headers.
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#pragma comment(lib, "dbghelp.lib")

extern "C" int port_sym_name(const void* addr, char* out, size_t n)
{
	static bool sInit;
	HANDLE proc = GetCurrentProcess();
	if (!sInit) {
		SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
		SymInitialize(proc, nullptr, TRUE); // harmless if already initialised
		sInit = true;
	}
	alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
	SYMBOL_INFO* sym  = (SYMBOL_INFO*)buf;
	sym->SizeOfStruct = sizeof(SYMBOL_INFO);
	sym->MaxNameLen   = 255;
	DWORD64 disp      = 0;
	if (!SymFromAddr(proc, (DWORD64)addr, &disp, sym)) {
		snprintf(out, n, "%p", addr);
		return 0;
	}
	snprintf(out, n, "%s", sym->Name);
	return 1;
}