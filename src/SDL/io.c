#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <strings.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "revenge/platform_backend.h"
#include <SDL3/SDL.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

enum
{
    FileHandleCapacity = 0x40,
    FirstGameFileHandle = 0x05,
    DosAttributeReadOnly = 0x01,
    DosAccessReadOnly = 0x00,
    DosAccessWriteOnly = 0x01,
    DosAccessReadWrite = 0x02,
    NormalizedPathCapacity = 1024
};

static SDL_IOStream *files[FileHandleCapacity];
static byte fileAccess[FileHandleCapacity];
static byte defaultDrive;

static int16 HostFileError(void)
{
    int errorNumber;

    errorNumber = errno;
    if (errorNumber <= 0x00 || errorNumber > 0x23)
        errorNumber = EIO;
    return (int16)-errorNumber;
}

static SDL_IOStream *GetFile(uint16 fileHandle)
{
    if (fileHandle >= FileHandleCapacity)
        return 0;
    return files[fileHandle];
}

static int16 StoreFile(SDL_IOStream *file, byte access)
{
    uint16 fileHandle;

    if (file == 0)
        return HostFileError();

    for (fileHandle = FirstGameFileHandle;
         fileHandle < FileHandleCapacity;
    ++fileHandle)
         {
             if (files[fileHandle] == 0)
             {
                 files[fileHandle] = file;
                 fileAccess[fileHandle] = access;
                 return (int16)fileHandle;
             }
         }

         SDL_CloseIO(file);
         return -EMFILE;
}

static int TruncateAtCurrentPosition(SDL_IOStream *file)
{
    SDL_PropertiesID properties;

    if (!SDL_FlushIO(file))
        return 0;

    properties = SDL_GetIOProperties(file);
    #if defined(_WIN32)
    {
        HANDLE handle;

        handle = (HANDLE)SDL_GetPointerProperty(
            properties,
            SDL_PROP_IOSTREAM_WINDOWS_HANDLE_POINTER,
            0);
        return handle != 0 && SetEndOfFile(handle) != 0;
    }
    #else
    {
        Sint64 position;
        Sint64 descriptor;

        position = SDL_TellIO(file);
        descriptor = SDL_GetNumberProperty(
            properties,
            SDL_PROP_IOSTREAM_FILE_DESCRIPTOR_NUMBER,
            -1);
        return position >= 0 && descriptor >= 0
        && ftruncate((int)descriptor, (off_t)position) == 0;
    }
    #endif
}

static const char *ResolveHostPath(
    const char *filePath,
    char buffer[NormalizedPathCapacity])
{
    size_t i = 0;

    if (filePath == 0)
        return 0;

    if (((filePath[0] >= 'A' && filePath[0] <= 'Z') ||
        (filePath[0] >= 'a' && filePath[0] <= 'z')) &&
        filePath[1] == ':')
    {
        filePath += 2;
        if (filePath[0] == '\\' || filePath[0] == '/')
            filePath += 1;
    }

    while (filePath[i] != '\0' && i + 1 < NormalizedPathCapacity)
    {
        buffer[i] = (filePath[i] == '\\') ? '/' : filePath[i];
        ++i;
    }
    buffer[i] = '\0';

    #ifndef _WIN32
    if (access(buffer, F_OK) != 0)
    {
        char dirPath[NormalizedPathCapacity];
        const char *baseName;
        const char *lastSlash = strrchr(buffer, '/');
        DIR *dir;

        if (lastSlash != NULL)
        {
            size_t dirLen = (size_t)(lastSlash - buffer);
            if (dirLen == 0)
            {
                strcpy(dirPath, "/");
            }
            else
            {
                memcpy(dirPath, buffer, dirLen);
                dirPath[dirLen] = '\0';
            }
            baseName = lastSlash + 1;
        }
        else
        {
            strcpy(dirPath, ".");
            baseName = buffer;
        }

        dir = opendir(dirPath);
        if (dir != NULL)
        {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL)
            {
                if (strcasecmp(entry->d_name, baseName) == 0)
                {
                    if (lastSlash != NULL)
                        snprintf(buffer, NormalizedPathCapacity, "%s/%s", dirPath, entry->d_name);
                    else
                        snprintf(buffer, NormalizedPathCapacity, "%s", entry->d_name);
                    break;
                }
            }
            closedir(dir);
        }
    }
    #endif

    return buffer;
}

uint16 SDLBackend_DirectConsoleIO(byte character)
{
    if (character == 0xFF)
        return 0x0600;

    (void)fwrite(&character, 0x01, 0x01, stdout);
    (void)fflush(stdout);
    return (uint16)(0x0600 | character);
}

void SDLBackend_SelectDefaultDrive(byte zeroBasedDrive)
{
    defaultDrive = zeroBasedDrive;
    (void)defaultDrive;
}

int16 SDLBackend_CreateFile(uint16 attributes, const char *filePath)
{
    char resolvedPath[NormalizedPathCapacity];

    (void)attributes;
    if (filePath == 0)
        return -EINVAL;
    filePath = ResolveHostPath(filePath, resolvedPath);
    return StoreFile(SDL_IOFromFile(filePath, "w+b"), DosAccessReadWrite);
}

