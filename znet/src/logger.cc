//
//    Copyright 2024 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

#include "logger.h"

bool LoggerInitializer::s_Initialized = false;

LoggerInitializer::LoggerInitializer() {
  if (s_Initialized) {
    return;
  }
  s_Initialized = true;
#ifdef TARGET_WIN
  HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
  if (hOut == INVALID_HANDLE_VALUE) return;

  DWORD dwMode = 0;
  if (!GetConsoleMode(hOut, &dwMode)) return;

  dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
  SetConsoleMode(hOut, dwMode);

  CONSOLE_SCREEN_BUFFER_INFO csbi;
  if (!GetConsoleScreenBufferInfo(hOut, &csbi)) {
    return;  // Handle error
  }
  COORD newSize;
  newSize.X = 1000;
  newSize.Y = csbi.dwSize.Y;
  SetConsoleScreenBufferSize(hOut, newSize);
#endif
}
