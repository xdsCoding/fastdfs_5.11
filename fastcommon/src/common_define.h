/**
* Copyright (C) 2008 Happy Fish / YuQing
*
* FastDFS may be copied only under the terms of the GNU General
* Public License V3, which may be found in the FastDFS source kit.
* Please visit the FastDFS Home Page http://www.csource.org/ for more detail.
**/

//common_define.h

#ifndef _COMMON_DEFINE_H_
#define _COMMON_DEFINE_H_

#include <pthread.h>
#include <string.h>
#include <errno.h>

#ifdef WIN32

#include <windows.h>
#include <winsock.h>
typedef UINT in_addr_t;
#define FILE_SEPERATOR	"\\"
#define THREAD_ENTRANCE_FUNC_DECLARE  DWORD WINAPI
#define THREAD_RETURN_VALUE	 0
typedef DWORD (WINAPI *ThreadEntranceFunc)(LPVOID lpThreadParameter);
#else

#include <unistd.h>
#include <signal.h>
#include <stdbool.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#define FILE_SEPERATOR	"/"
typedef int SOCKET;
#define closesocket     close
#define INVALID_SOCKET  -1
#define THREAD_ENTRANCE_FUNC_DECLARE  void *
typedef void *LPVOID;
#define THREAD_RETURN_VALUE	 NULL
typedef void * (*ThreadEntranceFunc)(LPVOID lpThreadParameter);

#endif

#ifndef WIN32
extern int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int kind);
#endif

#include "_os_define.h"

#ifdef OS_LINUX
#ifndef PTHREAD_MUTEX_ERRORCHECK
#define PTHREAD_MUTEX_ERRORCHECK PTHREAD_MUTEX_ERRORCHECK_NP
#endif
#endif

#ifdef OS_BITS
  #if OS_BITS == 64
    #define INT64_PRINTF_FORMAT   "%ld"
  #else
    #define INT64_PRINTF_FORMAT   "%lld"
  #endif
#else
  #define INT64_PRINTF_FORMAT   "%lld"
#endif

#ifdef OFF_BITS
  #if OFF_BITS == 64
    #define OFF_PRINTF_FORMAT   INT64_PRINTF_FORMAT
  #else
    #define OFF_PRINTF_FORMAT   "%d"
  #endif
#else
  #define OFF_PRINTF_FORMAT   INT64_PRINTF_FORMAT
#endif

#ifndef WIN32
#define USE_SENDFILE
#endif

#define MAX_PATH_SIZE				256
#define LOG_FILE_DIR				"logs"
#define CONF_FILE_DIR				"conf"
#define DEFAULT_CONNECT_TIMEOUT			30
#define DEFAULT_NETWORK_TIMEOUT			30
#define DEFAULT_MAX_CONNECTONS			256
#define DEFAULT_WORK_THREADS			4
#define SYNC_LOG_BUFF_DEF_INTERVAL              10
#define TIME_NONE                               -1

/************ add by xiaods start ***********/
//#define IP_ADDRESS_SIZE	16
#define IP_ADDRESS_SIZE	46
#define CONFIG_BINDADDR_MAX 16
#define NETWORK_CARD_SIZE 32
/************ add by xiaods end ***********/

#define INFINITE_FILE_SIZE (256 * 1024LL * 1024 * 1024 * 1024 * 1024LL)
//add by yb 20170817 start
#define ALLOC_IP_ASDDR_MEM_SIZE_ONCE        10
#define IP_ADDRESS_PORT_SIZE 32
//end


#ifndef byte
#define byte signed char
#endif

#ifndef ubyte
#define ubyte unsigned char
#endif

#ifndef WIN32
#ifndef INADDR_NONE
#define  INADDR_NONE  ((in_addr_t) 0xffffffff)
#endif
#endif

#ifndef ECANCELED
#define ECANCELED 125
#endif

#ifndef ENONET
#define ENONET          64      /* Machine is not on the network */
#endif

#define IS_UPPER_HEX(ch) ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F'))
#define IS_HEX_CHAR(ch)  (IS_UPPER_HEX(ch) || (ch >= 'a' && ch <= 'f'))

#define STRERROR(no) (strerror(no) != NULL ? strerror(no) : "Unkown error")

#if defined(OS_LINUX)
#if defined __USE_MISC || defined __USE_XOPEN2K8
#define st_atimensec st_atim.tv_nsec
#define st_mtimensec st_mtim.tv_nsec
#define st_ctimensec st_ctim.tv_nsec
#endif
#elif defined(OS_FREEBSD)
#define st_atimensec st_atimespec.tv_nsec
#define st_mtimensec st_mtimespec.tv_nsec
#define st_ctimensec st_ctimespec.tv_nsec
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
	byte hour;
	byte minute;
	byte second;
} TimeInfo;

typedef struct
{
	char major;
	char minor;
    char patch;
} Version;

typedef struct
{
	char *key;
	char *value;
} KeyValuePair;

typedef struct
{
	char *key;
	char *value;
    int key_len;
    int value_len;
} KeyValuePairEx;

typedef struct
{
	char *buff;
	int alloc_size;
	int length;
} BufferInfo;

typedef struct
{
    char *str;
    int len;
} string_t;

typedef void (*FreeDataFunc)(void *ptr);
typedef int (*CompareFunc)(void *p1, void *p2);
typedef void* (*MallocFunc)(size_t size);

#define TO_UPPERCASE(c)  (((c) >= 'a' && (c) <= 'z') ? (c) - 32 : c)
#define MEM_ALIGN(x)  (((x) + 7) & (~7))

#ifdef WIN32
#define strcasecmp	_stricmp
#endif

#ifndef likely

#if defined(__GNUC__) &&  __GNUC__ >= 3
#define likely(cond)   __builtin_expect ((cond), 1)
#define unlikely(cond) __builtin_expect ((cond), 0)
#else
#define likely(cond)   (cond)
#define unlikely(cond) (cond)
#endif

#endif

#ifdef __GNUC__
  #define __gcc_attribute__ __attribute__
#else
  #define __gcc_attribute__(x)
#endif

static inline int fc_compare_string(const string_t *s1, const string_t *s2)
{
    int result;
    if (s1->len == s2->len) {
        return memcmp(s1->str, s2->str, s1->len);
    } else if (s1->len < s2->len) {
        result = memcmp(s1->str, s2->str, s1->len);
        return result == 0 ? -1 : result;
    } else {
        result = memcmp(s1->str, s2->str, s2->len);
        return result == 0 ? 1 : result;
    }
}

#ifdef __cplusplus
}
#endif

#endif