int16 SDLBackend_OpenFile(const char *filePath, uint16 openFlags)
{
    byte access;
    const char *mode;
    char resolvedPath[NormalizedPathCapacity];
    SDL_IOStream *stream;

    if (filePath == 0)
        return -EINVAL;
    filePath = ResolveHostPath(filePath, resolvedPath);

    if ((openFlags & 0x0002) != 0x0000)
        access = DosAccessWriteOnly;
    else if ((openFlags & 0x0004) != 0x0000)
        access = DosAccessReadWrite;
    else
        access = DosAccessReadOnly;
    mode = access == DosAccessReadOnly ? "rb" : "r+b";

    stream = SDL_IOFromFile(filePath, mode);
    if (stream == 0)
    {
        fprintf(stderr, "[Revenge IO] Failed to open '%s' (mode=%s)\n", filePath, mode);
    }

    return StoreFile(stream, access);
}

int16 SDLBackend_CloseFile(int16 fileHandle)
{
    SDL_IOStream *file;

    if (fileHandle < 0)
        return -EBADF;

    file = GetFile((uint16)fileHandle);
    if (file == 0)
        return -EBADF;

    files[(uint16)fileHandle] = 0;
    fileAccess[(uint16)fileHandle] = DosAccessReadOnly;
    return SDL_CloseIO(file) ? 0x0000 : HostFileError();
}

int16 SDLBackend_ReadFile(
    uint16 fileHandle,
    void *destinationBuffer,
    uint16 byteCount)
{
    SDL_IOStream *file;
    size_t bytesRead;

    file = GetFile(fileHandle);
    if (file == 0)
        return -EBADF;
    if (destinationBuffer == 0 && byteCount != 0x0000)
        return -EINVAL;
    if (fileAccess[fileHandle] == DosAccessWriteOnly)
        return -EACCES;

    bytesRead = SDL_ReadIO(file, destinationBuffer, byteCount);
    if (SDL_GetIOStatus(file) == SDL_IO_STATUS_ERROR)
        return HostFileError();
    return (int16)(uint16)bytesRead;
}

int16 SDLBackend_WriteFile(
    uint16 fileHandle,
    const void *sourceBuffer,
    uint16 byteCount)
{
    SDL_IOStream *file;
    size_t bytesWritten;

    file = GetFile(fileHandle);
    if (file == 0)
        return -EBADF;
    if (fileAccess[fileHandle] == DosAccessReadOnly)
        return -EACCES;
    if (sourceBuffer == 0 && byteCount != 0x0000)
        return -EINVAL;

    if (byteCount == 0x0000)
        return TruncateAtCurrentPosition(file) ? 0x0000 : HostFileError();

    bytesWritten = SDL_WriteIO(file, sourceBuffer, byteCount);
    if (SDL_GetIOStatus(file) == SDL_IO_STATUS_ERROR)
        return HostFileError();
    return (int16)(uint16)bytesWritten;
}

long SDLBackend_SeekFile(uint16 fileHandle, long fileOffset, byte origin)
{
    SDL_IOStream *file;
    SDL_IOWhence whence;
    Sint64 position;

    file = GetFile(fileHandle);
    if (file == 0)
        return -EBADF;
    if (origin > 0x02)
        return -EINVAL;

    whence = origin == 0x00 ? SDL_IO_SEEK_SET
    : origin == 0x01 ? SDL_IO_SEEK_CUR
    : SDL_IO_SEEK_END;
    position = SDL_SeekIO(file, (Sint64)fileOffset, whence);
    if (position < 0 || position > INT32_MAX)
        return HostFileError();
    return (long)position;
}

int16 SDLBackend_DeleteFile(const char *filePath)
{
    char resolvedPath[NormalizedPathCapacity];

    if (filePath == 0)
        return -EINVAL;
    filePath = ResolveHostPath(filePath, resolvedPath);
    return SDL_RemovePath(filePath) ? 0x0000 : HostFileError();
}

int16 SDLBackend_GetSetFileAttributes(
    const char *filePath,
    byte operation,
    uint16 attributes)
{
    struct stat status;
    char resolvedPath[NormalizedPathCapacity];

    if (filePath == 0)
        return -EINVAL;
    filePath = ResolveHostPath(filePath, resolvedPath);
    if (stat(filePath, &status) != 0)
        return HostFileError();

    if (operation == DOS_FILE_ATTRIBUTES_GET)
    {
        #if defined(_WIN32)
        return (status.st_mode & _S_IWRITE) == 0 ? DosAttributeReadOnly : 0x0000;
        #else
        return (status.st_mode & S_IWUSR) == 0 ? DosAttributeReadOnly : 0x0000;
        #endif
    }

    if (operation != DOS_FILE_ATTRIBUTES_SET)
        return -EINVAL;

    #if defined(_WIN32)
    if (_chmod(filePath,
        (attributes & DosAttributeReadOnly) != 0
        ? _S_IREAD
        : _S_IREAD | _S_IWRITE) != 0)
        return HostFileError();
    #else
    if ((attributes & DosAttributeReadOnly) != 0)
        status.st_mode &= ~(S_IWUSR | S_IWGRP | S_IWOTH);
    else
        status.st_mode |= S_IWUSR;
    if (chmod(filePath, status.st_mode) != 0)
        return HostFileError();
    #endif
    return (int16)attributes;
}

int16 SDLBackend_DeviceIOControl(
    uint16 fileHandle,
    uint16 operation,
    void *data,
    uint16 count)
{
    (void)data;
    (void)count;

    if (GetFile(fileHandle) == 0)
        return -EBADF;

    if (operation == 0x0000 || operation == 0x0001)
        return 0x0000;
    return -EINVAL;
}
