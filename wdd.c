/* Copyright 2018 Sergey Zolotarev
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <stdio.h>
#include <windows.h>

typedef struct {
    const char *filename_in;
    const char *filename_out;
    size_t block_size;
    size_t count;
    const char *status;
} ProgramOptions;

static HANDLE file_in;
static HANDLE file_out;
static size_t buffer_size;
static char *buffer;
static BOOL started_copying = FALSE;
static ULONGLONG start_time;
static size_t num_bytes_in = 0;
static size_t num_bytes_out = 0;
static size_t num_blocks_copied = 0;

static void PrintUsageAndExit() {
    fprintf(stderr, "Usage: wdd if=<in_file> of=<out_file> "
                              "[bs=N] [count=N] [status=progress]\n");
    exit(EXIT_FAILURE);
}

static ULONGLONG GetSystemTimeMicroseconds() {
    FILETIME filetime;
    ULARGE_INTEGER time;

    GetSystemTimeAsFileTime(&filetime);
    time.LowPart = filetime.dwLowDateTime;
    time.HighPart = filetime.dwHighDateTime;

    return time.QuadPart / 10;
}

static void FormatByteCount(char *buffer, size_t buffer_size, double size) {
    if (size >= (1 << 30)) {
        snprintf(buffer, buffer_size, "%0.1f GB", size / (1 << 30));
    } else if (size >= (1 << 20)) {
        snprintf(buffer, buffer_size, "%0.1f MB", size / (1 << 20));
    } else if (size >= (1 << 10)) {
        snprintf(buffer, buffer_size, "%0.1f KB", size / (1 << 10));
    } else {
        snprintf(buffer, buffer_size, "%0.1f bytes", size);
    }
}

static void PrintStatus() {
    ULONGLONG elapsed_time;
    double speed;
    char bytes_str[10];
    char speed_str[10];

    elapsed_time = GetSystemTimeMicroseconds() - start_time;
    if (elapsed_time >= 1000000) {
        speed = (double)num_bytes_out / ((double)elapsed_time / 1000000);
    } else {
        speed = (double)num_bytes_out;
    }

    FormatByteCount(bytes_str, sizeof(bytes_str), num_bytes_in);
    FormatByteCount(speed_str, sizeof(speed_str), speed);

    printf("%zu bytes (%s) copied, %0.1f s, %s/s\n",
        num_bytes_out,
        bytes_str,
        (double)elapsed_time / 1000000.0,
        speed_str);
}

static char *GetErrorMessage(DWORD error) {
    char *buffer = NULL;

    FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
        NULL,
        error,
        0,
        (char *)&buffer,
        0,
        NULL);
    return buffer;
}

static void CleanUpOnExit() {
    LocalFree(buffer);
    CloseHandle(file_in);
    CloseHandle(file_out);
}

static void ExitWithError(int error_code, char *format, ...) {
    va_list arg_list;
    char *reason;

    va_start(arg_list, format);
    vfprintf(stderr, format, arg_list);
    va_end(arg_list);
    fprintf(stderr, ": ");

    reason = GetErrorMessage(error_code);
    fprintf(stderr, reason);
    LocalFree(reason);

    if (started_copying) {
        PrintStatus();
    }

    CleanUpOnExit();
    exit(EXIT_FAILURE);
}

static size_t ParseSize(const char *str) {
    char *end = NULL;
    size_t size = (size_t)_strtoi64(str, &end, 10);

    if (end != NULL && *end != '\0') {
        switch (*end) {
            case 'k':
            case 'K':
                size *= 1 << 10;
                break;
            case 'm':
            case 'M':
                size *= 1 << 20;
                break;
            case 'g':
            case 'G':
                size *= 1 << 30;
                break;
        }
    }
    return size;
}

static BOOL IsEmptyStringOrNull(const char *s) {
    return s == NULL || *s == '\0';
}

static BOOL CheckOptions(ProgramOptions *options) {
    return !IsEmptyStringOrNull(options->filename_in)
        && !IsEmptyStringOrNull(options->filename_out);
}

static void ParseOptions(int argc, char **argv, ProgramOptions *options) {
    int i;

    for (i = 1; i < argc; i++) {
        char *value = NULL;
        char *name = strtok_s(argv[i], "=", &value);

        if (strcmp(name, "if") == 0) {
            options->filename_in = _strdup(value);
        } else if (strcmp(name, "of") == 0) {
            options->filename_out = _strdup(value);
        } else if (strcmp(name, "bs") == 0) {
            options->block_size = ParseSize(value);
        } else if (strcmp(name, "count") == 0) {
            options->count = (size_t)_strtoi64(value, NULL, 10);
        } else if (strcmp(name, "status") == 0) {
            options->status = _strdup(value);
        }
    }
}

int main(int argc, char **argv) {
    ProgramOptions options = {NULL, NULL, 0, -1};
    BOOL show_progress = FALSE;
    DISK_GEOMETRY_EX disk_geometry;

    ParseOptions(argc, argv, &options);
    if (!CheckOptions(&options)) {
        PrintUsageAndExit();
    }

    start_time = GetSystemTimeMicroseconds();

    file_in = CreateFileA(
        options.filename_in,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    if (file_in == INVALID_HANDLE_VALUE) {
        ExitWithError(
            GetLastError(),
            "Could not open input file or device %s for reading",
            options.filename_in);
    }

    /* First try to open as an existing file, thne as a new file. We can't
     * use OPEN_ALWAYS because it fails when out_file is a physical drive
     * (no idea why).
     */
    file_out = CreateFileA(
        options.filename_out,
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    if (file_out == INVALID_HANDLE_VALUE) {
        file_out = CreateFileA(
            options.filename_out,
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            NULL);
    }
    if (file_out == INVALID_HANDLE_VALUE) {
        ExitWithError(
            GetLastError(),
            "Could not open output file or device %s for writing",
            options.filename_out);
    }

    if (strstr(options.filename_out, "\\\\") == options.filename_out) {
        if (!DeviceIoControl(
                file_out,
                IOCTL_DISK_GET_DRIVE_GEOMETRY,
                NULL,
                0,
                &disk_geometry,
                sizeof(disk_geometry),
                NULL,
                NULL)) {
            ExitWithError(GetLastError(), "Failed to get disk information");
        }
        buffer_size = disk_geometry.Geometry.BytesPerSector;
    } else {
        buffer_size = 4096;
    }

    if (options.block_size > 0) {
        buffer_size = options.block_size;
    }

    buffer = LocalAlloc(LPTR, buffer_size);
    if (buffer == NULL) {
        ExitWithError(GetLastError(), "Failed to allocate buffer");
    }

    if (options.status != NULL && strcmp(options.status, "progress") == 0) {
        show_progress = TRUE;
    }

    started_copying = TRUE;

    for (;;) {
        size_t num_block_bytes_in;
        size_t num_block_bytes_out;
        BOOL result;

        if (options.count >= 0 && num_blocks_copied >= options.count) {
            break;
        }

        result = ReadFile(
            file_in,
            buffer,
            buffer_size,
            &num_block_bytes_in,
            NULL);
        if (!result) {
            ExitWithError(GetLastError(), "Error reading from file");
        }
        num_bytes_in += num_block_bytes_in;

        if (num_block_bytes_in == 0) {
            break;
        }

        result = WriteFile(
            file_out,
            buffer,
            num_block_bytes_in,
            &num_block_bytes_out,
            NULL);
        if (!result) {
            ExitWithError(GetLastError(), "Error writing to file");
        }
        num_bytes_out += num_block_bytes_out;

        num_blocks_copied++;
    }

    CleanUpOnExit();
    PrintStatus();

    return 0;
}

