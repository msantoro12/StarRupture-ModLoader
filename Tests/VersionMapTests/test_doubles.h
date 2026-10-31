#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace TestDoubles
{
	// pattern -> RVA in the test exe, for ScanValidation::Resolve.
	extern std::map<std::string, uintptr_t> g_rvas;
	// Every LogInfo and LogWarn line, formatted.
	extern std::vector<std::wstring> g_log;
}
