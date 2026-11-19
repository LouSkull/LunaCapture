#pragma once

#include "Types.hpp"

#include <string>

bool EncodeScreenshotWic(const SaveJob& job, std::wstring& error);
ClipboardMemory EncodeClipboardPng(const SaveJob& job);
