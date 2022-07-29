// This file is part of the FidelityFX SDK.
//
// Copyright (c) 2022 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "ffx_assert.h"
#include <stdlib.h>  // for malloc()

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>  // required for OutputDebugString()
#include <stdio.h>    // required for sprintf_s
#else
// https://github.com/mpaland/printf
int sprintf_s(char* buffer, size_t sizeOfBuffer, const char* format, ...)
{
	va_list va;
	va_start(va, format);
	const int ret = vsnprintf(buffer, sizeOfBuffer, format, va);
	va_end(va);
	return ret;
}
#endif                // #ifndef _WIN32

static FfxAssertCallback s_assertCallback;

// set the printing callback function
void ffxAssertSetPrintingCallback(FfxAssertCallback callback)
{
    s_assertCallback = callback;
    return;
}

// implementation of assert reporting
bool ffxAssertReport(const char* file, int32_t line, const char* condition, const char* message)
{
    if (!file) {

        return true;
    }

    // form the final assertion string and output to the TTY.
    const size_t bufferSize = snprintf(NULL, 0, "%s(%d): ASSERTION FAILED. %s\n", file, line, message ? message : condition) + 1;
    char*        tempBuf    = (char*)malloc(bufferSize);
    if (!tempBuf) {

        return true;
    }

    if (!message) {
        sprintf_s(tempBuf, bufferSize, "%s(%d): ASSERTION FAILED. %s\n", file, line, condition);
    } else {
        sprintf_s(tempBuf, bufferSize, "%s(%d): ASSERTION FAILED. %s\n", file, line, message);
    }

    if (!s_assertCallback) {
#ifdef _WIN32
        OutputDebugStringA(tempBuf);
#endif
    } else {
        s_assertCallback(tempBuf);
    }

    // free the buffer.
    free(tempBuf);

    return true;
}
