#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>

// 简易中文输入对话框（模态）：返回 true 表示确定（result 为输入文本）。
bool ShowInputBox(HWND owner, const std::wstring& title,
                  const std::wstring& prompt, const std::wstring& initial,
                  std::wstring& result);
