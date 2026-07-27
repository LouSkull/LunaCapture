#pragma once

#include "Types.hpp"

#include <string>

void PrewarmWic();

bool EncodeScreenshotWic(const SaveJob& job, std::wstring& error);
