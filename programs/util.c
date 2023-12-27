/*
    util.h - utility functions
    Copyright (C) 2023, Yann Collet

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#if defined (__cplusplus)
extern "C" {
#endif


/*-****************************************
*  Dependencies
******************************************/
#include "util.h"   /* note : ensure that platform.h is included first ! */
#include <stdio.h>  /* FILE*, perror */
#include <errno.h>
#include <assert.h>

/*-****************************************
*  count the number of cores
******************************************/

#if defined(_WIN32) || defined(WIN32)

#include <windows.h>

typedef BOOL(WINAPI* LPFN_GLPI)(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION, PDWORD);

DWORD CountSetBits(ULONG_PTR bitMask)
{
    DWORD LSHIFT = sizeof(ULONG_PTR)*8 - 1;
    DWORD bitSetCount = 0;
    ULONG_PTR bitTest = (ULONG_PTR)1 << LSHIFT;
    DWORD i;

    for (i = 0; i <= LSHIFT; ++i)
    {
        bitSetCount += ((bitMask & bitTest)?1:0);
        bitTest/=2;
    }

    return bitSetCount;
}

int UTIL_countCores(void)
{
    static int numCores = 0;
    if (numCores != 0) return numCores;

    {   LPFN_GLPI glpi;
        BOOL done = FALSE;
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buffer = NULL;
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION ptr = NULL;
        DWORD returnLength = 0;
        size_t byteOffset = 0;

#if defined(_MSC_VER)
/* Visual Studio does not like the following cast */
#   pragma warning( disable : 4054 )  /* conversion from function ptr to data ptr */
#   pragma warning( disable : 4055 )  /* conversion from data ptr to function ptr */
#endif
        glpi = (LPFN_GLPI)(void*)GetProcAddress(GetModuleHandle(TEXT("kernel32")),
                                               "GetLogicalProcessorInformation");

        if (glpi == NULL) {
            goto failed;
        }

        while(!done) {
            DWORD rc = glpi(buffer, &returnLength);
            if (FALSE == rc) {
                if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                    if (buffer)
                        free(buffer);
                    buffer = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION)malloc(returnLength);

                    if (buffer == NULL) {
                        perror("lz4");
                        exit(1);
                    }
                } else {
                    /* some other error */
                    goto failed;
                }
            } else {
                done = TRUE;
        }   }

        ptr = buffer;

        while (byteOffset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) <= returnLength) {

            if (ptr->Relationship == RelationProcessorCore) {
                numCores += CountSetBits(ptr->ProcessorMask);
            }

            ptr++;
            byteOffset += sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
        }

        free(buffer);

        return numCores;
    }

failed:
    /* try to fall back on GetSystemInfo */
    {   SYSTEM_INFO sysinfo;
        GetSystemInfo(&sysinfo);
        numCores = sysinfo.dwNumberOfProcessors;
        if (numCores == 0) numCores = 1; /* just in case */
    }
    return numCores;
}

#elif defined(__APPLE__)

#include <sys/sysctl.h>

/* Use apple-provided syscall
 * see: man 3 sysctl */
int UTIL_countCores(void)
{
    static S32 numCores = 0; /* apple specifies int32_t */
    if (numCores != 0) return (int)numCores;

    {   size_t size = sizeof(S32);
        int const ret = sysctlbyname("hw.logicalcpu", &numCores, &size, NULL, 0);
        if (ret != 0) {
            /* error: fall back on 1 */
            numCores = 1;
        }
    }
    return (int)numCores;
}

#elif defined(__linux__)

/* parse /proc/cpuinfo
 * siblings / cpu cores should give hyperthreading ratio
 * otherwise fall back on sysconf */
int UTIL_countCores(void)
{
    static int numCores = 0;

    if (numCores != 0) return numCores;

    numCores = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (numCores == -1) {
        /* value not queryable, fall back on 1 */
        return numCores = 1;
    }

    /* try to determine if there's hyperthreading */
    {   FILE* const cpuinfo = fopen("/proc/cpuinfo", "r");
#define BUF_SIZE 80
        char buff[BUF_SIZE];

        int siblings = 0;
        int cpu_cores = 0;
        int ratio = 1;

        if (cpuinfo == NULL) {
            /* fall back on the sysconf value */
            return numCores;
        }

        /* assume the cpu cores/siblings values will be constant across all
         * present processors */
        while (!feof(cpuinfo)) {
            if (fgets(buff, BUF_SIZE, cpuinfo) != NULL) {
                if (strncmp(buff, "siblings", 8) == 0) {
                    const char* const sep = strchr(buff, ':');
                    if (sep == NULL || *sep == '\0') {
                        /* formatting was broken? */
                        goto failed;
                    }

                    siblings = atoi(sep + 1);
                }
                if (strncmp(buff, "cpu cores", 9) == 0) {
                    const char* const sep = strchr(buff, ':');
                    if (sep == NULL || *sep == '\0') {
                        /* formatting was broken? */
                        goto failed;
                    }

                    cpu_cores = atoi(sep + 1);
                }
            } else if (ferror(cpuinfo)) {
                /* fall back on the sysconf value */
                goto failed;
        }   }
        if (siblings && cpu_cores && siblings > cpu_cores) {
            ratio = siblings / cpu_cores;
        }

failed:
        fclose(cpuinfo);
        return numCores;
    }
}

#elif defined(__FreeBSD__)

#include <sys/param.h>
#include <sys/sysctl.h>

/* Use physical core sysctl when available
 * see: man 4 smp, man 3 sysctl */
int UTIL_countCores(void)
{
    static int numCores = 0; /* freebsd sysctl is native int sized */
    if (numCores != 0) return numCores;

#if __FreeBSD_version >= 1300008
    {   size_t size = sizeof(numCores);
        int ret = sysctlbyname("kern.smp.cores", &numCores, &size, NULL, 0);
        if (ret == 0) {
            int perCore = 1;
            ret = sysctlbyname("kern.smp.threads_per_core", &perCore, &size, NULL, 0);
            /* default to physical cores if logical cannot be read */
            if (ret != 0) /* error */
                return numCores;
            numCores *= perCore;
            return numCores;
        }
        if (errno != ENOENT) {
            perror("lz4: can't get number of cpus");
            exit(1);
        }
        /* sysctl not present, fall through to older sysconf method */
    }
#endif

    numCores = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (numCores == -1) {
        /* value not queryable, fall back on 1 */
        numCores = 1;
    }
    return numCores;
}

#elif defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__) || defined(__CYGWIN__)

/* Use POSIX sysconf
 * see: man 3 sysconf */
int UTIL_countCores(void)
{
    static int numCores = 0;
    if (numCores != 0) return numCores;

    numCores = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (numCores == -1) {
        /* value not queryable, fall back on 1 */
        numCores = 1;
    }
    return numCores;
}

#else

int UTIL_countCores(void)
{
    /* no clue */
    return 1;
}

#endif

#if defined (__cplusplus)
}
#endif
