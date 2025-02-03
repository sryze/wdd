/*
 * Copyright 2018-2020 Sergey Zolotarev
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

#define KB (1 << 10)
#define MB (1 << 20)
#define GB (1 << 30)
#define BUFFER_SIZE 4096
#define UPDATE_INTERVAL 1000000

#ifdef _MSC_VER
    #define strdup _strdup
    #define strtoll _strtoi64
    #define strtok_r strtok_s
#endif

struct program_options {
    BOOL print_drive_list;
    const char *filename_in;
    const char *filename_out;
    size_t block_size;
    size_t count;
    const char *status;
};

struct program_state {
    HANDLE in_file;
    HANDLE out_file;
    DWORD buffer_size;
    char *buffer;
    BOOL out_file_is_device;
    BOOL started_copying;
    ULONGLONG start_time;
    size_t num_bytes_in;
    size_t num_bytes_out;
    size_t num_blocks_copied;
};

static void print_usage(void) {
    fprintf(stderr, "Usage: wdd if=<in_file> of=<out_file> [bs=N] [count=N] "
                               "[status=progress]\n");
}

static ULONGLONG get_time_usec(void) {
    FILETIME filetime;
    ULARGE_INTEGER time;

    GetSystemTimeAsFileTime(&filetime);
    time.LowPart = filetime.dwLowDateTime;
    time.HighPart = filetime.dwHighDateTime;
    return time.QuadPart / 10;
}

static void format_size(char *buffer, size_t buffer_size, size_t size) {
    if (size >= GB) {
        snprintf(buffer, buffer_size, "%0.1f GB", (double)size / (double)GB);
    } else if (size >= MB) {
        snprintf(buffer, buffer_size, "%0.1f MB", (double)size / (double)MB);
    } else if (size >= KB) {
        snprintf(buffer, buffer_size, "%0.1f KB", (double)size / (double)KB);
    } else {
        snprintf(buffer, buffer_size, "%zu bytes", size);
    }
}

static void format_speed(char *buffer, size_t buffer_size, double speed) {
    if (speed >= (double)GB) {
        snprintf(buffer, buffer_size, "%0.1f GB/s", speed / (double)GB);
    } else if (speed >= (double)MB) {
        snprintf(buffer, buffer_size, "%0.1f MB/s", speed /(double) MB);
    } else if (speed >= (double)KB) {
        snprintf(buffer, buffer_size, "%0.1f KB/s", speed / (double)KB);
    } else {
        snprintf(buffer, buffer_size, "%0.1f bytes/s", speed);
    }
}

static void print_progress(size_t num_bytes_copied,
                           size_t last_bytes_copied,
                           ULONGLONG start_time,
                           ULONGLONG last_time) {
    ULONGLONG current_time;
    ULONGLONG elapsed_time;
    double speed;
    char bytes_str[16];
    char speed_str[16];

    current_time = get_time_usec();
    elapsed_time = current_time - start_time;
    if (elapsed_time >= 1000000) {
        speed = last_bytes_copied
            / ((double)(current_time - last_time) / 1000000);
    } else {
        speed = (double)last_bytes_copied;
    }

    format_size(bytes_str, sizeof(bytes_str), num_bytes_copied);
    format_speed(speed_str, sizeof(speed_str), speed);

    printf("%zu bytes (%s) copied, %0.1f s, %s\n",
        num_bytes_copied,
        bytes_str,
        (double)elapsed_time / 1000000.0,
        speed_str);
}

static void print_status(size_t num_bytes_copied, ULONGLONG start_time) {
    print_progress(
        num_bytes_copied,
        num_bytes_copied,
        start_time,
        start_time);
}

static void clear_output(void) {
    HANDLE console;
    COORD start_coord = {0, 0};
    DWORD num_chars_written;
    CONSOLE_SCREEN_BUFFER_INFO buffer_info;

    console = GetStdHandle(STD_OUTPUT_HANDLE);
    GetConsoleScreenBufferInfo(console, &buffer_info);
    start_coord.Y = buffer_info.dwCursorPosition.Y - 1;
    FillConsoleOutputCharacterA(
        console,
        ' ',
        buffer_info.dwSize.X,
        start_coord,
        &num_chars_written);
    SetConsoleCursorPosition(console, start_coord);
}

static char *get_error_message(DWORD error) {
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

static void cleanup(const struct program_state *s) {
    VirtualFree(s->buffer, 0, MEM_RELEASE);

    if (s->out_file_is_device) {
        DeviceIoControl(s->out_file, FSCTL_UNLOCK_VOLUME,
            NULL, 0, NULL, 0, NULL, NULL);
    }

    if (s->in_file != INVALID_HANDLE_VALUE) {
        CloseHandle(s->in_file);
    }
    if (s->out_file != INVALID_HANDLE_VALUE) {
        CloseHandle(s->out_file);
    }
}

static void exit_on_error(const struct program_state *s,
                          int error_code,
                          char *format,
                          ...) {
    va_list arg_list;
    char *reason;

    va_start(arg_list, format);
    vfprintf(stderr, format, arg_list);
    va_end(arg_list);
    fprintf(stderr, ": ");

    reason = get_error_message(error_code);
    reason[strlen(reason) - 2] = '\0';
    fprintf(stderr, "%s\n", reason);
    LocalFree(reason);

    if (s->started_copying) {
        print_status(s->num_bytes_out, s->start_time);
    }

    cleanup(s);
    exit(EXIT_FAILURE);
}

static size_t parse_size(const char *str) {
    char *end = NULL;
    size_t size = (size_t)strtoll(str, &end, 10);

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

static BOOL is_empty_string(const char *s) {
    return s == NULL || *s == '\0';
}

static BOOL parse_options(int argc,
                          char **argv,
                          struct program_options *options) {
    int i;

    options->filename_in = NULL;
    options->filename_out = NULL;
    options->block_size = 0;
    options->count = -1;
    options->status = NULL;

    for (i = 1; i < argc; i++) {
        char *value = NULL;
        char *name = strtok_r(argv[i], "=", &value);

        if (strcmp(name, "list") == 0) {
            options->print_drive_list = TRUE;
            return TRUE;
        } else if (strcmp(name, "if") == 0) {
            options->filename_in = strdup(value);
        } else if (strcmp(name, "of") == 0) {
            options->filename_out = strdup(value);
        } else if (strcmp(name, "bs") == 0) {
            options->block_size = parse_size(value);
        } else if (strcmp(name, "count") == 0) {
            options->count = (size_t)strtoll(value, NULL, 10);
        } else if (strcmp(name, "status") == 0) {
            options->status = strdup(value);
        } else {
            return FALSE;
        }
    }

    return !is_empty_string(options->filename_in)
        && !is_empty_string(options->filename_out);
}

int main(int argc, char **argv) {
    struct program_options options;
    struct program_state s;
    size_t num_blocks_copied = 0;
    BOOL show_progress = FALSE;
    size_t last_bytes_copied = 0;
    ULONGLONG last_time = 0;
    DISK_GEOMETRY_EX disk_geometry;

    ZeroMemory(&options, sizeof(options));

    if (!parse_options(argc, argv, &options)) {
        print_usage();
        return EXIT_FAILURE;
    }

    if (options.print_drive_list) {
        return system("powershell -Command \"Get-PhysicalDisk | Format-Table -Property DeviceID, MediaType, OperationalStatus, Size\"");
    }

    ZeroMemory(&s, sizeof(s));
    s.in_file = INVALID_HANDLE_VALUE;
    s.out_file = INVALID_HANDLE_VALUE;
    s.start_time = get_time_usec();
    s.out_file_is_device = FALSE;
    s.started_copying = FALSE;
    s.num_bytes_in = 0;
    s.num_bytes_out = 0;
    s.num_blocks_copied = 0;

    s.in_file = CreateFileA(
        options.filename_in,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        NULL);
    if (s.in_file == INVALID_HANDLE_VALUE) {
        exit_on_error(
            &s,
            GetLastError(),
            "Could not open input file or device %s for reading",
            options.filename_in);
    }

    /* First try to open as an existing file, thne as a new file. We can't
     * use OPEN_ALWAYS because it fails when out_file is a physical drive
     * (no idea why).
     */
    s.out_file = CreateFileA(
        options.filename_out,
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    if (s.out_file == INVALID_HANDLE_VALUE) {
        s.out_file = CreateFileA(
            options.filename_out,
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            NULL);
    }
    if (s.out_file == INVALID_HANDLE_VALUE) {
        exit_on_error(
            &s,
            GetLastError(),
            "Could not open output file or device %s for writing",
            options.filename_out);
    }

    s.buffer_size = BUFFER_SIZE;
    s.out_file_is_device = DeviceIoControl(
        s.out_file,
        IOCTL_DISK_GET_DRIVE_GEOMETRY,
        NULL,
        0,
        &disk_geometry,
        sizeof(disk_geometry),
        NULL,
        NULL);

    if (s.out_file_is_device) {
        DWORD sector_size;

        if (!DeviceIoControl(s.out_file, FSCTL_DISMOUNT_VOLUME,
                NULL, 0, NULL, 0, NULL, NULL)) {
            exit_on_error(
                &s,
                GetLastError(),
                "Failed to dismount output volume");
        }
        if (!DeviceIoControl(s.out_file, FSCTL_LOCK_VOLUME,
                NULL, 0, NULL, 0, NULL, NULL)) {
            exit_on_error(
                &s,
                GetLastError(),
                "Failed to lock output volume");
        }

        sector_size = disk_geometry.Geometry.BytesPerSector;
        if (options.block_size < sector_size) {
            s.buffer_size = sector_size;
        } else {
            s.buffer_size = (s.buffer_size / sector_size) * sector_size;
        }
    } else {
        if (options.block_size > 0) {
            s.buffer_size = (DWORD)options.block_size; // TODO: Possible bug with bs > 4GB
        }
    }

    s.buffer = VirtualAlloc(
        NULL,
        s.buffer_size,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE);
    if (s.buffer == NULL) {
        exit_on_error(&s, GetLastError(), "Failed to allocate buffer");
    }

    show_progress =
        (options.status != NULL && strcmp(options.status, "progress") == 0);
    s.started_copying = TRUE;

    for (;;) {
        DWORD num_block_bytes_in;
        DWORD num_block_bytes_out;
        BOOL result;
        ULONGLONG current_time;

        if (options.count >= 0 && s.num_blocks_copied >= options.count) {
            break;
        }

        if (show_progress) {
            current_time = get_time_usec();
            if (last_time == 0) {
                last_time = current_time;
            } else {
                if (current_time - last_time >= UPDATE_INTERVAL) {
                    clear_output();
                    print_progress(
                        s.num_bytes_out,
                        s.num_bytes_out - last_bytes_copied,
                        s.start_time,
                        last_time);
                    last_time = current_time;
                    last_bytes_copied = s.num_bytes_out;
                }
            }
        }

        result = ReadFile(
            s.in_file,
            s.buffer,
            s.buffer_size,
            &num_block_bytes_in,
            NULL);
        if (num_block_bytes_in == 0
            || (!result && GetLastError() == ERROR_SECTOR_NOT_FOUND)) {
            break;
        }
        if (!result) {
            exit_on_error(&s, GetLastError(), "Error reading from file");
        }

        s.num_bytes_in += num_block_bytes_in;

        result = WriteFile(
            s.out_file,
            s.buffer,
            num_block_bytes_in,
            &num_block_bytes_out,
            NULL);
        if (!result) {
            exit_on_error(&s, GetLastError(), "Error writing to file");
        }

        s.num_bytes_out += num_block_bytes_out;
        s.num_blocks_copied++;
    }

    cleanup(&s);
    clear_output();
    print_status(s.num_bytes_out, s.start_time);

    return EXIT_SUCCESS;
}

