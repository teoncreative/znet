//
//    Copyright 2024 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

std::string GetLastErrorInfo() {
#ifdef TARGET_WIN
  char buf[256];
  buf[0] = '\0';
  int err = WSAGetLastError();
  FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                nullptr,
                err,
                MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                buf,
                sizeof(buf),
                nullptr);
  return {buf};
#else
  char buf[256];
  strerror_s(buf, 256, errno);
  return {buf};
#endif
}
