/*
 * freestanding.c
 *
 * The required Mettle runtime. This file must compile without libc, a C
 * startup object, or compiler support libraries. It provides the small C ABI
 * surface that generated programs use and delegates only to the host kernel
 * or to documented operating system APIs.
 */

#if defined(__GNUC__) || defined(__clang__)
#define MT_NORETURN __attribute__((noreturn))
#else
#define MT_NORETURN __declspec(noreturn)
#endif

typedef unsigned char mt_u8;
typedef unsigned short mt_u16;
typedef unsigned int mt_u32;
typedef int mt_i32;
typedef unsigned long long mt_u64;
typedef long long mt_i64;
typedef mt_u64 mt_size;
typedef mt_i64 mt_ssize;
typedef __builtin_va_list mt_va_list;

#define MT_NULL ((void *)0)
#define MT_SIZE_MAX ((mt_size)~(mt_size)0)

MT_NORETURN void abort(void);
MT_NORETURN void _exit(int status);
int isspace(int character);
int tolower(int character);
void *malloc(mt_size size);
void free(void *memory);
struct MtFile;
static int mt_stream_flush(struct MtFile *file);
static void mt_open_write_track(struct MtFile *file);
static void mt_open_write_forget(struct MtFile *file);
static void mt_flush_open_streams(void);

/* read_buffer is allocated on the first buffered read and only for a file
 * opened read-only, which is what lets fgets stop asking the kernel for one
 * byte at a time. See mt_stream_read. */
typedef struct MtFile {
  mt_i64 handle;
  mt_u32 flags;
  mt_i64 child_pid;
  unsigned char *read_buffer;
  int read_fill;
  int read_pos;
  unsigned char *write_buffer;
  int write_fill;
  struct MtFile *next_open;
} MtFile;

#define MT_FILE_READ 1u
#define MT_FILE_WRITE 2u
#define MT_FILE_BUFFERED 4u
#define MT_FILE_WRITE_BUFFERED 8u
#define MT_FILE_STANDARD 0x80000000u
#define MT_FILE_BUFFER_BYTES 8192
#define MT_FILE_WRITE_BUFFER_BYTES 65536

static MtFile mt_stdin_file = {0, MT_FILE_STANDARD | MT_FILE_READ, 0, 0, 0, 0, 0, 0, 0};
static MtFile mt_stdout_file = {1, MT_FILE_STANDARD | MT_FILE_WRITE, 0, 0, 0, 0, 0, 0, 0};
static MtFile mt_stderr_file = {2, MT_FILE_STANDARD | MT_FILE_WRITE, 0, 0, 0, 0, 0, 0, 0};
/* A shared object reaches a thread-local at a fixed offset from the thread
 * pointer, which is a program's privilege: the offset a loaded library would
 * need is not known until the loader places it. The shared build of this
 * runtime therefore keeps errno per process. Its threads are the loading
 * program's, and a library that wants per-thread errno belongs in the
 * program. */
#if defined(_WIN32) || defined(MT_SHARED_RUNTIME)
static int mt_errno_value;
#else
static __thread int mt_errno_value;
#endif
static volatile mt_u64 mt_allocation_count;
static volatile mt_u64 mt_free_count;

void *stdin = &mt_stdin_file;
void *stdout = &mt_stdout_file;
void *stderr = &mt_stderr_file;

#define MT_WORD_ONES 0x0101010101010101ull
#define MT_WORD_HIGH 0x8080808080808080ull
#define MT_WORD_ALIGN(p) ((mt_u64)(mt_size)(const void *)(p) & 7u)

static mt_u64 mt_word_zero_mask(mt_u64 word) {
  return (word - MT_WORD_ONES) & ~word & MT_WORD_HIGH;
}

void *memset(void *destination, int value, mt_size count) {
  mt_u8 *out = (mt_u8 *)destination;
  mt_u8 byte = (mt_u8)value;
  mt_u64 pattern = (mt_u64)byte * MT_WORD_ONES;

  while (count >= 32) {
    __builtin_memcpy(out, &pattern, 8);
    __builtin_memcpy(out + 8, &pattern, 8);
    __builtin_memcpy(out + 16, &pattern, 8);
    __builtin_memcpy(out + 24, &pattern, 8);
    out += 32;
    count -= 32;
  }
  while (count >= 8) {
    __builtin_memcpy(out, &pattern, 8);
    out += 8;
    count -= 8;
  }
  while (count--) {
    *out++ = byte;
  }
  return destination;
}

void *memcpy(void *destination, const void *source, mt_size count) {
  mt_u8 *out = (mt_u8 *)destination;
  const mt_u8 *in = (const mt_u8 *)source;

  while (count >= 32) {
    mt_u64 a, b, c, d;
    __builtin_memcpy(&a, in, 8);
    __builtin_memcpy(&b, in + 8, 8);
    __builtin_memcpy(&c, in + 16, 8);
    __builtin_memcpy(&d, in + 24, 8);
    __builtin_memcpy(out, &a, 8);
    __builtin_memcpy(out + 8, &b, 8);
    __builtin_memcpy(out + 16, &c, 8);
    __builtin_memcpy(out + 24, &d, 8);
    out += 32;
    in += 32;
    count -= 32;
  }
  while (count >= 8) {
    mt_u64 a;
    __builtin_memcpy(&a, in, 8);
    __builtin_memcpy(out, &a, 8);
    out += 8;
    in += 8;
    count -= 8;
  }
  while (count--) {
    *out++ = *in++;
  }
  return destination;
}

void *memmove(void *destination, const void *source, mt_size count) {
  mt_u8 *out = (mt_u8 *)destination;
  const mt_u8 *in = (const mt_u8 *)source;
  if (out == in || count == 0) {
    return destination;
  }
  if (out < in || out >= in + count) {
    return memcpy(destination, source, count);
  }
  while (count >= 8) {
    mt_u64 a;
    count -= 8;
    __builtin_memcpy(&a, in + count, 8);
    __builtin_memcpy(out + count, &a, 8);
  }
  while (count != 0) {
    count--;
    out[count] = in[count];
  }
  return destination;
}

void *memchr(const void *memory, int character, mt_size count) {
  const mt_u8 *bytes = (const mt_u8 *)memory;
  mt_u64 pattern = (mt_u64)(mt_u8)character * MT_WORD_ONES;

  while (count >= 8) {
    mt_u64 word;
    mt_u64 hit;
    __builtin_memcpy(&word, bytes, 8);
    hit = mt_word_zero_mask(word ^ pattern);
    if (hit) {
      return (void *)(bytes + (__builtin_ctzll(hit) >> 3));
    }
    bytes += 8;
    count -= 8;
  }
  while (count--) {
    if (*bytes == (mt_u8)character) {
      return (void *)bytes;
    }
    bytes++;
  }
  return MT_NULL;
}

int memcmp(const void *left, const void *right, mt_size count) {
  const mt_u8 *a = (const mt_u8 *)left;
  const mt_u8 *b = (const mt_u8 *)right;

  while (count >= 8) {
    mt_u64 x, y;
    __builtin_memcpy(&x, a, 8);
    __builtin_memcpy(&y, b, 8);
    if (x != y) {
      mt_u64 lane = (mt_u64)(__builtin_ctzll(x ^ y) & ~7u);
      return (int)((x >> lane) & 0xffu) - (int)((y >> lane) & 0xffu);
    }
    a += 8;
    b += 8;
    count -= 8;
  }
  while (count--) {
    if (*a != *b) {
      return (int)*a - (int)*b;
    }
    a++;
    b++;
  }
  return 0;
}

#if defined(MTLC_HOST_PREFIX_H)
/*
 * As the host runtime the definitions above are renamed to mtlc_host_*, but a
 * compiler still lowers struct copies and zero-init to calls that name memcpy
 * and memset literally, and no macro can reach those. Export the plain names
 * as well, for the same reason ___chkstk_ms below is written in asm.
 */
#undef memcpy
#undef memset
#undef memmove
#undef memcmp

void *memcpy(void *destination, const void *source, mt_size count) {
  return mtlc_host_memcpy(destination, source, count);
}

void *memset(void *destination, int value, mt_size count) {
  return mtlc_host_memset(destination, value, count);
}

void *memmove(void *destination, const void *source, mt_size count) {
  return mtlc_host_memmove(destination, source, count);
}

int memcmp(const void *left, const void *right, mt_size count) {
  return mtlc_host_memcmp(left, right, count);
}
#endif

mt_size strlen(const char *text) {
  const char *cursor = text;

  if (!text) {
    return 0;
  }
  while (MT_WORD_ALIGN(cursor) != 0) {
    if (!*cursor) {
      return (mt_size)(cursor - text);
    }
    cursor++;
  }
  for (;;) {
    mt_u64 word;
    mt_u64 zero;
    __builtin_memcpy(&word, cursor, 8);
    zero = mt_word_zero_mask(word);
    if (zero) {
      return (mt_size)(cursor - text) + (mt_size)(__builtin_ctzll(zero) >> 3);
    }
    cursor += 8;
  }
}

int strcmp(const char *left, const char *right) {
  if (MT_WORD_ALIGN(left) == MT_WORD_ALIGN(right)) {
    while (MT_WORD_ALIGN(left) != 0) {
      mt_u8 a = (mt_u8)*left;
      mt_u8 b = (mt_u8)*right;
      if (a != b || a == 0) {
        return (int)a - (int)b;
      }
      left++;
      right++;
    }
    for (;;) {
      mt_u64 x, y;
      __builtin_memcpy(&x, left, 8);
      __builtin_memcpy(&y, right, 8);
      if (x != y || mt_word_zero_mask(x)) {
        break;
      }
      left += 8;
      right += 8;
    }
  }
  {
    mt_size i = 0;
    while (left[i] && left[i] == right[i]) {
      i++;
    }
    return (int)(mt_u8)left[i] - (int)(mt_u8)right[i];
  }
}

int strncmp(const char *left, const char *right, mt_size count) {
  for (mt_size i = 0; i < count; i++) {
    mt_u8 a = (mt_u8)left[i];
    mt_u8 b = (mt_u8)right[i];
    if (a != b || a == 0) {
      return (int)a - (int)b;
    }
  }
  return 0;
}

char *strchr(const char *text, int character) {
  char wanted = (char)character;
  mt_u64 pattern = (mt_u64)(mt_u8)wanted * MT_WORD_ONES;

  while (MT_WORD_ALIGN(text) != 0) {
    if (*text == wanted) {
      return (char *)text;
    }
    if (!*text++) {
      return MT_NULL;
    }
  }
  for (;;) {
    mt_u64 word;
    mt_u64 zero;
    mt_u64 hit;
    __builtin_memcpy(&word, text, 8);
    zero = mt_word_zero_mask(word);
    hit = mt_word_zero_mask(word ^ pattern);
    if (hit | zero) {
      if (hit && (!zero || __builtin_ctzll(hit) <= __builtin_ctzll(zero))) {
        return (char *)text + (__builtin_ctzll(hit) >> 3);
      }
      return MT_NULL;
    }
    text += 8;
  }
}

char *strrchr(const char *text, int character) {
  const char *found = MT_NULL;
  char wanted = (char)character;
  do {
    if (*text == wanted) {
      found = text;
    }
  } while (*text++);
  return (char *)found;
}

char *strncpy(char *destination, const char *source, mt_size count) {
  mt_size i = 0;
  while (i < count && source[i]) {
    destination[i] = source[i];
    i++;
  }
  while (i < count) {
    destination[i++] = 0;
  }
  return destination;
}

char *strcpy(char *destination, const char *source) {
  char *out = destination;
  while ((*out++ = *source++) != 0) {
  }
  return destination;
}

char *strcat(char *destination, const char *source) {
  strcpy(destination + strlen(destination), source);
  return destination;
}

char *strpbrk(const char *text, const char *characters) {
  for (; *text; text++) {
    if (strchr(characters, *text)) {
      return (char *)text;
    }
  }
  return MT_NULL;
}

int strcasecmp(const char *left, const char *right) {
  while (*left && tolower((mt_u8)*left) == tolower((mt_u8)*right)) {
    left++;
    right++;
  }
  return tolower((mt_u8)*left) - tolower((mt_u8)*right);
}

char *strstr(const char *text, const char *part) {
  char first = part[0];
  mt_size rest;

  if (!first) {
    return (char *)text;
  }
  rest = strlen(part + 1);
  for (;;) {
    text = strchr(text, first);
    if (!text) {
      return MT_NULL;
    }
    if (strncmp(text + 1, part + 1, rest) == 0) {
      return (char *)text;
    }
    text++;
  }
}

static char *mt_strtok_next = MT_NULL;

char *strtok(char *text, const char *delimiters) {
  char *start = text ? text : mt_strtok_next;
  if (!start) {
    return MT_NULL;
  }
  while (*start && strchr(delimiters, *start)) {
    start++;
  }
  if (!*start) {
    mt_strtok_next = MT_NULL;
    return MT_NULL;
  }
  char *end = start;
  while (*end && !strchr(delimiters, *end)) {
    end++;
  }
  if (*end) {
    *end++ = 0;
  }
  mt_strtok_next = *end ? end : MT_NULL;
  return start;
}

static int mt_append_md_path(char *paths, mt_i32 capacity, mt_i32 *used,
                             mt_i32 *count, mt_i32 limit,
                             const char *relative_path) {
  mt_size length;
  if (!paths || !used || !count || !relative_path || *count >= limit) {
    return 0;
  }
  length = strlen(relative_path);
  if (length == 0 || length > 0x7fffffffu ||
      *used > capacity - (mt_i32)length - 1) {
    return 0;
  }
  memcpy(paths + *used, relative_path, length + 1);
  *used += (mt_i32)length + 1;
  (*count)++;
  return 1;
}

static int mt_is_markdown_name(const char *name) {
  mt_size length = strlen(name);
  return length >= 3 && strcasecmp(name + length - 3, ".md") == 0;
}

#if defined(_WIN32)

#define MT_DLLIMPORT __declspec(dllimport)
#define MT_INVALID_HANDLE ((void *)(mt_i64)-1)
#define MT_HEAP_ZERO_MEMORY 0x00000008u
#define MT_MEM_COMMIT 0x00001000u
#define MT_MEM_RESERVE 0x00002000u
#define MT_PAGE_READWRITE 0x00000004u
#define MT_STD_INPUT_HANDLE ((mt_u32)-10)
#define MT_STD_OUTPUT_HANDLE ((mt_u32)-11)
#define MT_STD_ERROR_HANDLE ((mt_u32)-12)
#define MT_GENERIC_READ 0x80000000u
#define MT_GENERIC_WRITE 0x40000000u
#define MT_FILE_SHARE_READ 0x00000001u
#define MT_OPEN_EXISTING 3u
#define MT_CREATE_ALWAYS 2u
#define MT_OPEN_ALWAYS 4u
#define MT_FILE_ATTRIBUTE_NORMAL 0x00000080u
#define MT_FILE_END 2u

typedef struct MtProcessInfo {
  void *process;
  void *thread;
  mt_u32 process_id;
  mt_u32 thread_id;
} MtProcessInfo;

typedef struct MtStartupInfo {
  mt_u32 size;
  char *reserved;
  char *desktop;
  char *title;
  mt_u32 x;
  mt_u32 y;
  mt_u32 x_size;
  mt_u32 y_size;
  mt_u32 x_chars;
  mt_u32 y_chars;
  mt_u32 fill;
  mt_u32 flags;
  unsigned short show;
  unsigned short reserved_count;
  mt_u8 *reserved_bytes;
  void *input;
  void *output;
  void *error;
} MtStartupInfo;

typedef struct MtSecurityAttributes {
  mt_u32 size;
  void *security_descriptor;
  int inherit_handle;
} MtSecurityAttributes;

typedef struct MtFileTime {
  mt_u32 low;
  mt_u32 high;
} MtFileTime;

typedef struct MtFindData {
  mt_u32 attributes;
  MtFileTime creation_time;
  MtFileTime access_time;
  MtFileTime write_time;
  mt_u32 size_high;
  mt_u32 size_low;
  mt_u32 reserved0;
  mt_u32 reserved1;
  char name[260];
  char alternate_name[14];
} MtFindData;

MT_DLLIMPORT void *VirtualAlloc(void *address, mt_size size, mt_u32 type,
                               mt_u32 protect);
MT_DLLIMPORT void *GetProcessHeap(void);
MT_DLLIMPORT void *HeapAlloc(void *heap, mt_u32 flags, mt_size bytes);
MT_DLLIMPORT void *HeapReAlloc(void *heap, mt_u32 flags, void *memory,
                               mt_size bytes);
MT_DLLIMPORT int HeapFree(void *heap, mt_u32 flags, void *memory);
MT_DLLIMPORT void *GetStdHandle(mt_u32 which);
MT_DLLIMPORT int ReadFile(void *file, void *buffer, mt_u32 count,
                          mt_u32 *read_count, void *overlapped);
MT_DLLIMPORT int WriteFile(void *file, const void *buffer, mt_u32 count,
                           mt_u32 *write_count, void *overlapped);
MT_DLLIMPORT void *CreateFileA(const char *path, mt_u32 access, mt_u32 sharing,
                               void *security, mt_u32 creation,
                               mt_u32 attributes, void *template_file);
MT_DLLIMPORT void *CreateFileW(const mt_u16 *path, mt_u32 access, mt_u32 sharing,
                               void *security, mt_u32 creation,
                               mt_u32 attributes, void *template_file);
MT_DLLIMPORT int CloseHandle(void *handle);
MT_DLLIMPORT int SetFilePointerEx(void *file, mt_i64 distance,
                                  mt_i64 *new_position, mt_u32 method);
MT_DLLIMPORT int DeleteFileA(const char *path);
MT_DLLIMPORT int DeleteFileW(const mt_u16 *path);
MT_DLLIMPORT int GetConsoleMode(void *handle, mt_u32 *mode);
#define MT_CP_UTF8 65001u
MT_DLLIMPORT int SetConsoleOutputCP(mt_u32 code_page);
MT_DLLIMPORT mt_u32 GetConsoleOutputCP(void);
MT_DLLIMPORT char *GetCommandLineA(void);
MT_DLLIMPORT mt_u16 *GetCommandLineW(void);

static mt_u16 *mt_widen_path(const char *path);
MT_DLLIMPORT mt_u32 GetEnvironmentVariableA(const char *name, char *buffer,
                                             mt_u32 size);
MT_DLLIMPORT int SetEnvironmentVariableA(const char *name, const char *value);
MT_DLLIMPORT mt_u32 GetCurrentDirectoryA(mt_u32 size, char *buffer);
MT_DLLIMPORT mt_u32 GetModuleFileNameA(void *module, char *buffer, mt_u32 size);
MT_DLLIMPORT mt_u32 SearchPathA(const char *path, const char *file,
                                const char *extension, mt_u32 size,
                                char *buffer, char **file_part);
MT_DLLIMPORT mt_u32 GetFullPathNameA(const char *path, mt_u32 size,
                                     char *buffer, char **file_part);
MT_DLLIMPORT mt_u32 GetFileAttributesA(const char *path);
MT_DLLIMPORT int CreateDirectoryA(const char *path, void *security);
MT_DLLIMPORT void *FindFirstFileA(const char *pattern, MtFindData *data);
MT_DLLIMPORT int FindNextFileA(void *handle, MtFindData *data);
MT_DLLIMPORT int FindClose(void *handle);
MT_DLLIMPORT void GetSystemTimeAsFileTime(MtFileTime *time);
MT_DLLIMPORT int CreatePipe(void **read_pipe, void **write_pipe,
                            MtSecurityAttributes *attributes, mt_u32 size);
MT_DLLIMPORT int SetHandleInformation(void *handle, mt_u32 mask,
                                      mt_u32 flags);
MT_DLLIMPORT mt_u64 GetTickCount64(void);
MT_DLLIMPORT mt_u32 GetCurrentThreadId(void);
MT_DLLIMPORT mt_u32 FlsAlloc(void (*callback)(void *));
MT_DLLIMPORT void *FlsGetValue(mt_u32 index);
MT_DLLIMPORT int FlsSetValue(mt_u32 index, void *value);
MT_DLLIMPORT int CreateProcessA(const char *application, char *command_line,
                                void *process_security, void *thread_security,
                                int inherit_handles, mt_u32 flags,
                                void *environment, const char *directory,
                                MtStartupInfo *startup,
                                MtProcessInfo *process_info);
MT_DLLIMPORT mt_u32 WaitForSingleObject(void *handle, mt_u32 milliseconds);
MT_DLLIMPORT int GetExitCodeProcess(void *process, mt_u32 *exit_code);
MT_DLLIMPORT MT_NORETURN void ExitProcess(mt_u32 status);

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
__asm__(".text\n"
        ".p2align 4\n"
        ".globl ___chkstk_ms\n"
        "___chkstk_ms:\n"
        "pushq %rcx\n"
        "pushq %rax\n"
        "cmpq $0x1000, %rax\n"
        "leaq 24(%rsp), %rcx\n"
        "jb 2f\n"
        "1:\n"
        "subq $0x1000, %rcx\n"
        "testb $0, (%rcx)\n"
        "subq $0x1000, %rax\n"
        "cmpq $0x1000, %rax\n"
        "ja 1b\n"
        "2:\n"
        "subq %rax, %rcx\n"
        "testb $0, (%rcx)\n"
        "popq %rax\n"
        "popq %rcx\n"
        "ret\n");
#endif

/* One slot per distinct NAME, not one shared buffer for every call: callers
 * routinely cache getenv's pointer (POSIX allows the value to be overwritten
 * by a later getenv, but every real CRT keeps it stable per name, and the
 * compiler's own pass-skip cache relied on that). The single shared buffer
 * this replaces meant any later getenv of ANY variable silently rewrote what
 * a cached pointer read -- METTLE_SKIP_PASS came back holding the value of
 * whichever variable was asked for last. */
#define MT_ENV_SLOTS 64
#define MT_ENV_NAME_MAX 128
#define MT_ENV_VALUE_MAX 32768
static struct {
  char name[MT_ENV_NAME_MAX];
  char *value; /* heap; grows to MT_ENV_VALUE_MAX at most */
} mt_environment_slots[MT_ENV_SLOTS];
static mt_size mt_environment_slot_count;

char *getenv(const char *name) {
  if (!name || strlen(name) >= MT_ENV_NAME_MAX) {
    return MT_NULL;
  }
  mt_size slot = mt_environment_slot_count;
  for (mt_size i = 0; i < mt_environment_slot_count; i++) {
    if (strcmp(mt_environment_slots[i].name, name) == 0) {
      slot = i;
      break;
    }
  }
  if (slot == mt_environment_slot_count) {
    if (slot >= MT_ENV_SLOTS) {
      return MT_NULL; /* more distinct names than any build ever asks for */
    }
  }
  if (!mt_environment_slots[slot].value) {
    mt_environment_slots[slot].value = malloc(MT_ENV_VALUE_MAX);
    if (!mt_environment_slots[slot].value) {
      return MT_NULL;
    }
  }
  mt_u32 length = GetEnvironmentVariableA(name, mt_environment_slots[slot].value,
                                          MT_ENV_VALUE_MAX);
  if (length == 0 || length >= MT_ENV_VALUE_MAX) {
    return MT_NULL;
  }
  if (slot == mt_environment_slot_count) {
    strcpy(mt_environment_slots[slot].name, name);
    mt_environment_slot_count++;
  }
  return mt_environment_slots[slot].value;
}

int putenv(char *setting) {
  char *equals = setting ? strchr(setting, '=') : MT_NULL;
  if (!equals || equals == setting) {
    mt_errno_value = 22;
    return -1;
  }
  char name[256];
  mt_size length = (mt_size)(equals - setting);
  if (length >= sizeof(name)) {
    mt_errno_value = 22;
    return -1;
  }
  memcpy(name, setting, length);
  name[length] = 0;
  return SetEnvironmentVariableA(name, equals + 1) ? 0 : -1;
}

/* Mettle strings are bytes, and a string literal carries whatever bytes the
 * source file held, which is UTF-8. The Windows console decodes what it is
 * given using its output code page, and that still defaults to a regional
 * legacy page on most machines. So a program printing an accented letter wrote
 * the correct two bytes and the console drew two wrong characters, one per
 * byte. The bytes were never wrong; the console had been told to read them as
 * something else.
 *
 * Saying so at startup is the whole fix. A program that writes UTF-8 should
 * declare it rather than hope the user ran chcp first. This touches only the
 * console: redirected output is bytes either way, and the input code page is
 * left alone because nothing here reads console input as text. */
void mettle_rt_startup(mt_i64 argc, char **argv) {
  (void)argc;
  (void)argv;
  if (GetConsoleOutputCP() != MT_CP_UTF8) {
    SetConsoleOutputCP(MT_CP_UTF8);
  }
}

void __main(void) {}

#define MT_INVALID_FILE_ATTRIBUTES 0xffffffffu
#define MT_FILE_ATTRIBUTE_READONLY 0x00000001u
#define MT_FILE_ATTRIBUTE_DIRECTORY 0x00000010u

int access(const char *path, int mode) {
  mt_u32 attributes = path ? GetFileAttributesA(path)
                           : MT_INVALID_FILE_ATTRIBUTES;
  if (attributes == MT_INVALID_FILE_ATTRIBUTES) return -1;
  if ((mode & 2) && (attributes & MT_FILE_ATTRIBUTE_READONLY)) return -1;
  return 0;
}

int mettle_path_exists(const char *path) { return access(path, 0) == 0; }

int mettle_path_is_directory(const char *path) {
  mt_u32 attributes = path ? GetFileAttributesA(path)
                           : MT_INVALID_FILE_ATTRIBUTES;
  return attributes != MT_INVALID_FILE_ATTRIBUTES &&
         (attributes & MT_FILE_ATTRIBUTE_DIRECTORY) != 0;
}

int mkdir(const char *path, unsigned int mode) {
  (void)mode;
  return CreateDirectoryA(path, MT_NULL) ? 0 : -1;
}

int _mkdir(const char *path) { return mkdir(path, 0); }

int mettle_make_directory(const char *path) { return mkdir(path, 0777); }

mt_i64 mettle_executable_path(char *buffer, mt_u64 size) {
  if (!buffer || size == 0 || size > 0xffffffffu) return -1;
  mt_u32 length = GetModuleFileNameA(MT_NULL, buffer, (mt_u32)size);
  return length == 0 || length >= size ? -1 : (mt_i64)length;
}

char *realpath(const char *path, char *resolved) {
  mt_u32 required;
  if (!path || !mettle_path_exists(path)) return MT_NULL;
  required = GetFullPathNameA(path, 0, MT_NULL, MT_NULL);
  if (!required) return MT_NULL;
  if (!resolved) {
    resolved = (char *)malloc(required);
    if (!resolved) return MT_NULL;
  }
  if (!GetFullPathNameA(path, required, resolved, MT_NULL)) {
    return MT_NULL;
  }
  return resolved;
}

char *mettle_realpath(const char *path, char *resolved) {
  return realpath(path, resolved);
}

int unlink(const char *path) {
  mt_u16 *wide_path = mt_widen_path(path);
  if (!wide_path) {
    return -1;
  }
  int ok = DeleteFileW(wide_path);
  free(wide_path);
  return ok ? 0 : -1;
}

int gettimeofday(void *time_value, void *timezone_value) {
  MtFileTime file_time;
  mt_u64 ticks;
  mt_i64 unix_ticks;
  mt_i32 *fields = (mt_i32 *)time_value;
  (void)timezone_value;
  if (!time_value) return -1;
  GetSystemTimeAsFileTime(&file_time);
  ticks = ((mt_u64)file_time.high << 32) | file_time.low;
  unix_ticks = (mt_i64)ticks - 116444736000000000LL;
  fields[0] = (mt_i32)(unix_ticks / 10000000LL);
  fields[1] = (mt_i32)((unix_ticks % 10000000LL) / 10LL);
  return 0;
}

static char *mt_getcwd_impl(char *buffer, mt_size size) {
  if (!buffer) {
    mt_u32 needed = GetCurrentDirectoryA(0, MT_NULL);
    if (!needed) return MT_NULL;
    buffer = (char *)malloc((mt_size)needed);
    if (!buffer) return MT_NULL;
    if (!GetCurrentDirectoryA(needed, buffer)) {
      free(buffer);
      return MT_NULL;
    }
    return buffer;
  }
  if (size == 0 || size > 0xffffffffu ||
      !GetCurrentDirectoryA((mt_u32)size, buffer)) {
    return MT_NULL;
  }
  return buffer;
}

#ifdef getcwd
char *getcwd(char *buffer, mt_size size) {
  return mt_getcwd_impl(buffer, size);
}
#endif

int mettle_getcwd(char *buffer, mt_i32 size) {
  return buffer && size > 0 && mt_getcwd_impl(buffer, (mt_size)size) ? 0 : -1;
}

int mettle_dir_exists(const char *path) {
  return mettle_path_is_directory(path);
}

int mettle_dir_create(const char *path) { return mkdir(path, 0777); }

int mettle_file_exists(const char *path) {
  mt_u32 attributes = path ? GetFileAttributesA(path)
                           : MT_INVALID_FILE_ATTRIBUTES;
  return attributes != MT_INVALID_FILE_ATTRIBUTES &&
         (attributes & MT_FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static void mt_scan_md_files(const char *root, const char *prefix, char *paths,
                             mt_i32 capacity, mt_i32 *used, mt_i32 *count,
                             mt_i32 limit) {
  char pattern[1024];
  char full_path[1024];
  char relative_path[1024];
  MtFindData data;
  mt_size root_length;
  void *handle;
  if (!root || !paths || !used || !count || *count >= limit) {
    return;
  }
  root_length = strlen(root);
  if (root_length + 3 > sizeof(pattern)) {
    return;
  }
  memcpy(pattern, root, root_length);
  pattern[root_length] = '\\';
  pattern[root_length + 1] = '*';
  pattern[root_length + 2] = 0;
  handle = FindFirstFileA(pattern, &data);
  if (handle == MT_INVALID_HANDLE) {
    return;
  }
  do {
    mt_size name_length = strlen(data.name);
    mt_size prefix_length = prefix ? strlen(prefix) : 0;
    if (strcmp(data.name, ".") == 0 || strcmp(data.name, "..") == 0) {
      continue;
    }
    if (root_length + name_length + 2 > sizeof(full_path) ||
        prefix_length + name_length + (prefix_length ? 2 : 1) >
            sizeof(relative_path)) {
      continue;
    }
    memcpy(full_path, root, root_length);
    full_path[root_length] = '\\';
    memcpy(full_path + root_length + 1, data.name, name_length + 1);
    if (prefix_length) {
      memcpy(relative_path, prefix, prefix_length);
      relative_path[prefix_length] = '/';
      memcpy(relative_path + prefix_length + 1, data.name, name_length + 1);
    } else {
      memcpy(relative_path, data.name, name_length + 1);
    }
    if (data.attributes & MT_FILE_ATTRIBUTE_DIRECTORY) {
      mt_scan_md_files(full_path, relative_path, paths, capacity, used, count,
                       limit);
    } else if (mt_is_markdown_name(data.name)) {
      mt_append_md_path(paths, capacity, used, count, limit, relative_path);
    }
  } while (*count < limit && FindNextFileA(handle, &data));
  FindClose(handle);
}

int mettle_dir_list_md_files(const char *root, char *paths, mt_i32 capacity,
                             mt_i32 limit) {
  mt_i32 used = 0;
  mt_i32 count = 0;
  if (!root || !paths || capacity <= 1 || limit <= 0 ||
      !mettle_dir_exists(root)) {
    return 0;
  }
  paths[0] = 0;
  mt_scan_md_files(root, "", paths, capacity, &used, &count, limit);
  return count;
}

mt_i64 mettle_readlink(const char *path, char *buffer, mt_u64 size) {
  (void)path;
  (void)buffer;
  (void)size;
  mt_errno_value = 38;
  return -1;
}

mt_u32 mettle_thread_current_id(void) { return GetCurrentThreadId(); }

int mettle_atomic_compare_exchange_i32(volatile int *target, int exchange,
                                       int comparand) {
  __atomic_compare_exchange_n(target, &comparand, exchange, 0,
                              __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  return comparand;
}

int mettle_atomic_exchange_i32(volatile int *target, int value) {
  return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);
}

int mettle_atomic_inc_i32(volatile int *target) {
  return __atomic_add_fetch(target, 1, __ATOMIC_SEQ_CST);
}

int mettle_atomic_dec_i32(volatile int *target) {
  return __atomic_sub_fetch(target, 1, __ATOMIC_SEQ_CST);
}

static void *mt_windows_std_handle(MtFile *file) {
  mt_u32 which = MT_STD_INPUT_HANDLE;
  if (file == &mt_stdout_file) {
    which = MT_STD_OUTPUT_HANDLE;
  } else if (file == &mt_stderr_file) {
    which = MT_STD_ERROR_HANDLE;
  }
  return GetStdHandle(which);
}

#define MT_WIN_SUBCHUNK 65536u
#define MT_WIN_COMMIT_GRANULE (1024u * 1024u)
#define MT_WIN_ARENA_BYTES (1024u * 1024u * 1024u)
#define MT_WIN_SUBCHUNK_COUNT (MT_WIN_ARENA_BYTES / MT_WIN_SUBCHUNK)
#define MT_WIN_CLASS_COUNT 11
#define MT_WIN_LARGE_MIN 16384u
#define MT_WIN_CLASS_BYTES(c) ((mt_size)16 << (c))

static unsigned char mt_win_subchunk_class[MT_WIN_SUBCHUNK_COUNT];
static char *mt_win_arena_base;
static char *mt_win_arena_end;
static char *mt_win_subchunk_next;
static char *mt_win_committed_end;
static void *mt_win_free_list[MT_WIN_CLASS_COUNT];
static char *mt_win_bump[MT_WIN_CLASS_COUNT];
static mt_size mt_win_bump_left[MT_WIN_CLASS_COUNT];
static volatile int mt_win_heap_lock;
static int mt_win_arena_unavailable;

static int mt_win_class_of(mt_size size) {
  if (size <= 16) {
    return 0;
  }
  return 60 - __builtin_clzll((mt_u64)size - 1);
}

__attribute__((noinline)) static void mt_win_heap_lock_contended(void) {
  while (__atomic_exchange_n(&mt_win_heap_lock, 1, __ATOMIC_ACQUIRE)) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
  }
}

static void mt_win_heap_acquire(void) {
  if (__atomic_exchange_n(&mt_win_heap_lock, 1, __ATOMIC_ACQUIRE)) {
    mt_win_heap_lock_contended();
  }
}

static void mt_win_heap_release(void) {
  __atomic_store_n(&mt_win_heap_lock, 0, __ATOMIC_RELEASE);
}

__attribute__((noinline)) static char *mt_win_take_subchunk(mt_size count) {
  mt_size span = count * MT_WIN_SUBCHUNK;
  char *chunk;

  if (!mt_win_arena_base) {
    char *base;

    if (mt_win_arena_unavailable) {
      return MT_NULL;
    }
    base = (char *)VirtualAlloc(MT_NULL, MT_WIN_ARENA_BYTES, MT_MEM_RESERVE,
                                MT_PAGE_READWRITE);
    if (!base) {
      mt_win_arena_unavailable = 1;
      return MT_NULL;
    }
    mt_win_arena_base = base;
    mt_win_arena_end = base + MT_WIN_ARENA_BYTES;
    mt_win_subchunk_next = base;
    mt_win_committed_end = base;
  }

  while (mt_win_subchunk_next + span > mt_win_committed_end) {
    mt_size want = span > MT_WIN_COMMIT_GRANULE ? span : MT_WIN_COMMIT_GRANULE;

    if (mt_win_committed_end + want > mt_win_arena_end) {
      want = (mt_size)(mt_win_arena_end - mt_win_committed_end);
    }
    if (want < span ||
        !VirtualAlloc(mt_win_committed_end, want, MT_MEM_COMMIT,
                      MT_PAGE_READWRITE)) {
      return MT_NULL;
    }
    mt_win_committed_end += want;
  }

  chunk = mt_win_subchunk_next;
  mt_win_subchunk_next += span;
  return chunk;
}

static void *mt_win_carve(int class_index) {
  mt_size block = MT_WIN_CLASS_BYTES(class_index);
  char *result;

  if (mt_win_bump_left[class_index] < block) {
    mt_size count = (block * 16 + MT_WIN_SUBCHUNK - 1) / MT_WIN_SUBCHUNK;
    mt_size index;
    char *chunk;

    if (count == 0) {
      count = 1;
    }
    chunk = mt_win_take_subchunk(count);
    if (!chunk) {
      return MT_NULL;
    }
    index = (mt_size)(chunk - mt_win_arena_base) / MT_WIN_SUBCHUNK;
    for (mt_size i = 0; i < count; i++) {
      mt_win_subchunk_class[index + i] = (unsigned char)class_index;
    }
    mt_win_bump[class_index] = chunk;
    mt_win_bump_left[class_index] = count * MT_WIN_SUBCHUNK;
  }

  result = mt_win_bump[class_index];
  mt_win_bump[class_index] = result + block;
  mt_win_bump_left[class_index] -= block;
  return result;
}

static int mt_win_owns(const void *memory) {
  return (const char *)memory >= mt_win_arena_base &&
         (const char *)memory < mt_win_arena_end;
}

static int mt_win_class_at(const void *memory) {
  return (int)mt_win_subchunk_class[(mt_size)((const char *)memory -
                                              mt_win_arena_base) /
                                    MT_WIN_SUBCHUNK];
}

void *malloc(mt_size size) {
  void *memory;
  int class_index;

  if (size == 0) {
    size = 1;
  }
  if (size > MT_WIN_LARGE_MIN) {
    memory = HeapAlloc(GetProcessHeap(), 0, size);
    if (memory) {
      mt_allocation_count++;
    }
    return memory;
  }

  class_index = mt_win_class_of(size);
  mt_win_heap_acquire();
  memory = mt_win_free_list[class_index];
  if (memory) {
    mt_win_free_list[class_index] = *(void **)memory;
  } else {
    memory = mt_win_carve(class_index);
  }
  if (memory) {
    mt_allocation_count++;
  }
  mt_win_heap_release();
  if (!memory) {
    memory = HeapAlloc(GetProcessHeap(), 0, size);
    if (memory) {
      mt_allocation_count++;
    }
  }
  return memory;
}

void *calloc(mt_size count, mt_size size) {
  if (size && count > MT_SIZE_MAX / size) {
    return MT_NULL;
  }
  mt_size total = count * size;
  if (total == 0) {
    total = 1;
  }
  if (total > MT_WIN_LARGE_MIN) {
    void *memory = HeapAlloc(GetProcessHeap(), MT_HEAP_ZERO_MEMORY, total);
    if (memory) {
      mt_allocation_count++;
    }
    return memory;
  }

  void *memory = malloc(total);
  if (memory) {
    memset(memory, 0, total);
  }
  return memory;
}

void free(void *memory) {
  if (!memory) {
    return;
  }
  if (mt_win_owns(memory)) {
    int class_index = mt_win_class_at(memory);

    mt_win_heap_acquire();
    *(void **)memory = mt_win_free_list[class_index];
    mt_win_free_list[class_index] = memory;
    mt_free_count++;
    mt_win_heap_release();
    return;
  }
  mt_free_count++;
  HeapFree(GetProcessHeap(), 0, memory);
}

void *realloc(void *memory, mt_size size) {
  if (!memory) {
    return malloc(size);
  }
  if (size == 0) {
    free(memory);
    return MT_NULL;
  }
  if (mt_win_owns(memory)) {
    mt_size held = MT_WIN_CLASS_BYTES(mt_win_class_at(memory));
    void *replacement;

    if (size <= held) {
      return memory;
    }
    replacement = malloc(size);
    if (!replacement) {
      return MT_NULL;
    }
    memcpy(replacement, memory, held);
    free(memory);
    return replacement;
  }
  return HeapReAlloc(GetProcessHeap(), 0, memory, size);
}

typedef struct MtEmutlsControl {
  mt_size size;
  mt_size alignment;
  mt_size index;
  void *template_value;
} MtEmutlsControl;

typedef struct MtEmutlsNode {
  MtEmutlsControl *control;
  void *allocation;
  void *value;
  struct MtEmutlsNode *next;
} MtEmutlsNode;

static mt_u32 mt_emutls_fls_index = 0xffffffffu;
static volatile int mt_emutls_fls_lock;

static void mt_emutls_thread_destroy(void *value) {
  MtEmutlsNode *node = (MtEmutlsNode *)value;
  while (node) {
    MtEmutlsNode *next = node->next;
    free(node->allocation);
    free(node);
    node = next;
  }
}

static mt_u32 mt_emutls_index(void) {
  if (mt_emutls_fls_index != 0xffffffffu) return mt_emutls_fls_index;
  while (__atomic_exchange_n(&mt_emutls_fls_lock, 1, __ATOMIC_ACQUIRE)) {
  }
  if (mt_emutls_fls_index == 0xffffffffu) {
    mt_emutls_fls_index = FlsAlloc(mt_emutls_thread_destroy);
  }
  __atomic_store_n(&mt_emutls_fls_lock, 0, __ATOMIC_RELEASE);
  return mt_emutls_fls_index;
}

void *__emutls_get_address(MtEmutlsControl *control) {
  mt_u32 index = mt_emutls_index();
  MtEmutlsNode *head;
  if (!control || index == 0xffffffffu) return MT_NULL;
  head = (MtEmutlsNode *)FlsGetValue(index);
  for (MtEmutlsNode *node = head; node; node = node->next) {
    if (node->control == control) return node->value;
  }

  mt_size alignment = control->alignment ? control->alignment : sizeof(void *);
  mt_size size = control->size ? control->size : 1;
  if (alignment < sizeof(void *)) alignment = sizeof(void *);
  if ((alignment & (alignment - 1)) != 0 || size > MT_SIZE_MAX - alignment) {
    return MT_NULL;
  }
  MtEmutlsNode *node = (MtEmutlsNode *)malloc(sizeof(MtEmutlsNode));
  void *allocation = malloc(size + alignment - 1);
  if (!node || !allocation) {
    free(node);
    free(allocation);
    return MT_NULL;
  }
  mt_u64 address = ((mt_u64)allocation + alignment - 1) & ~(alignment - 1);
  node->control = control;
  node->allocation = allocation;
  node->value = (void *)address;
  node->next = head;
  if (control->template_value) {
    memcpy(node->value, control->template_value, size);
  } else {
    memset(node->value, 0, size);
  }
  if (!FlsSetValue(index, node)) {
    free(allocation);
    free(node);
    return MT_NULL;
  }
  return node->value;
}

void __emutls_register_common(MtEmutlsControl *control, mt_size size,
                              mt_size alignment, void *template_value) {
  if (!control) return;
  if (control->size < size) control->size = size;
  if (control->alignment < alignment) control->alignment = alignment;
  if (!control->template_value) control->template_value = template_value;
}

static mt_ssize mt_file_read(MtFile *file, void *buffer, mt_size count) {
  void *handle = file->flags & MT_FILE_STANDARD
                     ? mt_windows_std_handle(file)
                     : (void *)(mt_i64)file->handle;
  mt_size total = 0;
  while (total < count) {
    mt_size left = count - total;
    mt_u32 part = left > 0x7fffffffu ? 0x7fffffffu : (mt_u32)left;
    mt_u32 done = 0;
    if (!ReadFile(handle, (mt_u8 *)buffer + total, part, &done, MT_NULL)) {
      return total ? (mt_ssize)total : -1;
    }
    total += done;
    if (done != part) {
      break;
    }
  }
  return (mt_ssize)total;
}

static mt_ssize mt_file_write(MtFile *file, const void *buffer, mt_size count) {
  void *handle = file->flags & MT_FILE_STANDARD
                     ? mt_windows_std_handle(file)
                     : (void *)(mt_i64)file->handle;
  mt_size total = 0;
  while (total < count) {
    mt_size left = count - total;
    mt_u32 part = left > 0x7fffffffu ? 0x7fffffffu : (mt_u32)left;
    mt_u32 done = 0;
    if (!WriteFile(handle, (const mt_u8 *)buffer + total, part, &done,
                   MT_NULL)) {
      return total ? (mt_ssize)total : -1;
    }
    total += done;
    if (done != part) {
      break;
    }
  }
  return (mt_ssize)total;
}

static mt_i64 mt_file_seek(MtFile *file, mt_i64 offset, int origin) {
  mt_i64 position = -1;
  void *handle = (file->flags & MT_FILE_STANDARD) ? mt_windows_std_handle(file)
                                                  : (void *)file->handle;
  if (!SetFilePointerEx(handle, offset, &position, (mt_u32)origin)) {
    return -1;
  }
  return position;
}

void *__acrt_iob_func(int index) {
  if (index == 0) {
    return &mt_stdin_file;
  }
  if (index == 2) {
    return &mt_stderr_file;
  }
  return &mt_stdout_file;
}

static mt_size mt_utf8_to_utf16(const char *input, mt_u16 *output,
                                mt_size capacity) {
  mt_size written = 0;
  mt_size i = 0;
  while (input[i]) {
    mt_u32 code = (mt_u8)input[i++];
    mt_size extra = 0;
    if (code >= 0xF0u) {
      code &= 0x07u;
      extra = 3;
    } else if (code >= 0xE0u) {
      code &= 0x0Fu;
      extra = 2;
    } else if (code >= 0xC0u) {
      code &= 0x1Fu;
      extra = 1;
    } else if (code >= 0x80u) {
      code = 0xFFFDu;
    }
    while (extra > 0) {
      mt_u8 next = (mt_u8)input[i];
      if ((next & 0xC0u) != 0x80u) {
        code = 0xFFFDu;
        break;
      }
      code = (code << 6) | (mt_u32)(next & 0x3Fu);
      i++;
      extra--;
    }
    if (code > 0x10FFFFu || (code >= 0xD800u && code <= 0xDFFFu)) {
      code = 0xFFFDu;
    }
    if (code >= 0x10000u) {
      if (output && written + 2 <= capacity) {
        output[written] = (mt_u16)(0xD800u + ((code - 0x10000u) >> 10));
        output[written + 1] = (mt_u16)(0xDC00u + ((code - 0x10000u) & 0x3FFu));
      }
      written += 2;
    } else {
      if (output && written < capacity) {
        output[written] = (mt_u16)code;
      }
      written += 1;
    }
  }
  if (output && written < capacity) {
    output[written] = 0;
  }
  return written;
}

static mt_size mt_utf16_to_utf8(const mt_u16 *input, mt_size length,
                                char *output) {
  mt_size written = 0;
  mt_size i = 0;
  while (i < length) {
    mt_u32 code = input[i++];
    if (code >= 0xD800u && code <= 0xDBFFu && i < length &&
        input[i] >= 0xDC00u && input[i] <= 0xDFFFu) {
      code = 0x10000u + ((code - 0xD800u) << 10) + (mt_u32)(input[i] - 0xDC00u);
      i++;
    }
    if (code < 0x80u) {
      if (output) {
        output[written] = (char)code;
      }
      written += 1;
    } else if (code < 0x800u) {
      if (output) {
        output[written] = (char)(0xC0u | (code >> 6));
        output[written + 1] = (char)(0x80u | (code & 0x3Fu));
      }
      written += 2;
    } else if (code < 0x10000u) {
      if (output) {
        output[written] = (char)(0xE0u | (code >> 12));
        output[written + 1] = (char)(0x80u | ((code >> 6) & 0x3Fu));
        output[written + 2] = (char)(0x80u | (code & 0x3Fu));
      }
      written += 3;
    } else {
      if (output) {
        output[written] = (char)(0xF0u | (code >> 18));
        output[written + 1] = (char)(0x80u | ((code >> 12) & 0x3Fu));
        output[written + 2] = (char)(0x80u | ((code >> 6) & 0x3Fu));
        output[written + 3] = (char)(0x80u | (code & 0x3Fu));
      }
      written += 4;
    }
  }
  return written;
}

static mt_u16 *mt_widen_path(const char *path) {
  mt_size units = mt_utf8_to_utf16(path, MT_NULL, 0);
  mt_u16 *wide = (mt_u16 *)malloc((units + 1) * sizeof(mt_u16));
  if (!wide) {
    return MT_NULL;
  }
  mt_utf8_to_utf16(path, wide, units + 1);
  wide[units] = 0;
  return wide;
}

void *fopen(const char *path, const char *mode) {
  mt_u32 access = MT_GENERIC_READ;
  mt_u32 creation = MT_OPEN_EXISTING;
  int append = 0;
  if (!path || !mode || !mode[0]) {
    return MT_NULL;
  }
  if (mode[0] == 'w') {
    access = MT_GENERIC_WRITE;
    creation = MT_CREATE_ALWAYS;
  } else if (mode[0] == 'a') {
    access = MT_GENERIC_WRITE;
    creation = MT_OPEN_ALWAYS;
    append = 1;
  }
  for (mt_size i = 1; mode[i]; i++) {
    if (mode[i] == '+') {
      access = MT_GENERIC_READ | MT_GENERIC_WRITE;
    }
  }
  mt_u16 *wide_path = mt_widen_path(path);
  if (!wide_path) {
    return MT_NULL;
  }
  void *handle = CreateFileW(wide_path, access, MT_FILE_SHARE_READ, MT_NULL,
                             creation, MT_FILE_ATTRIBUTE_NORMAL, MT_NULL);
  free(wide_path);
  if (handle == MT_INVALID_HANDLE) {
    return MT_NULL;
  }
  MtFile *file = (MtFile *)malloc(sizeof(MtFile));
  if (!file) {
    CloseHandle(handle);
    return MT_NULL;
  }
  file->handle = (mt_i64)handle;
  file->flags = (access & MT_GENERIC_READ ? MT_FILE_READ : 0u) |
                (access & MT_GENERIC_WRITE ? MT_FILE_WRITE : 0u);
  if (file->flags == MT_FILE_READ) {
    file->flags |= MT_FILE_BUFFERED;
  }
  if (file->flags & MT_FILE_WRITE) {
    file->flags |= MT_FILE_WRITE_BUFFERED;
  }
  file->child_pid = 0;
  file->read_buffer = MT_NULL;
  file->read_fill = 0;
  file->read_pos = 0;
  file->write_buffer = MT_NULL;
  file->write_fill = 0;
  file->next_open = MT_NULL;
  if (file->flags & MT_FILE_WRITE_BUFFERED) {
    mt_open_write_track(file);
  }
  if (append) {
    SetFilePointerEx(handle, 0, MT_NULL, MT_FILE_END);
  }
  return file;
}

int fclose(void *stream) {
  MtFile *file = (MtFile *)stream;
  if (!file || file == &mt_stdin_file || file == &mt_stdout_file ||
      file == &mt_stderr_file) {
    return file ? 0 : -1;
  }
  mt_open_write_forget(file);
  if (mt_stream_flush(file) != 0) {
    CloseHandle((void *)(mt_i64)file->handle);
    free(file->read_buffer);
    free(file->write_buffer);
    free(file);
    return -1;
  }
  int result = CloseHandle((void *)(mt_i64)file->handle) ? 0 : -1;
  free(file->read_buffer);
  free(file->write_buffer);
  free(file);
  return result;
}

int mettle_rt_getmainargs(int *argc_out, char ***argv_out) {
  mt_u16 *command = GetCommandLineW();
  mt_size length = 0;
  while (command[length]) {
    length++;
  }

  mt_u16 *text = (mt_u16 *)malloc((length + 1) * sizeof(mt_u16));
  mt_size *starts = (mt_size *)malloc((length + 2) * sizeof(mt_size));
  int argc = 0;
  mt_size source = 0;
  mt_size target = 0;
  if (!argc_out || !argv_out || !text || !starts) {
    free(text);
    free(starts);
    return 0;
  }

  while (source < length) {
    while (source < length &&
           (command[source] == ' ' || command[source] == '\t')) {
      source++;
    }
    if (source == length) {
      break;
    }
    starts[argc++] = target;
    int quoted = 0;
    while (source < length) {
      mt_size slash_count = 0;
      while (source < length && command[source] == '\\') {
        slash_count++;
        source++;
      }
      if (source < length && command[source] == '"') {
        for (mt_size i = 0; i < slash_count / 2; i++) {
          text[target++] = '\\';
        }
        if (slash_count & 1u) {
          text[target++] = '"';
          source++;
        } else if (quoted && source + 1 < length && command[source + 1] == '"') {
          text[target++] = '"';
          source += 2;
        } else {
          quoted = !quoted;
          source++;
        }
        continue;
      }
      while (slash_count--) {
        text[target++] = '\\';
      }
      if (source == length ||
          (!quoted && (command[source] == ' ' || command[source] == '\t'))) {
        break;
      }
      text[target++] = command[source++];
    }
    text[target++] = 0;
  }

  mt_size utf8_bytes = 0;
  for (int i = 0; i < argc; i++) {
    mt_size units = 0;
    while (text[starts[i] + units]) {
      units++;
    }
    utf8_bytes += mt_utf16_to_utf8(text + starts[i], units, MT_NULL) + 1;
  }

  char **argv = (char **)malloc(((mt_size)argc + 1) * sizeof(char *));
  char *bytes = (char *)malloc(utf8_bytes + 1);
  if (!argv || !bytes) {
    free(argv);
    free(bytes);
    free(text);
    free(starts);
    return 0;
  }

  mt_size written = 0;
  for (int i = 0; i < argc; i++) {
    mt_size units = 0;
    while (text[starts[i] + units]) {
      units++;
    }
    argv[i] = bytes + written;
    written += mt_utf16_to_utf8(text + starts[i], units, bytes + written);
    bytes[written++] = 0;
  }
  argv[argc] = MT_NULL;

  free(text);
  free(starts);
  *argc_out = argc;
  *argv_out = argv;
  return 1;
}

MT_NORETURN void exit(int status) {
  mt_flush_open_streams();
  ExitProcess((mt_u32)status);
}
MT_NORETURN void _exit(int status) { ExitProcess((mt_u32)status); }

static mt_size mt_windows_quoted_size(const char *text) {
  mt_size size = 3;
  mt_size slashes = 0;
  for (; *text; text++) {
    if (*text == '\\') {
      slashes++;
    } else if (*text == '"') {
      size += slashes * 2 + 2;
      slashes = 0;
    } else {
      size += slashes + 1;
      slashes = 0;
    }
  }
  return size + slashes * 2;
}

static char *mt_windows_quote(char *out, const char *text) {
  mt_size slashes = 0;
  *out++ = '"';
  for (; *text; text++) {
    if (*text == '\\') {
      slashes++;
      continue;
    }
    if (*text == '"') {
      for (mt_size i = 0; i < slashes * 2 + 1; i++) *out++ = '\\';
      *out++ = '"';
    } else {
      while (slashes) {
        *out++ = '\\';
        slashes--;
      }
      *out++ = *text;
    }
    slashes = 0;
  }
  for (mt_size i = 0; i < slashes * 2; i++) *out++ = '\\';
  *out++ = '"';
  return out;
}

int mettle_find_executable(const char *program) {
  char resolved[32768];
  const char *extension;
  mt_u32 length;
  if (!program || !program[0]) return 0;
  extension = strchr(program, '.') ? MT_NULL : ".exe";
  length = SearchPathA(MT_NULL, program, extension, sizeof(resolved), resolved,
                       MT_NULL);
  return length > 0 && length < sizeof(resolved);
}

int mettle_run_process(const char *program, const char *const *arguments) {
  mt_size command_size = 1;
  mt_size count = 0;
  char resolved_program[32768];
  const char *application = program;
  while (arguments[count]) {
    command_size += mt_windows_quoted_size(arguments[count]) + 1;
    count++;
  }
  char *command = (char *)malloc(command_size);
  if (!command) return 127;
  char *out = command;
  for (mt_size i = 0; i < count; i++) {
    if (i) *out++ = ' ';
    out = mt_windows_quote(out, arguments[i]);
  }
  *out = 0;

  MtStartupInfo startup;
  MtProcessInfo process;
  memset(&startup, 0, sizeof(startup));
  memset(&process, 0, sizeof(process));
  startup.size = sizeof(startup);
  {
    const char *extension = strchr(program, '.') ? MT_NULL : ".exe";
    mt_u32 length = SearchPathA(MT_NULL, program, extension,
                                sizeof(resolved_program), resolved_program,
                                MT_NULL);
    if (length > 0 && length < sizeof(resolved_program)) {
      application = resolved_program;
    }
  }
  int created = CreateProcessA(application, command, MT_NULL, MT_NULL, 0, 0,
                               MT_NULL, MT_NULL, &startup, &process);
  free(command);
  if (!created) return 127;
  (void)WaitForSingleObject(process.process, 0xffffffffu);
  mt_u32 status = 127;
  (void)GetExitCodeProcess(process.process, &status);
  CloseHandle(process.thread);
  CloseHandle(process.process);
  return (int)status;
}

/* CreateProcessA does not consult PATH for its application-name argument, so
 * the shell has to be resolved to a real path before the child is asked for.
 * COMSPEC first, then a PATH search, so an unusual but working environment is
 * honoured before the conventional one. */
static int mt_resolve_command_shell(char *buffer, mt_u32 size) {
  mt_u32 length = GetEnvironmentVariableA("COMSPEC", buffer, size);
  if (length > 0 && length < size) return 1;
  length = SearchPathA(MT_NULL, "cmd.exe", MT_NULL, size, buffer, MT_NULL);
  return length > 0 && length < size;
}

void *popen(const char *command, const char *mode) {
  void *read_pipe = MT_NULL;
  void *write_pipe = MT_NULL;
  MtSecurityAttributes attributes;
  MtStartupInfo startup;
  MtProcessInfo process;
  MtFile *file = MT_NULL;
  char *command_line = MT_NULL;
  char shell[512];
  const char prefix[] = "cmd.exe /S /C ";
  if (!command || !mode || mode[0] != 'r' || mode[1] != 0) return MT_NULL;
  if (!mt_resolve_command_shell(shell, sizeof(shell))) return MT_NULL;

  attributes.size = sizeof(attributes);
  attributes.security_descriptor = MT_NULL;
  attributes.inherit_handle = 1;
  if (!CreatePipe(&read_pipe, &write_pipe, &attributes, 0)) return MT_NULL;
  if (!SetHandleInformation(read_pipe, 1, 0)) {
    CloseHandle(read_pipe);
    CloseHandle(write_pipe);
    return MT_NULL;
  }

  mt_size command_size = sizeof(prefix) + strlen(command);
  command_line = (char *)malloc(command_size);
  file = (MtFile *)malloc(sizeof(MtFile));
  if (!command_line || !file) {
    free(command_line);
    free(file);
    CloseHandle(read_pipe);
    CloseHandle(write_pipe);
    return MT_NULL;
  }
  strcpy(command_line, prefix);
  strcat(command_line, command);
  memset(&startup, 0, sizeof(startup));
  memset(&process, 0, sizeof(process));
  startup.size = sizeof(startup);
  startup.flags = 0x00000100u;
  startup.input = GetStdHandle(MT_STD_INPUT_HANDLE);
  startup.output = write_pipe;
  startup.error = write_pipe;
  if (!CreateProcessA(shell, command_line, MT_NULL, MT_NULL, 1, 0,
                      MT_NULL, MT_NULL, &startup, &process)) {
    free(command_line);
    free(file);
    CloseHandle(read_pipe);
    CloseHandle(write_pipe);
    return MT_NULL;
  }
  free(command_line);
  CloseHandle(write_pipe);
  CloseHandle(process.thread);
  file->handle = (mt_i64)read_pipe;
  file->flags = MT_FILE_READ;
  file->child_pid = (mt_i64)process.process;
  file->read_buffer = MT_NULL;
  file->read_fill = 0;
  file->read_pos = 0;
  file->write_buffer = MT_NULL;
  file->write_fill = 0;
  file->next_open = MT_NULL;
  return file;
}

int pclose(void *stream) {
  MtFile *file = (MtFile *)stream;
  mt_u32 status = 127;
  void *process;
  if (!file || file->child_pid == 0) return -1;
  process = (void *)(mt_i64)file->child_pid;
  CloseHandle((void *)(mt_i64)file->handle);
  (void)WaitForSingleObject(process, 0xffffffffu);
  (void)GetExitCodeProcess(process, &status);
  CloseHandle(process);
  free(file->read_buffer);
  free(file);
  return (int)status;
}

#else

static char **mt_environment;
static char mt_environment_value[32768];
static char *mt_environment_items[512];
static char *mt_environment_overrides[64];
static mt_size mt_environment_override_count;
static char *mt_read_environment_value(const char *name, mt_size name_length);
static int mt_initialize_initial_tls(mt_i64 argc, char **argv);
#if defined(MTLC_HOST_PREFIX_H)
static void mt_raise_stack_limit(void);
#endif

void mettle_rt_startup(mt_i64 argc, char **argv) {
  if (!mt_initialize_initial_tls(argc, argv)) {
    _exit(127);
  }
  mt_environment = argv + argc + 1;
#if defined(MTLC_HOST_PREFIX_H)
  mt_raise_stack_limit();
#endif
}

char *getenv(const char *name) {
  mt_size name_length = strlen(name);
  for (mt_size i = 0; i < mt_environment_override_count; i++) {
    char *item = mt_environment_overrides[i];
    if (strncmp(item, name, name_length) == 0 && item[name_length] == '=') {
      return item + name_length + 1;
    }
  }
  if (mt_environment) {
    for (char **item = mt_environment; *item; item++) {
      if (strncmp(*item, name, name_length) == 0 &&
          (*item)[name_length] == '=') {
        return *item + name_length + 1;
      }
    }
    return MT_NULL;
  }

  return mt_read_environment_value(name, name_length);
}

#if defined(__x86_64__)
static mt_i64 mt_syscall6(mt_i64 number, mt_i64 a1, mt_i64 a2, mt_i64 a3,
                          mt_i64 a4, mt_i64 a5, mt_i64 a6) {
  register mt_i64 r10 __asm__("r10") = a4;
  register mt_i64 r8 __asm__("r8") = a5;
  register mt_i64 r9 __asm__("r9") = a6;
  mt_i64 result;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(number), "D"(a1), "S"(a2), "d"(a3), "r"(r10),
                     "r"(r8), "r"(r9)
                   : "rcx", "r11", "memory");
  return result;
}
#define MT_SYS_READ 0
#define MT_SYS_WRITE 1
#define MT_SYS_CLOSE 3
#define MT_SYS_LSEEK 8
#define MT_SYS_IOCTL 16
#define MT_SYS_MMAP 9
#define MT_SYS_PRLIMIT64 302
#define MT_SYS_MPROTECT 10
#define MT_SYS_MUNMAP 11
#define MT_SYS_SCHED_YIELD 24
#define MT_SYS_NANOSLEEP 35
#define MT_SYS_SOCKET 41
#define MT_SYS_CONNECT 42
#define MT_SYS_ACCEPT 43
#define MT_SYS_SENDTO 44
#define MT_SYS_RECVFROM 45
#define MT_SYS_SHUTDOWN 48
#define MT_SYS_BIND 49
#define MT_SYS_LISTEN 50
#define MT_SYS_SETSOCKOPT 54
#define MT_SYS_EXIT 60
#define MT_SYS_EXECVE 59
#define MT_SYS_WAIT4 61
#define MT_SYS_RT_SIGACTION 13
#define MT_SYS_GETCWD 79
#define MT_SYS_MKDIR 83
#define MT_SYS_CHMOD 90
#define MT_SYS_NEWFSTATAT 262
#define MT_SYS_GETDENTS64 217
#define MT_SYS_READLINKAT 267
#define MT_SYS_FACCESSAT 269
#define MT_SYS_DUP3 292
#define MT_SYS_PIPE2 293
#define MT_SYS_CLOCK_GETTIME 228
#define MT_SYS_OPENAT 257
#define MT_SYS_UNLINKAT 263
#define MT_SYS_CLONE 56
#define MT_SYS_GETTID 186
#define MT_SYS_FUTEX 202
#define MT_SYS_ARCH_PRCTL 158
#define MT_SYS_SIGALTSTACK 131
#elif defined(__aarch64__)
static mt_i64 mt_syscall6(mt_i64 number, mt_i64 a1, mt_i64 a2, mt_i64 a3,
                          mt_i64 a4, mt_i64 a5, mt_i64 a6) {
  register mt_i64 x0 __asm__("x0") = a1;
  register mt_i64 x1 __asm__("x1") = a2;
  register mt_i64 x2 __asm__("x2") = a3;
  register mt_i64 x3 __asm__("x3") = a4;
  register mt_i64 x4 __asm__("x4") = a5;
  register mt_i64 x5 __asm__("x5") = a6;
  register mt_i64 x8 __asm__("x8") = number;
  __asm__ volatile("svc 0"
                   : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                   : "memory");
  return x0;
}
#define MT_SYS_OPENAT 56
#define MT_SYS_UNLINKAT 35
#define MT_SYS_CLOSE 57
#define MT_SYS_LSEEK 62
#define MT_SYS_IOCTL 29
#define MT_SYS_READ 63
#define MT_SYS_WRITE 64
#define MT_SYS_EXIT 93
#define MT_SYS_EXECVE 221
#define MT_SYS_WAIT4 260
#define MT_SYS_RT_SIGACTION 134
#define MT_SYS_GETCWD 17
#define MT_SYS_DUP3 24
#define MT_SYS_MKDIRAT 34
#define MT_SYS_FCHMODAT 53
#define MT_SYS_FACCESSAT 48
#define MT_SYS_PIPE2 59
#define MT_SYS_READLINKAT 78
#define MT_SYS_NEWFSTATAT 79
#define MT_SYS_GETDENTS64 61
#define MT_SYS_NANOSLEEP 101
#define MT_SYS_CLOCK_GETTIME 113
#define MT_SYS_SCHED_YIELD 124
#define MT_SYS_SOCKET 198
#define MT_SYS_BIND 200
#define MT_SYS_LISTEN 201
#define MT_SYS_ACCEPT 202
#define MT_SYS_CONNECT 203
#define MT_SYS_SENDTO 206
#define MT_SYS_RECVFROM 207
#define MT_SYS_SETSOCKOPT 208
#define MT_SYS_SHUTDOWN 210
#define MT_SYS_MUNMAP 215
#define MT_SYS_MMAP 222
#define MT_SYS_PRLIMIT64 261
#define MT_SYS_MPROTECT 226
#define MT_SYS_CLONE 220
#define MT_SYS_GETTID 178
#define MT_SYS_FUTEX 98
#define MT_SYS_SIGALTSTACK 132
#else
#error The freestanding Mettle runtime needs a syscall table for this target
#endif

#define MT_AT_FDCWD -100
#define MT_O_RDONLY 0
#define MT_O_RDWR 2
#define MT_O_CREAT 0100
#define MT_O_TRUNC 01000
#define MT_O_APPEND 02000
#define MT_O_DIRECTORY 00200000
#define MT_FILE_MODE 0666
#define MT_PROT_READ 1
#define MT_PROT_WRITE 2
#define MT_MAP_PRIVATE 2
#define MT_MAP_ANONYMOUS 0x20

#if defined(MTLC_HOST_PREFIX_H)
#define MT_RLIMIT_STACK 3
#define MT_HOST_STACK_BYTES ((mt_u64)64 * 1024 * 1024)

typedef struct {
  mt_u64 cur;
  mt_u64 max;
} MtRlimit64;

static void mt_raise_stack_limit(void) {
  MtRlimit64 current;
  MtRlimit64 next;
  mt_u64 want = MT_HOST_STACK_BYTES;

  if (mt_syscall6(MT_SYS_PRLIMIT64, 0, MT_RLIMIT_STACK, 0, (mt_i64)&current, 0,
                  0) < 0) {
    return;
  }
  if (current.max != (mt_u64)-1 && want > current.max) {
    want = current.max;
  }
  if (want <= current.cur) {
    return;
  }
  next.cur = want;
  next.max = current.max;
  mt_syscall6(MT_SYS_PRLIMIT64, 0, MT_RLIMIT_STACK, (mt_i64)&next, 0, 0, 0);
}
#endif

static mt_i64 mt_sys_result(mt_i64 result) {
  if ((mt_u64)result >= (mt_u64)-4095) {
    mt_errno_value = (int)-result;
    return -1;
  }
  return result;
}

typedef struct MtElfProgramHeader {
  mt_u32 type;
  mt_u32 flags;
  mt_u64 offset;
  mt_u64 virtual_address;
  mt_u64 physical_address;
  mt_u64 file_size;
  mt_u64 memory_size;
  mt_u64 alignment;
} MtElfProgramHeader;

#define MT_AT_PHDR 3
#define MT_AT_PHENT 4
#define MT_AT_PHNUM 5
#define MT_PT_TLS 7

static void *mt_tls_template;
static mt_size mt_tls_file_size;
static mt_size mt_tls_memory_size;
static mt_size mt_tls_alignment;

static mt_u64 mt_align_up_u64(mt_u64 value, mt_u64 alignment) {
  if (alignment <= 1) return value;
  return (value + alignment - 1) & ~(alignment - 1);
}

static int mt_prepare_thread_pointer(void *mapping, mt_size mapping_size,
                                     void **thread_pointer_out) {
  mt_u64 base = (mt_u64)mapping;
  mt_u64 alignment = mt_tls_alignment > 16 ? mt_tls_alignment : 16;
#if defined(__x86_64__)
  mt_u8 *tls = (mt_u8 *)mt_align_up_u64(base, alignment);
  mt_u8 *thread_pointer = tls + mt_align_up_u64(mt_tls_memory_size, alignment);
  if ((mt_u64)(thread_pointer + 16) > base + mapping_size) return 0;
  if (mt_tls_file_size) memcpy(tls, mt_tls_template, mt_tls_file_size);
  if (mt_tls_memory_size > mt_tls_file_size) {
    memset(tls + mt_tls_file_size, 0,
           mt_tls_memory_size - mt_tls_file_size);
  }
  *(void **)thread_pointer = thread_pointer;
  *thread_pointer_out = thread_pointer;
  return 1;
#elif defined(__aarch64__)
  mt_u8 *tls = (mt_u8 *)mt_align_up_u64(base + 16, alignment);
  mt_u8 *thread_pointer = tls - 16;
  if ((mt_u64)(tls + mt_tls_memory_size) > base + mapping_size) return 0;
  if (mt_tls_file_size) memcpy(tls, mt_tls_template, mt_tls_file_size);
  if (mt_tls_memory_size > mt_tls_file_size) {
    memset(tls + mt_tls_file_size, 0,
           mt_tls_memory_size - mt_tls_file_size);
  }
  *(void **)thread_pointer = thread_pointer;
  *thread_pointer_out = thread_pointer;
  return 1;
#endif
}

static int mt_install_thread_pointer(void *mapping, mt_size mapping_size) {
  void *thread_pointer = MT_NULL;
  if (!mt_prepare_thread_pointer(mapping, mapping_size, &thread_pointer)) {
    return 0;
  }
#if defined(__x86_64__)
  return mt_syscall6(MT_SYS_ARCH_PRCTL, 0x1002, (mt_i64)thread_pointer,
                     0, 0, 0, 0) == 0;
#elif defined(__aarch64__)
  __asm__ volatile("msr tpidr_el0, %0" : : "r"(thread_pointer) : "memory");
  return 1;
#endif
}

static int mt_thread_pointer_installed(void) {
#if defined(__x86_64__)
  mt_u64 base = 0;
  if (mt_syscall6(MT_SYS_ARCH_PRCTL, 0x1003, (mt_i64)(mt_u64)&base, 0, 0, 0,
                  0) != 0) {
    return 0;
  }
  return base != 0;
#elif defined(__aarch64__)
  mt_u64 base = 0;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(base));
  return base != 0;
#else
  return 0;
#endif
}

static int mt_initialize_initial_tls(mt_i64 argc, char **argv) {
  char **environment = argv + argc + 1;
  while (*environment) environment++;
  mt_u64 *auxiliary = (mt_u64 *)(environment + 1);
  MtElfProgramHeader *headers = MT_NULL;
  mt_u64 header_size = 0;
  mt_u64 header_count = 0;

  while (auxiliary[0]) {
    if (auxiliary[0] == MT_AT_PHDR) {
      headers = (MtElfProgramHeader *)auxiliary[1];
    } else if (auxiliary[0] == MT_AT_PHENT) {
      header_size = auxiliary[1];
    } else if (auxiliary[0] == MT_AT_PHNUM) {
      header_count = auxiliary[1];
    }
    auxiliary += 2;
  }
  if (!headers || header_size < sizeof(MtElfProgramHeader)) return 1;

  for (mt_u64 i = 0; i < header_count; i++) {
    MtElfProgramHeader *header = (MtElfProgramHeader *)
        ((mt_u8 *)headers + i * header_size);
    if (header->type != MT_PT_TLS) continue;
    mt_tls_template = (void *)header->virtual_address;
    mt_tls_file_size = (mt_size)header->file_size;
    mt_tls_memory_size = (mt_size)header->memory_size;
    mt_tls_alignment = (mt_size)(header->alignment ? header->alignment : 1);
    if (mt_tls_memory_size == 0) return 1;
    if (mt_thread_pointer_installed()) return 1;

    mt_size alignment = mt_tls_alignment > 16 ? mt_tls_alignment : 16;
    mt_size mapping_size = mt_tls_memory_size + alignment * 2 + 16;
    mt_i64 result = mt_syscall6(MT_SYS_MMAP, 0, mapping_size,
                                MT_PROT_READ | MT_PROT_WRITE,
                                MT_MAP_PRIVATE | MT_MAP_ANONYMOUS, -1, 0);
    if ((mt_u64)result >= (mt_u64)-4095) return 0;
    return mt_install_thread_pointer((void *)result, mapping_size);
  }
  return 1;
}

typedef struct MtKernelSigaction {
  void (*handler)(int, void *, void *);
  mt_u64 flags;
  void (*restorer)(void);
  mt_u64 mask;
} MtKernelSigaction;

typedef struct MtKernelStack {
  void *pointer;
  int flags;
  int padding;
  mt_size size;
} MtKernelStack;

static char mt_signal_stack[64 * 1024];
static int mt_signal_stack_installed;

#if defined(__x86_64__)
__asm__(".text\n"
        ".p2align 4\n"
        ".globl mt_signal_restorer\n"
        "mt_signal_restorer:\n"
        "movq $15, %rax\n"
        "syscall\n");
extern void mt_signal_restorer(void);
#elif defined(__aarch64__)
__asm__(".text\n"
        ".p2align 4\n"
        ".globl mt_signal_restorer\n"
        "mt_signal_restorer:\n"
        "mov x8, #139\n"
        "svc #0\n");
extern void mt_signal_restorer(void);
#endif

int mettle_install_signal_handler(
    int signal_number, void (*handler)(int, void *, void *)) {
  if (!mt_signal_stack_installed) {
    MtKernelStack stack;
    stack.pointer = mt_signal_stack;
    stack.flags = 0;
    stack.padding = 0;
    stack.size = sizeof(mt_signal_stack);
    if (mt_sys_result(mt_syscall6(MT_SYS_SIGALTSTACK, (mt_i64)&stack, 0,
                                  0, 0, 0, 0)) == 0) {
      mt_signal_stack_installed = 1;
    }
  }
  MtKernelSigaction action;
  memset(&action, 0, sizeof(action));
  action.handler = handler;
  action.flags = 4;
  if (mt_signal_stack_installed) action.flags |= 0x08000000u;
#if defined(__x86_64__)
  action.flags |= 0x04000000u;
  action.restorer = mt_signal_restorer;
#endif
  return (int)mt_sys_result(mt_syscall6(MT_SYS_RT_SIGACTION, signal_number,
                                        (mt_i64)&action, 0, 8, 0, 0));
}

int mettle_address_is_readable(const void *address, mt_u64 length) {
  int descriptors[2] = {-1, -1};
  char sink[64];
  if (!address || length == 0) return 0;
  if (mt_sys_result(mt_syscall6(MT_SYS_PIPE2, (mt_i64)descriptors, 0, 0,
                                0, 0, 0)) < 0) {
    return 0;
  }
  mt_i64 written = mt_syscall6(MT_SYS_WRITE, descriptors[1],
                                (mt_i64)address, length, 0, 0, 0);
  if (written > 0) {
    mt_i64 remaining = written;
    while (remaining > 0) {
      mt_i64 amount = remaining < (mt_i64)sizeof(sink)
                          ? remaining
                          : (mt_i64)sizeof(sink);
      mt_i64 got = mt_syscall6(MT_SYS_READ, descriptors[0],
                               (mt_i64)sink, amount, 0, 0, 0);
      if (got <= 0) break;
      remaining -= got;
    }
  }
  (void)mt_syscall6(MT_SYS_CLOSE, descriptors[0], 0, 0, 0, 0, 0);
  (void)mt_syscall6(MT_SYS_CLOSE, descriptors[1], 0, 0, 0, 0, 0);
  return written == (mt_i64)length;
}

mt_ssize write(int fd, const void *buffer, mt_size count) {
  return mt_sys_result(
      mt_syscall6(MT_SYS_WRITE, fd, (mt_i64)buffer, count, 0, 0, 0));
}

mt_ssize read(int fd, void *buffer, mt_size count) {
  return mt_sys_result(
      mt_syscall6(MT_SYS_READ, fd, (mt_i64)buffer, count, 0, 0, 0));
}

static char *mt_read_environment_value(const char *name, mt_size name_length) {
  mt_i64 fd = mt_syscall6(MT_SYS_OPENAT, MT_AT_FDCWD,
                           (mt_i64)"/proc/self/environ", MT_O_RDONLY, 0, 0, 0);
  if (fd < 0) {
    return MT_NULL;
  }
  mt_i64 count = mt_syscall6(MT_SYS_READ, fd, (mt_i64)mt_environment_value,
                             sizeof(mt_environment_value) - 1, 0, 0, 0);
  (void)mt_syscall6(MT_SYS_CLOSE, fd, 0, 0, 0, 0, 0);
  if (count <= 0) {
    return MT_NULL;
  }
  mt_environment_value[count] = 0;
  char *item = mt_environment_value;
  char *limit = mt_environment_value + count;
  mt_size item_count = 0;
  char *found = MT_NULL;
  while (item < limit && *item) {
    mt_size item_length = strlen(item);
    if (item_count + 1 < sizeof(mt_environment_items) /
                             sizeof(mt_environment_items[0])) {
      mt_environment_items[item_count++] = item;
    }
    if (item_length > name_length && item[name_length] == '=' &&
        strncmp(item, name, name_length) == 0) {
      found = item + name_length + 1;
    }
    item += item_length + 1;
  }
  mt_environment_items[item_count] = MT_NULL;
  mt_environment = mt_environment_items;
  return found;
}

int putenv(char *setting) {
  char *equals = setting ? strchr(setting, '=') : MT_NULL;
  if (!equals || equals == setting) {
    mt_errno_value = 22;
    return -1;
  }
  mt_size name_length = (mt_size)(equals - setting);
  for (mt_size i = 0; i < mt_environment_override_count; i++) {
    if (strncmp(mt_environment_overrides[i], setting, name_length) == 0 &&
        mt_environment_overrides[i][name_length] == '=') {
      mt_environment_overrides[i] = setting;
      return 0;
    }
  }
  if (mt_environment_override_count >= sizeof(mt_environment_overrides) /
                                           sizeof(mt_environment_overrides[0])) {
    mt_errno_value = 12;
    return -1;
  }
  mt_environment_overrides[mt_environment_override_count++] = setting;
  return 0;
}

int close(int fd) {
  return (int)mt_sys_result(mt_syscall6(MT_SYS_CLOSE, fd, 0, 0, 0, 0, 0));
}

int access(const char *path, int mode) {
  return (int)mt_sys_result(mt_syscall6(MT_SYS_FACCESSAT, MT_AT_FDCWD,
                                        (mt_i64)path, mode, 0, 0, 0));
}

int mettle_path_exists(const char *path) { return access(path, 0) == 0; }

int mettle_path_is_directory(const char *path) {
  mt_i64 fd = mt_syscall6(MT_SYS_OPENAT, MT_AT_FDCWD, (mt_i64)path,
                           MT_O_RDONLY | MT_O_DIRECTORY, 0, 0, 0);
  if (fd < 0) return 0;
  (void)mt_syscall6(MT_SYS_CLOSE, fd, 0, 0, 0, 0, 0);
  return 1;
}

static char *mt_getcwd_impl(char *buffer, mt_size size) {
  int allocated = 0;
  if (!buffer) {
    size = size ? size : 4096;
    buffer = (char *)malloc(size);
    if (!buffer) return MT_NULL;
    allocated = 1;
  }
  mt_i64 result = mt_sys_result(
      mt_syscall6(MT_SYS_GETCWD, (mt_i64)buffer, size, 0, 0, 0, 0));
  if (result < 0) {
    if (allocated) free(buffer);
    return MT_NULL;
  }
  return buffer;
}

#ifdef getcwd
char *getcwd(char *buffer, mt_size size) {
  return mt_getcwd_impl(buffer, size);
}
#endif

int mettle_getcwd(char *buffer, mt_i32 size) {
  return buffer && size > 0 && mt_getcwd_impl(buffer, (mt_size)size) ? 0 : -1;
}

int mkdir(const char *path, unsigned int mode) {
#if defined(__x86_64__)
  return (int)mt_sys_result(
      mt_syscall6(MT_SYS_MKDIR, (mt_i64)path, mode, 0, 0, 0, 0));
#else
  return (int)mt_sys_result(mt_syscall6(MT_SYS_MKDIRAT, MT_AT_FDCWD,
                                        (mt_i64)path, mode, 0, 0, 0));
#endif
}

int chmod(const char *path, unsigned int mode) {
#if defined(__x86_64__)
  return (int)mt_sys_result(
      mt_syscall6(MT_SYS_CHMOD, (mt_i64)path, mode, 0, 0, 0, 0));
#else
  return (int)mt_sys_result(mt_syscall6(MT_SYS_FCHMODAT, MT_AT_FDCWD,
                                        (mt_i64)path, mode, 0, 0, 0));
#endif
}

int mettle_make_directory(const char *path) { return mkdir(path, 0777); }

int mettle_dir_exists(const char *path) {
  return mettle_path_is_directory(path);
}

int mettle_dir_create(const char *path) { return mkdir(path, 0755); }

int mettle_file_exists(const char *path) {
  return path && access(path, 0) == 0 && !mettle_path_is_directory(path);
}

typedef struct MtLinuxDirent64 {
  mt_u64 inode;
  mt_i64 next_offset;
  unsigned short record_length;
  mt_u8 type;
  char name[];
} MtLinuxDirent64;

static void mt_scan_md_files(const char *root, const char *prefix, char *paths,
                             mt_i32 capacity, mt_i32 *used, mt_i32 *count,
                             mt_i32 limit) {
  char entries[8192];
  char full_path[4096];
  char relative_path[4096];
  mt_size root_length;
  mt_i64 directory;
  if (!root || !paths || !used || !count || *count >= limit) {
    return;
  }
  directory = mt_sys_result(mt_syscall6(
      MT_SYS_OPENAT, MT_AT_FDCWD, (mt_i64)root,
      MT_O_RDONLY | MT_O_DIRECTORY, 0, 0, 0));
  if (directory < 0) {
    return;
  }
  root_length = strlen(root);
  for (;;) {
    mt_i64 bytes = mt_sys_result(mt_syscall6(
        MT_SYS_GETDENTS64, directory, (mt_i64)entries, sizeof(entries), 0, 0,
        0));
    if (bytes <= 0) {
      break;
    }
    mt_i64 offset = 0;
    while (offset < bytes && *count < limit) {
      MtLinuxDirent64 *entry = (MtLinuxDirent64 *)(entries + offset);
      mt_size name_length;
      mt_size prefix_length;
      int is_directory;
      if (entry->record_length < 20 || offset + entry->record_length > bytes) {
        offset = bytes;
        break;
      }
      offset += entry->record_length;
      if (strcmp(entry->name, ".") == 0 || strcmp(entry->name, "..") == 0) {
        continue;
      }
      name_length = strlen(entry->name);
      prefix_length = prefix ? strlen(prefix) : 0;
      if (root_length + name_length + 2 > sizeof(full_path) ||
          prefix_length + name_length + (prefix_length ? 2 : 1) >
              sizeof(relative_path)) {
        continue;
      }
      memcpy(full_path, root, root_length);
      full_path[root_length] = '/';
      memcpy(full_path + root_length + 1, entry->name, name_length + 1);
      if (prefix_length) {
        memcpy(relative_path, prefix, prefix_length);
        relative_path[prefix_length] = '/';
        memcpy(relative_path + prefix_length + 1, entry->name,
               name_length + 1);
      } else {
        memcpy(relative_path, entry->name, name_length + 1);
      }
      is_directory = entry->type == 4 ||
                     (entry->type == 0 && mettle_path_is_directory(full_path));
      if (is_directory) {
        mt_scan_md_files(full_path, relative_path, paths, capacity, used, count,
                         limit);
      } else if ((entry->type == 8 || entry->type == 0) &&
                 mt_is_markdown_name(entry->name)) {
        mt_append_md_path(paths, capacity, used, count, limit, relative_path);
      }
    }
  }
  mt_syscall6(MT_SYS_CLOSE, directory, 0, 0, 0, 0, 0);
}

int mettle_dir_list_md_files(const char *root, char *paths, mt_i32 capacity,
                             mt_i32 limit) {
  mt_i32 used = 0;
  mt_i32 count = 0;
  if (!root || !paths || capacity <= 1 || limit <= 0 ||
      !mettle_dir_exists(root)) {
    return 0;
  }
  paths[0] = 0;
  mt_scan_md_files(root, "", paths, capacity, &used, &count, limit);
  return count;
}

int gettimeofday(void *time_value, void *timezone_value) {
  mt_i64 timespec_value[2] = {0, 0};
  (void)timezone_value;
  if (mt_sys_result(mt_syscall6(MT_SYS_CLOCK_GETTIME, 0,
                                (mt_i64)timespec_value, 0, 0, 0, 0)) < 0) {
    return -1;
  }
  ((mt_i64 *)time_value)[0] = timespec_value[0];
  ((mt_i64 *)time_value)[1] = timespec_value[1] / 1000;
  return 0;
}

mt_ssize readlink(const char *path, char *buffer, mt_size size) {
  return mt_sys_result(mt_syscall6(MT_SYS_READLINKAT, MT_AT_FDCWD,
                                    (mt_i64)path, (mt_i64)buffer, size, 0, 0));
}

mt_i64 mettle_readlink(const char *path, char *buffer, mt_u64 size) {
  return (mt_i64)readlink(path, buffer, (mt_size)size);
}

mt_i64 mettle_executable_path(char *buffer, mt_u64 size) {
  return mettle_readlink("/proc/self/exe", buffer, size);
}

int stat(const char *path, void *status) {
  return (int)mt_sys_result(mt_syscall6(MT_SYS_NEWFSTATAT, MT_AT_FDCWD,
                                        (mt_i64)path, (mt_i64)status, 0, 0, 0));
}

int unlink(const char *path) {
  return (int)mt_sys_result(mt_syscall6(MT_SYS_UNLINKAT, MT_AT_FDCWD,
                                        (mt_i64)path, 0, 0, 0, 0));
}

char *realpath(const char *path, char *resolved) {
  char full[4096];
  mt_size used = 0;
  if (!path || access(path, 0) != 0) {
    return MT_NULL;
  }
  if (*path == '/') {
    full[used++] = '/';
    path++;
  } else {
    if (!mt_getcwd_impl(full, sizeof(full))) {
      return MT_NULL;
    }
    used = strlen(full);
    if (used == 0 || full[used - 1] != '/') {
      full[used++] = '/';
    }
  }
  while (*path) {
    while (*path == '/') path++;
    const char *component = path;
    while (*path && *path != '/') path++;
    mt_size length = (mt_size)(path - component);
    if (length == 0 || (length == 1 && component[0] == '.')) {
      continue;
    }
    if (length == 2 && component[0] == '.' && component[1] == '.') {
      if (used > 1 && full[used - 1] == '/') used--;
      while (used > 1 && full[used - 1] != '/') used--;
      continue;
    }
    if (used > 1 && full[used - 1] != '/') full[used++] = '/';
    if (used + length + 1 >= sizeof(full)) {
      mt_errno_value = 36;
      return MT_NULL;
    }
    memcpy(full + used, component, length);
    used += length;
  }
  if (used > 1 && full[used - 1] == '/') used--;
  full[used] = 0;
  if (!resolved) {
    resolved = (char *)malloc(used + 1);
    if (!resolved) return MT_NULL;
  }
  memcpy(resolved, full, used + 1);
  return resolved;
}


char *mettle_realpath(const char *path, char *resolved) {
  return realpath(path, resolved);
}

void *mmap(void *address, mt_size length, int protection, int flags, int fd,
           mt_i64 offset) {
  mt_i64 result = mt_syscall6(MT_SYS_MMAP, (mt_i64)address, length, protection,
                              flags, fd, offset);
  if ((mt_u64)result >= (mt_u64)-4095) {
    mt_errno_value = (int)-result;
    return (void *)-1;
  }
  return (void *)result;
}

int munmap(void *address, mt_size length) {
  return (int)mt_sys_result(
      mt_syscall6(MT_SYS_MUNMAP, (mt_i64)address, length, 0, 0, 0, 0));
}

int mprotect(void *address, mt_size length, int protection) {
  return (int)mt_sys_result(mt_syscall6(MT_SYS_MPROTECT, (mt_i64)address,
                                        length, protection, 0, 0, 0));
}

/* Size-classed heap over a few large mappings.
 *
 * Every allocation used to be its own mmap and every free its own munmap. That
 * is one syscall each way, a page fault on first touch, and a whole 4 KiB page
 * for a four-byte string. A compile that allocates three hundred thousand
 * times therefore spent most of its wall clock in the kernel: on this repo's
 * own 1500-line sample the Linux build took 1.5 s where the Windows build,
 * which reaches HeapAlloc, took 0.55 s.
 *
 * The header stays 16 bytes so the payload keeps its 16-byte alignment, and
 * base[0] still distinguishes the two shapes: a small block stores its class
 * plus one (1..MT_HEAP_CLASS_COUNT), a large one stores its mapping size,
 * which is always at least a page. Small blocks come off a per-class free list
 * or a bump pointer into the current chunk, and go back to the free list on
 * free; the chunk itself is never unmapped, which is what makes the common
 * allocate/free/allocate cycle syscall-free. Allocations above
 * MT_HEAP_LARGE_MIN keep the old one-mapping-each behaviour, so a program that
 * allocates and releases big buffers still returns that memory to the kernel.
 *
 * The classes are the powers of two from 16 to 16384, so a class index is one
 * shift away from a size and back, with no table to carry. A finer ladder would
 * waste less memory per block, but this file is linked into every Mettle binary
 * and the arithmetic to walk a finer ladder costs more bytes there than the
 * fragmentation is worth.
 *
 * Losing the one-mapping-per-allocation layout also loses the guard page that
 * made a heap overrun fault immediately. MTLC_POISON_FREED_OPERANDS and the
 * memory diagnostics cover that ground deliberately instead. */
#define MT_HEAP_HEADER 16
#define MT_HEAP_CLASS_COUNT 11
#define MT_HEAP_LARGE_MIN 16384
#define MT_HEAP_CHUNK_MIN (256u * 1024u)
#define MT_HEAP_CHUNK_MAX (8u * 1024u * 1024u)
#define MT_HEAP_CLASS_BYTES(c) ((mt_size)16 << (c))

static void *mt_heap_free_list[MT_HEAP_CLASS_COUNT];
static char *mt_heap_bump;
static mt_size mt_heap_bump_left;
static mt_size mt_heap_chunk_size;
static volatile int mt_heap_lock;

static int mt_heap_class_of(mt_size size) {
  if (size <= 16) {
    return 0;
  }
  return 60 - __builtin_clzll((mt_u64)size - 1);
}

/* Out of line and cold: uncontended acquire is one exchange. */
__attribute__((noinline)) static void mt_heap_lock_contended(void) {
  int spins = 0;
  while (__atomic_exchange_n(&mt_heap_lock, 1, __ATOMIC_ACQUIRE)) {
    if (++spins > 64) {
      mt_syscall6(MT_SYS_SCHED_YIELD, 0, 0, 0, 0, 0, 0);
      spins = 0;
    }
  }
}

static void mt_heap_acquire(void) {
  if (__atomic_exchange_n(&mt_heap_lock, 1, __ATOMIC_ACQUIRE)) {
    mt_heap_lock_contended();
  }
}

static void mt_heap_release(void) {
  __atomic_store_n(&mt_heap_lock, 0, __ATOMIC_RELEASE);
}

/* Out of line: this is the only place the mmap syscall sequence is written, and
 * both the large-allocation path and the chunk refill reach it. */
__attribute__((noinline)) static void *mt_heap_map(mt_size bytes) {
  void *mapping = mmap(MT_NULL, bytes, MT_PROT_READ | MT_PROT_WRITE,
                       MT_MAP_PRIVATE | MT_MAP_ANONYMOUS, -1, 0);
  return mapping == (void *)-1 ? MT_NULL : mapping;
}

/* Cold: once per chunk, which is once per few thousand allocations. Caller
 * holds the heap lock. */
__attribute__((noinline)) static int mt_heap_refill(void) {
  mt_size want = mt_heap_chunk_size ? mt_heap_chunk_size * 2
                                    : (mt_size)MT_HEAP_CHUNK_MIN;
  char *chunk;
  if (want > MT_HEAP_CHUNK_MAX) {
    want = MT_HEAP_CHUNK_MAX;
  }
  chunk = (char *)mt_heap_map(want);
  if (!chunk) {
    return 0;
  }
  /* The tail of the old chunk is abandoned rather than tracked: it is at most
   * one block, against a chunk of a quarter megabyte or more. */
  mt_heap_bump = chunk;
  mt_heap_bump_left = want;
  mt_heap_chunk_size = want;
  return 1;
}

/* One block off the size-class lists, header included. Out of line so malloc
 * stays a handful of instructions on the path that hits a free list. */
__attribute__((noinline)) static mt_u64 *mt_heap_take(int class_index) {
  mt_u64 *base;
  mt_size block_bytes = MT_HEAP_CLASS_BYTES(class_index) + MT_HEAP_HEADER;

  mt_heap_acquire();
  base = (mt_u64 *)mt_heap_free_list[class_index];
  if (base) {
    mt_heap_free_list[class_index] = *(void **)(base + 2);
  } else if (mt_heap_bump_left >= block_bytes || mt_heap_refill()) {
    base = (mt_u64 *)mt_heap_bump;
    mt_heap_bump += block_bytes;
    mt_heap_bump_left -= block_bytes;
  }
  mt_heap_release();
  return base;
}

void *malloc(mt_size size) {
  mt_u64 *base;
  mt_u64 tag;

  if (size == 0) {
    size = 1;
  }
  if (size > MT_HEAP_LARGE_MIN) {
    if (size > MT_SIZE_MAX - MT_HEAP_HEADER) {
      return MT_NULL;
    }
    tag = (mt_u64)size + MT_HEAP_HEADER;
    base = (mt_u64 *)mt_heap_map((mt_size)tag);
  } else {
    int class_index = mt_heap_class_of(size);
    tag = (mt_u64)(class_index + 1);
    base = mt_heap_take(class_index);
  }
  if (!base) {
    return MT_NULL;
  }
  base[0] = tag;
  base[1] = size;
  mt_allocation_count++;
  return base + 2;
}

void *calloc(mt_size count, mt_size size) {
  if (size && count > MT_SIZE_MAX / size) {
    return MT_NULL;
  }
  mt_size total = count * size;
  void *memory = malloc(total);
  /* A recycled block carries whatever the last owner left in it, so unlike the
   * one-mapping-each heap this cannot lean on mmap handing back zeroed pages. */
  if (memory && total) {
    memset(memory, 0, total);
  }
  return memory;
}

void free(void *memory) {
  if (!memory) {
    return;
  }
  mt_u64 *base = (mt_u64 *)memory - 2;
  mt_free_count++;
  if (base[0] > MT_HEAP_CLASS_COUNT) {
    munmap(base, base[0]);
    return;
  }
  mt_heap_acquire();
  *(void **)memory = mt_heap_free_list[base[0] - 1];
  mt_heap_free_list[base[0] - 1] = base;
  mt_heap_release();
}

void *realloc(void *memory, mt_size size) {
  if (!memory) {
    return malloc(size);
  }
  if (size == 0) {
    free(memory);
    return MT_NULL;
  }
  mt_u64 *base = (mt_u64 *)memory - 2;
  mt_size old_size = base[1];
  /* Growing inside the block it already occupies is the common case behind
   * every doubling array in the compiler, and it costs nothing. */
  if (base[0] <= MT_HEAP_CLASS_COUNT &&
      size <= MT_HEAP_CLASS_BYTES(base[0] - 1)) {
    base[1] = size;
    return memory;
  }
  void *replacement = malloc(size);
  if (!replacement) {
    return MT_NULL;
  }
  memcpy(replacement, memory, old_size < size ? old_size : size);
  free(memory);
  return replacement;
}

static mt_ssize mt_file_read(MtFile *file, void *buffer, mt_size count) {
  return read((int)file->handle, buffer, count);
}

static mt_ssize mt_file_write(MtFile *file, const void *buffer, mt_size count) {
  return write((int)file->handle, buffer, count);
}

static mt_i64 mt_file_seek(MtFile *file, mt_i64 offset, int origin) {
  return mt_sys_result(mt_syscall6(MT_SYS_LSEEK, file->handle, offset, origin,
                                   0, 0, 0));
}

void *fopen(const char *path, const char *mode) {
  int flags = MT_O_RDONLY;
  if (!path || !mode || !mode[0]) {
    return MT_NULL;
  }
  if (mode[0] == 'w') {
    flags = MT_O_CREAT | MT_O_TRUNC | MT_O_RDWR;
  } else if (mode[0] == 'a') {
    flags = MT_O_CREAT | MT_O_APPEND | MT_O_RDWR;
  }
  for (mt_size i = 1; mode[i]; i++) {
    if (mode[i] == '+') {
      flags = (flags & ~MT_O_RDONLY) | MT_O_RDWR;
    }
  }
  int fd = (int)mt_sys_result(mt_syscall6(MT_SYS_OPENAT, MT_AT_FDCWD,
                                           (mt_i64)path, flags, MT_FILE_MODE,
                                           0, 0));
  if (fd < 0) {
    return MT_NULL;
  }
  MtFile *file = (MtFile *)malloc(sizeof(MtFile));
  if (!file) {
    close(fd);
    return MT_NULL;
  }
  file->handle = fd;
  file->flags = flags == MT_O_RDONLY
                    ? (MT_FILE_READ | MT_FILE_BUFFERED)
                    : (MT_FILE_READ | MT_FILE_WRITE | MT_FILE_WRITE_BUFFERED);
  file->child_pid = 0;
  file->read_buffer = MT_NULL;
  file->read_fill = 0;
  file->read_pos = 0;
  file->write_buffer = MT_NULL;
  file->write_fill = 0;
  file->next_open = MT_NULL;
  if (file->flags & MT_FILE_WRITE_BUFFERED) {
    mt_open_write_track(file);
  }
  return file;
}

int fclose(void *stream) {
  MtFile *file = (MtFile *)stream;
  if (!file || file == &mt_stdin_file || file == &mt_stdout_file ||
      file == &mt_stderr_file) {
    return file ? 0 : -1;
  }
  mt_open_write_forget(file);
  int flushed = mt_stream_flush(file);
  int result = close((int)file->handle);
  free(file->read_buffer);
  free(file->write_buffer);
  free(file);
  return flushed != 0 ? -1 : result;
}

int posix_get_errno(void) { return mt_errno_value; }

int socket(int domain, int type, int protocol) {
  return (int)mt_sys_result(
      mt_syscall6(MT_SYS_SOCKET, domain, type, protocol, 0, 0, 0));
}

int connect(int fd, const void *address, int length) {
  return (int)mt_sys_result(mt_syscall6(MT_SYS_CONNECT, fd, (mt_i64)address,
                                        length, 0, 0, 0));
}

int bind(int fd, const void *address, int length) {
  return (int)mt_sys_result(
      mt_syscall6(MT_SYS_BIND, fd, (mt_i64)address, length, 0, 0, 0));
}

int listen(int fd, int backlog) {
  return (int)mt_sys_result(
      mt_syscall6(MT_SYS_LISTEN, fd, backlog, 0, 0, 0, 0));
}

int accept(int fd, void *address, void *length) {
  return (int)mt_sys_result(mt_syscall6(MT_SYS_ACCEPT, fd, (mt_i64)address,
                                        (mt_i64)length, 0, 0, 0));
}

int setsockopt(int fd, int level, int option, const void *value, int length) {
  return (int)mt_sys_result(mt_syscall6(MT_SYS_SETSOCKOPT, fd, level, option,
                                        (mt_i64)value, length, 0));
}

mt_ssize send(int fd, const void *buffer, mt_size length, int flags) {
  return mt_sys_result(mt_syscall6(MT_SYS_SENDTO, fd, (mt_i64)buffer, length,
                                   flags, 0, 0));
}

mt_ssize recv(int fd, void *buffer, mt_size length, int flags) {
  return mt_sys_result(mt_syscall6(MT_SYS_RECVFROM, fd, (mt_i64)buffer, length,
                                   flags, 0, 0));
}

int shutdown(int fd, int how) {
  return (int)mt_sys_result(
      mt_syscall6(MT_SYS_SHUTDOWN, fd, how, 0, 0, 0, 0));
}

int clock_gettime(int clock_id, void *timespec_value) {
  return (int)mt_sys_result(mt_syscall6(MT_SYS_CLOCK_GETTIME, clock_id,
                                        (mt_i64)timespec_value, 0, 0, 0, 0));
}

int nanosleep(const void *request, void *remaining) {
  return (int)mt_sys_result(mt_syscall6(MT_SYS_NANOSLEEP, (mt_i64)request,
                                        (mt_i64)remaining, 0, 0, 0, 0));
}

int usleep(mt_u32 microseconds) {
  mt_i64 request[2];
  request[0] = microseconds / 1000000u;
  request[1] = (microseconds % 1000000u) * 1000u;
  return nanosleep(request, MT_NULL);
}

void posix_yield(void) {
  mt_sys_result(mt_syscall6(MT_SYS_SCHED_YIELD, 0, 0, 0, 0, 0, 0));
}

#define MT_CLONE_FLAGS 0x003d0f00
#define MT_FUTEX_WAIT 0
#define MT_FUTEX_WAKE 1
#define MT_WAIT_OBJECT_0 0u
#define MT_WAIT_TIMEOUT 258u
#define MT_WAIT_FAILED 0xffffffffu
#define MT_INFINITE 0xffffffffu
#define MT_THREAD_STACK_SIZE (1024u * 1024u)

typedef mt_u32 (*MtThreadStart)(void *);
typedef mt_u64 (*MtPthreadStart)(void *);

typedef struct MtThread {
  volatile int tid;
  int detached;
  void *stack;
  void *tls_mapping;
  mt_size tls_mapping_size;
  void *thread_pointer;
  MtThreadStart start;
  MtPthreadStart pthread_start;
  void *argument;
  mt_u64 result;
} MtThread;

typedef struct MtMutex {
  volatile int state;
} MtMutex;

static int mt_futex(volatile int *address, int operation, int value,
                    const void *timeout) {
  return (int)mt_sys_result(mt_syscall6(MT_SYS_FUTEX, (mt_i64)address,
                                        operation, value, (mt_i64)timeout, 0,
                                        0));
}

static void mt_thread_child(MtThread *thread) __attribute__((noreturn, used));

static void mt_thread_child(MtThread *thread) {
  thread->result = thread->pthread_start
                       ? thread->pthread_start(thread->argument)
                       : (mt_u64)thread->start(thread->argument);
  mt_syscall6(MT_SYS_EXIT, 0, 0, 0, 0, 0, 0);
  for (;;) {
  }
}

#if defined(__x86_64__)
__attribute__((naked)) static mt_i64 mt_clone_start(void *stack_top,
                                                     mt_i64 flags,
                                                     MtThread *thread,
                                                     void *thread_pointer) {
  __asm__("sub $16, %rdi\n\t"
          "mov %rdx, (%rdi)\n\t"
          "mov %rcx, %r8\n\t"
          "mov %rdi, %r11\n\t"
          "mov %rsi, %rdi\n\t"
          "mov %r11, %rsi\n\t"
          "lea 0(%rdx), %r10\n\t"
          "mov %r10, %rdx\n\t"
          "mov $56, %eax\n\t"
          "syscall\n\t"
          "test %rax, %rax\n\t"
          "jnz 1f\n\t"
          "mov (%rsp), %rdi\n\t"
          "call mt_thread_child\n\t"
          "1: ret\n\t");
}
#else
__attribute__((naked)) static mt_i64 mt_clone_start(void *stack_top,
                                                     mt_i64 flags,
                                                     MtThread *thread,
                                                     void *thread_pointer) {
  __asm__("sub x5, x0, #16\n\t"
          "str x2, [x5]\n\t"
          "mov x0, x1\n\t"
          "mov x1, x5\n\t"
          "mov x4, x2\n\t"
          "mov x2, x4\n\t"
          "mov x8, #220\n\t"
          "svc #0\n\t"
          "cbnz x0, 1f\n\t"
          "ldr x0, [sp]\n\t"
          "bl mt_thread_child\n\t"
          "1: ret\n\t");
}
#endif

mt_i64 mettle_thread_create(void *attributes, mt_u64 stack_size,
                            MtThreadStart start, void *argument,
                            mt_u32 creation_flags, mt_u32 *thread_id) {
  (void)attributes;
  (void)creation_flags;
  if (!start) {
    return 0;
  }
  mt_size size = stack_size ? (mt_size)stack_size : MT_THREAD_STACK_SIZE;
  if (size < 65536) {
    size = 65536;
  }
  MtThread *thread = (MtThread *)calloc(1, sizeof(MtThread));
  void *stack = malloc(size);
  if (!thread || !stack) {
    free(thread);
    free(stack);
    return 0;
  }
  thread->stack = stack;
  thread->start = start;
  thread->argument = argument;
  if (mt_tls_memory_size) {
    mt_size alignment = mt_tls_alignment > 16 ? mt_tls_alignment : 16;
    thread->tls_mapping_size =
        mt_tls_memory_size + alignment * 2 + 16;
    thread->tls_mapping = malloc(thread->tls_mapping_size);
    if (!thread->tls_mapping ||
        !mt_prepare_thread_pointer(thread->tls_mapping,
                                   thread->tls_mapping_size,
                                   &thread->thread_pointer)) {
      free(thread->tls_mapping);
      free(stack);
      free(thread);
      return 0;
    }
  }
  mt_i64 tid = mt_clone_start((mt_u8 *)stack + size, MT_CLONE_FLAGS, thread,
                              thread->thread_pointer);
  if (tid < 0) {
    free(thread->tls_mapping);
    free(stack);
    free(thread);
    return 0;
  }
  if (thread_id) {
    *thread_id = (mt_u32)tid;
  }
  return (mt_i64)thread;
}

static mt_i64 mt_pthread_create(MtPthreadStart start, void *argument) {
  if (!start) {
    return 0;
  }
  MtThread *thread = (MtThread *)calloc(1, sizeof(MtThread));
  void *stack = malloc(MT_THREAD_STACK_SIZE);
  if (!thread || !stack) {
    free(thread);
    free(stack);
    return 0;
  }
  thread->stack = stack;
  thread->pthread_start = start;
  thread->argument = argument;
  if (mt_tls_memory_size) {
    mt_size alignment = mt_tls_alignment > 16 ? mt_tls_alignment : 16;
    thread->tls_mapping_size = mt_tls_memory_size + alignment * 2 + 16;
    thread->tls_mapping = malloc(thread->tls_mapping_size);
    if (!thread->tls_mapping ||
        !mt_prepare_thread_pointer(thread->tls_mapping,
                                   thread->tls_mapping_size,
                                   &thread->thread_pointer)) {
      free(thread->tls_mapping);
      free(stack);
      free(thread);
      return 0;
    }
  }
  mt_i64 tid = mt_clone_start((mt_u8 *)stack + MT_THREAD_STACK_SIZE,
                              MT_CLONE_FLAGS, thread,
                              thread->thread_pointer);
  if (tid < 0) {
    free(thread->tls_mapping);
    free(stack);
    free(thread);
    return 0;
  }
  return (mt_i64)thread;
}

mt_u32 mettle_thread_wait(mt_i64 handle, mt_u32 milliseconds) {
  MtThread *thread = (MtThread *)handle;
  if (!thread) {
    return MT_WAIT_FAILED;
  }
  mt_i64 timeout_value[2];
  const void *timeout = MT_NULL;
  if (milliseconds != MT_INFINITE) {
    timeout_value[0] = milliseconds / 1000u;
    timeout_value[1] = (milliseconds % 1000u) * 1000000u;
    timeout = timeout_value;
  }
  for (;;) {
    int tid = __atomic_load_n(&thread->tid, __ATOMIC_ACQUIRE);
    if (tid == 0) {
      return MT_WAIT_OBJECT_0;
    }
    int result = mt_futex(&thread->tid, MT_FUTEX_WAIT, tid, timeout);
    if (result < 0 && mt_errno_value == 110) {
      return MT_WAIT_TIMEOUT;
    }
    if (result < 0 && mt_errno_value != 4 && mt_errno_value != 11) {
      return MT_WAIT_FAILED;
    }
  }
}

int mettle_thread_close(mt_i64 handle) {
  MtThread *thread = (MtThread *)handle;
  if (!thread) {
    return 0;
  }
  if (__atomic_load_n(&thread->tid, __ATOMIC_ACQUIRE) != 0) {
    thread->detached = 1;
    return 1;
  }
  free(thread->stack);
  free(thread->tls_mapping);
  free(thread);
  return 1;
}

mt_u32 mettle_thread_current_id(void) {
  return (mt_u32)mt_sys_result(
      mt_syscall6(MT_SYS_GETTID, 0, 0, 0, 0, 0, 0));
}

void mettle_thread_sleep_ms(mt_u32 milliseconds) {
  mt_i64 request[2];
  request[0] = milliseconds / 1000u;
  request[1] = (milliseconds % 1000u) * 1000000u;
  nanosleep(request, MT_NULL);
}

mt_i64 mettle_mutex_create(void *attributes, int initial_owner,
                           const char *name) {
  (void)attributes;
  (void)name;
  MtMutex *mutex = (MtMutex *)calloc(1, sizeof(MtMutex));
  if (mutex && initial_owner) {
    mutex->state = 1;
  }
  return (mt_i64)mutex;
}

mt_u32 mettle_mutex_wait(mt_i64 handle, mt_u32 milliseconds) {
  MtMutex *mutex = (MtMutex *)handle;
  if (!mutex) {
    return MT_WAIT_FAILED;
  }
  mt_i64 timeout_value[2];
  const void *timeout = MT_NULL;
  if (milliseconds != MT_INFINITE) {
    timeout_value[0] = milliseconds / 1000u;
    timeout_value[1] = (milliseconds % 1000u) * 1000000u;
    timeout = timeout_value;
  }
  for (;;) {
    int expected = 0;
    if (__atomic_compare_exchange_n(&mutex->state, &expected, 1, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      return MT_WAIT_OBJECT_0;
    }
    int result = mt_futex(&mutex->state, MT_FUTEX_WAIT, 1, timeout);
    if (result < 0 && mt_errno_value == 110) {
      return MT_WAIT_TIMEOUT;
    }
    if (result < 0 && mt_errno_value != 4 && mt_errno_value != 11) {
      return MT_WAIT_FAILED;
    }
  }
}

int mettle_mutex_release(mt_i64 handle) {
  MtMutex *mutex = (MtMutex *)handle;
  if (!mutex) {
    return 0;
  }
  __atomic_store_n(&mutex->state, 0, __ATOMIC_RELEASE);
  mt_futex(&mutex->state, MT_FUTEX_WAKE, 1, MT_NULL);
  return 1;
}

int mettle_mutex_close(mt_i64 handle) {
  MtMutex *mutex = (MtMutex *)handle;
  if (!mutex || __atomic_load_n(&mutex->state, __ATOMIC_ACQUIRE) != 0) {
    return 0;
  }
  free(mutex);
  return 1;
}

/* Source compatible POSIX thread names backed only by clone and futex. These
 * are ABI shims for std/thread_posix, not calls into a host thread library. */
int pthread_create(mt_i64 *thread_out, const void *attributes,
                   MtPthreadStart start, void *argument) {
  (void)attributes;
  if (!thread_out) {
    return 22;
  }
  mt_i64 thread = mt_pthread_create(start, argument);
  if (!thread) {
    return 11;
  }
  *thread_out = thread;
  return 0;
}

int pthread_join(mt_i64 handle, void **result_out) {
  MtThread *thread = (MtThread *)handle;
  if (!thread || mettle_thread_wait(handle, MT_INFINITE) != MT_WAIT_OBJECT_0) {
    return 22;
  }
  if (result_out) {
    *result_out = (void *)(mt_size)thread->result;
  }
  return mettle_thread_close(handle) ? 0 : 22;
}

int pthread_detach(mt_i64 handle) {
  return mettle_thread_close(handle) ? 0 : 22;
}

mt_i64 pthread_self(void) { return (mt_i64)mettle_thread_current_id(); }

MT_NORETURN void pthread_exit(void *result) {
  (void)result;
  mt_syscall6(MT_SYS_EXIT, 0, 0, 0, 0, 0, 0);
  for (;;) {
  }
}

int pthread_mutex_init(MtMutex *mutex, const void *attributes) {
  (void)attributes;
  if (!mutex) {
    return 22;
  }
  __atomic_store_n(&mutex->state, 0, __ATOMIC_RELAXED);
  return 0;
}

int pthread_mutex_destroy(MtMutex *mutex) {
  return mutex && __atomic_load_n(&mutex->state, __ATOMIC_ACQUIRE) == 0 ? 0
                                                                       : 16;
}

int pthread_mutex_trylock(MtMutex *mutex) {
  int expected = 0;
  if (!mutex) {
    return 22;
  }
  return __atomic_compare_exchange_n(&mutex->state, &expected, 1, 0,
                                     __ATOMIC_ACQUIRE,
                                     __ATOMIC_RELAXED)
             ? 0
             : 16;
}

int pthread_mutex_lock(MtMutex *mutex) {
  if (!mutex) {
    return 22;
  }
  for (;;) {
    int expected = 0;
    if (__atomic_compare_exchange_n(&mutex->state, &expected, 1, 0,
                                    __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED)) {
      return 0;
    }
    int result = mt_futex(&mutex->state, MT_FUTEX_WAIT, 1, MT_NULL);
    if (result < 0 && mt_errno_value != 4 && mt_errno_value != 11) {
      return mt_errno_value;
    }
  }
}

int pthread_mutex_unlock(MtMutex *mutex) {
  if (!mutex || __atomic_exchange_n(&mutex->state, 0, __ATOMIC_RELEASE) == 0) {
    return 22;
  }
  mt_futex(&mutex->state, MT_FUTEX_WAKE, 1, MT_NULL);
  return 0;
}

typedef struct MtCondition {
  volatile int sequence;
} MtCondition;

int pthread_cond_init(MtCondition *condition, const void *attributes) {
  (void)attributes;
  if (!condition) {
    return 22;
  }
  __atomic_store_n(&condition->sequence, 0, __ATOMIC_RELAXED);
  return 0;
}

int pthread_cond_destroy(MtCondition *condition) {
  return condition ? 0 : 22;
}

int pthread_cond_wait(MtCondition *condition, MtMutex *mutex) {
  if (!condition || !mutex) {
    return 22;
  }
  int sequence = __atomic_load_n(&condition->sequence, __ATOMIC_ACQUIRE);
  int result = pthread_mutex_unlock(mutex);
  if (result != 0) {
    return result;
  }
  int wait_error = 0;
  for (;;) {
    result = mt_futex(&condition->sequence, MT_FUTEX_WAIT, sequence, MT_NULL);
    if (result >= 0 || mt_errno_value == 11) {
      break;
    }
    if (mt_errno_value != 4) {
      wait_error = mt_errno_value;
      break;
    }
  }
  int lock_result = pthread_mutex_lock(mutex);
  return wait_error ? wait_error : lock_result;
}

int pthread_cond_signal(MtCondition *condition) {
  if (!condition) {
    return 22;
  }
  __atomic_add_fetch(&condition->sequence, 1, __ATOMIC_RELEASE);
  mt_futex(&condition->sequence, MT_FUTEX_WAKE, 1, MT_NULL);
  return 0;
}

int pthread_cond_broadcast(MtCondition *condition) {
  if (!condition) {
    return 22;
  }
  __atomic_add_fetch(&condition->sequence, 1, __ATOMIC_RELEASE);
  mt_futex(&condition->sequence, MT_FUTEX_WAKE, 0x7fffffff, MT_NULL);
  return 0;
}

int mettle_atomic_compare_exchange_i32(volatile int *target, int exchange,
                                       int comparand) {
  __atomic_compare_exchange_n(target, &comparand, exchange, 0,
                              __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  return comparand;
}

int mettle_atomic_exchange_i32(volatile int *target, int value) {
  return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);
}

int mettle_atomic_inc_i32(volatile int *target) {
  return __atomic_add_fetch(target, 1, __ATOMIC_SEQ_CST);
}

int mettle_atomic_dec_i32(volatile int *target) {
  return __atomic_sub_fetch(target, 1, __ATOMIC_SEQ_CST);
}

int posix_cas_i32(volatile int *target, int expected, int desired) {
  return __atomic_compare_exchange_n(target, &expected, desired, 0,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

int posix_atomic_exchange_i32(volatile int *target, int value) {
  return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);
}

int posix_atomic_add_i32(volatile int *target, int value) {
  return __atomic_fetch_add(target, value, __ATOMIC_SEQ_CST);
}

MT_NORETURN void exit(int status) {
  mt_flush_open_streams();
  mt_syscall6(MT_SYS_EXIT, status, 0, 0, 0, 0, 0);
  for (;;) {
  }
}

MT_NORETURN void _exit(int status) { exit(status); }

static void mt_exec_search(const char *program, const char *const *arguments) {
  char *const *argv = (char *const *)arguments;
  if (strchr(program, '/')) {
    (void)mt_syscall6(MT_SYS_EXECVE, (mt_i64)program, (mt_i64)argv,
                      (mt_i64)mt_environment, 0, 0, 0);
    return;
  }
  const char *path = getenv("PATH");
  if (!path) path = "/usr/local/bin:/usr/bin:/bin";
  while (*path) {
    const char *end = path;
    while (*end && *end != ':') end++;
    mt_size directory_length = (mt_size)(end - path);
    mt_size program_length = strlen(program);
    char candidate[1024];
    if (directory_length + program_length + 2 < sizeof(candidate)) {
      memcpy(candidate, path, directory_length);
      candidate[directory_length] = '/';
      memcpy(candidate + directory_length + 1, program, program_length + 1);
      (void)mt_syscall6(MT_SYS_EXECVE, (mt_i64)candidate, (mt_i64)argv,
                        (mt_i64)mt_environment, 0, 0, 0);
    }
    path = *end ? end + 1 : end;
  }
}

int mettle_find_executable(const char *program) {
  const char *path;
  if (!program || !program[0]) return 0;
  if (strchr(program, '/')) return access(program, 0) == 0;
  path = getenv("PATH");
  if (!path) path = "/usr/local/bin:/usr/bin:/bin";
  while (*path) {
    const char *end = path;
    mt_size directory_length;
    mt_size program_length;
    char candidate[1024];
    while (*end && *end != ':') end++;
    directory_length = (mt_size)(end - path);
    program_length = strlen(program);
    if (directory_length + program_length + 2 < sizeof(candidate)) {
      memcpy(candidate, path, directory_length);
      candidate[directory_length] = '/';
      memcpy(candidate + directory_length + 1, program, program_length + 1);
      if (access(candidate, 0) == 0) return 1;
    }
    path = *end ? end + 1 : end;
  }
  return 0;
}

int mettle_run_process(const char *program, const char *const *arguments) {
  mt_i64 pid = mt_syscall6(MT_SYS_CLONE, 17, 0, 0, 0, 0, 0);
  if (pid < 0) return 127;
  if (pid == 0) {
    mt_exec_search(program, arguments);
    _exit(127);
  }
  int status = 0;
  for (;;) {
    mt_i64 result = mt_syscall6(MT_SYS_WAIT4, pid, (mt_i64)&status, 0, 0, 0, 0);
    if (result >= 0) break;
    if (result != -4) return 127;
  }
  if ((status & 0x7f) != 0) return 128 + (status & 0x7f);
  return (status >> 8) & 0xff;
}

void *popen(const char *command, const char *mode) {
  int descriptors[2] = {-1, -1};
  if (!command || !mode || mode[0] != 'r' || mode[1] != 0) {
    mt_errno_value = 22;
    return MT_NULL;
  }
  (void)getenv("PATH");
  if (mt_sys_result(mt_syscall6(MT_SYS_PIPE2, (mt_i64)descriptors, 0, 0, 0,
                                0, 0)) < 0) {
    return MT_NULL;
  }
  mt_i64 pid = mt_syscall6(MT_SYS_CLONE, 17, 0, 0, 0, 0, 0);
  if (pid < 0) {
    close(descriptors[0]);
    close(descriptors[1]);
    return MT_NULL;
  }
  if (pid == 0) {
    close(descriptors[0]);
    if (descriptors[1] != 1) {
      (void)mt_syscall6(MT_SYS_DUP3, descriptors[1], 1, 0, 0, 0, 0);
      close(descriptors[1]);
    }
    const char *arguments[] = {"sh", "-c", command, MT_NULL};
    (void)mt_syscall6(MT_SYS_EXECVE, (mt_i64)"/bin/sh", (mt_i64)arguments,
                      (mt_i64)mt_environment, 0, 0, 0);
    _exit(127);
  }
  close(descriptors[1]);
  MtFile *file = (MtFile *)malloc(sizeof(MtFile));
  if (!file) {
    close(descriptors[0]);
    return MT_NULL;
  }
  file->handle = descriptors[0];
  file->flags = MT_FILE_READ;
  file->child_pid = pid;
  file->read_buffer = MT_NULL;
  file->read_fill = 0;
  file->read_pos = 0;
  file->write_buffer = MT_NULL;
  file->write_fill = 0;
  file->next_open = MT_NULL;
  return file;
}

int pclose(void *stream) {
  MtFile *file = (MtFile *)stream;
  if (!file || file->child_pid <= 0) {
    mt_errno_value = 22;
    return -1;
  }
  mt_i64 pid = file->child_pid;
  close((int)file->handle);
  free(file->read_buffer);
  free(file);
  int status = 0;
  for (;;) {
    mt_i64 result = mt_syscall6(MT_SYS_WAIT4, pid, (mt_i64)&status, 0, 0, 0, 0);
    if (result >= 0) return status;
    if (result != -4) return -1;
  }
}

#endif

int system(const char *command) {
  if (!command) return 1;
#if defined(_WIN32)
  const char *arguments[] = {"cmd.exe", "/S", "/C", command, MT_NULL};
  return mettle_run_process("cmd.exe", arguments);
#else
  const char *arguments[] = {"sh", "-c", command, MT_NULL};
  return mettle_run_process("/bin/sh", arguments);
#endif
}

/* Every read above this goes straight to the kernel: the copy through the
 * buffer would cost more than the syscall it saves. */
#define MT_FILE_BUFFER_BYPASS (MT_FILE_BUFFER_BYTES / 2)

/* Reads through a per-file buffer when the file was opened read-only. fgets
 * used to ask the kernel for a single byte per character, which is fine on a
 * local disk and ruinous anywhere a syscall is expensive: reading one 46 MB
 * table under WSL took three and a third million read() calls and five
 * minutes. Standard streams and pipes stay unbuffered, so a program that
 * hands its descriptor to a child, or interleaves reads with writes, sees
 * exactly the bytes it did before. */
static mt_ssize mt_stream_read(MtFile *file, void *buffer, mt_size bytes) {
  unsigned char *out = (unsigned char *)buffer;
  mt_size done = 0;

  if (file->write_fill > 0) {
    mt_stream_flush(file);
  }
  if (!(file->flags & MT_FILE_BUFFERED)) {
    return mt_file_read(file, buffer, bytes);
  }

  while (done < bytes) {
    mt_size available = (mt_size)(file->read_fill - file->read_pos);
    mt_size wanted = bytes - done;

    if (available == 0) {
      mt_ssize filled;
      if (wanted >= MT_FILE_BUFFER_BYPASS) {
        filled = mt_file_read(file, out + done, wanted);
        if (filled <= 0) {
          break;
        }
        done += (mt_size)filled;
        continue;
      }
      if (!file->read_buffer) {
        file->read_buffer = (unsigned char *)malloc(MT_FILE_BUFFER_BYTES);
        if (!file->read_buffer) {
          file->flags &= ~MT_FILE_BUFFERED;
          break;
        }
      }
      filled = mt_file_read(file, file->read_buffer, MT_FILE_BUFFER_BYTES);
      if (filled <= 0) {
        break;
      }
      file->read_fill = (int)filled;
      file->read_pos = 0;
      available = (mt_size)filled;
    }

    if (available > wanted) {
      available = wanted;
    }
    memcpy(out + done, file->read_buffer + file->read_pos, available);
    file->read_pos += (int)available;
    done += available;
  }

  if (done == 0 && bytes != 0) {
    return 0;
  }
  return (mt_ssize)done;
}

static MtFile *mt_open_write_files;
static volatile int mt_open_write_lock;

static void mt_open_write_track(struct MtFile *file) {
  MtFile *stream = (MtFile *)file;

  while (__atomic_exchange_n(&mt_open_write_lock, 1, __ATOMIC_ACQUIRE)) {
  }
  stream->next_open = mt_open_write_files;
  mt_open_write_files = stream;
  __atomic_store_n(&mt_open_write_lock, 0, __ATOMIC_RELEASE);
}

static void mt_open_write_forget(struct MtFile *file) {
  MtFile *stream = (MtFile *)file;
  MtFile **link;

  while (__atomic_exchange_n(&mt_open_write_lock, 1, __ATOMIC_ACQUIRE)) {
  }
  link = &mt_open_write_files;
  while (*link) {
    if (*link == stream) {
      *link = stream->next_open;
      break;
    }
    link = &(*link)->next_open;
  }
  stream->next_open = MT_NULL;
  __atomic_store_n(&mt_open_write_lock, 0, __ATOMIC_RELEASE);
}

static int mt_stream_flush(struct MtFile *file) {
  MtFile *stream = (MtFile *)file;
  mt_size pending;

  if (!stream || stream->write_fill <= 0) {
    return 0;
  }
  pending = (mt_size)stream->write_fill;
  stream->write_fill = 0;
  return mt_file_write(stream, stream->write_buffer, pending) ==
                 (mt_ssize)pending
             ? 0
             : -1;
}

static mt_ssize mt_stream_write(MtFile *file, const void *buffer,
                                mt_size bytes) {
  const unsigned char *in = (const unsigned char *)buffer;
  mt_size done = 0;

  if (!(file->flags & MT_FILE_WRITE_BUFFERED)) {
    return mt_file_write(file, buffer, bytes);
  }

  while (done < bytes) {
    mt_size room;
    mt_size wanted = bytes - done;

    if (!file->write_buffer) {
      file->write_buffer = (unsigned char *)malloc(MT_FILE_WRITE_BUFFER_BYTES);
      if (!file->write_buffer) {
        mt_ssize direct = mt_file_write(file, in + done, wanted);
        file->flags &= ~MT_FILE_WRITE_BUFFERED;
        return direct > 0 ? (mt_ssize)done + direct : (mt_ssize)done;
      }
    }

    room = (mt_size)(MT_FILE_WRITE_BUFFER_BYTES - file->write_fill);
    if (room == 0) {
      if (mt_stream_flush(file) != 0) {
        break;
      }
      room = MT_FILE_WRITE_BUFFER_BYTES;
    }
    if (wanted >= room && file->write_fill == 0) {
      mt_ssize written = mt_file_write(file, in + done, wanted);
      if (written <= 0) {
        break;
      }
      done += (mt_size)written;
      continue;
    }
    if (wanted > room) {
      wanted = room;
    }
    memcpy(file->write_buffer + file->write_fill, in + done, wanted);
    file->write_fill += (int)wanted;
    done += wanted;
  }

  return (mt_ssize)done;
}

static void mt_flush_open_streams(void) {
  MtFile *stream = mt_open_write_files;

  while (stream) {
    MtFile *next = stream->next_open;
    mt_stream_flush(stream);
    stream = next;
  }
}

/* Bytes read ahead of what the caller has consumed. Seek and tell have to
 * account for them: the kernel's offset is that far past the stream's. */
static mt_size mt_stream_pending(const MtFile *file) {
  return (mt_size)(file->read_fill - file->read_pos);
}

static void mt_stream_discard(MtFile *file) {
  file->read_fill = 0;
  file->read_pos = 0;
}

mt_size fread(void *buffer, mt_size size, mt_size count, void *stream) {
  if (!stream || (size && count > MT_SIZE_MAX / size)) {
    return 0;
  }
  mt_size bytes = size * count;
  mt_ssize result = mt_stream_read((MtFile *)stream, buffer, bytes);
  if (result <= 0 || size == 0) {
    return 0;
  }
  return (mt_size)result / size;
}

mt_size fwrite(const void *buffer, mt_size size, mt_size count, void *stream) {
  if (!stream || (size && count > MT_SIZE_MAX / size)) {
    return 0;
  }
  mt_size bytes = size * count;
  mt_ssize result = mt_stream_write((MtFile *)stream, buffer, bytes);
  if (result <= 0 || size == 0) {
    return 0;
  }
  return (mt_size)result / size;
}

int fputs(const char *text, void *stream) {
  mt_size length = strlen(text);
  mt_ssize result = mt_stream_write((MtFile *)stream, text, length);
  return result == (mt_ssize)length ? 0 : -1;
}

int puts(const char *text) {
  if (fputs(text, stdout) < 0) {
    return -1;
  }
  return mt_stream_write((MtFile *)stdout, "\n", 1) == 1 ? 0 : -1;
}

int putchar(int character) {
  mt_u8 byte = (mt_u8)character;
  return mt_stream_write((MtFile *)stdout, &byte, 1) == 1 ? character : -1;
}

int getchar(void) {
  mt_u8 byte = 0;
  return mt_file_read((MtFile *)stdin, &byte, 1) == 1 ? (int)byte : -1;
}

char *fgets(char *buffer, int size, void *stream) {
  int used = 0;
  if (!buffer || size <= 0 || !stream) {
    return MT_NULL;
  }
  while (used + 1 < size) {
    mt_u8 byte = 0;
    mt_ssize result = mt_stream_read((MtFile *)stream, &byte, 1);
    if (result != 1) {
      break;
    }
    buffer[used++] = (char)byte;
    if (byte == '\n') {
      break;
    }
  }
  if (used == 0) {
    return MT_NULL;
  }
  buffer[used] = 0;
  return buffer;
}

int fflush(void *stream) {
  if (!stream) {
    mt_flush_open_streams();
    return 0;
  }
  return mt_stream_flush((MtFile *)stream);
}

int ferror(void *stream) {
  (void)stream;
  return 0;
}

int fputc(int character, void *stream) {
  mt_u8 byte = (mt_u8)character;
  return mt_stream_write((MtFile *)stream, &byte, 1) == 1 ? character : -1;
}

int fseek(void *stream, long offset, int origin) {
  MtFile *file = (MtFile *)stream;
  if (!stream || origin < 0 || origin > 2) {
    mt_errno_value = 22;
    return -1;
  }
  /* A seek from the current position has to start from where the caller
   * thinks it is, not from where the read-ahead left the descriptor. */
  mt_stream_flush(file);
  if (origin == 1) {
    offset -= (long)mt_stream_pending(file);
  }
  mt_stream_discard(file);
  return mt_file_seek(file, (mt_i64)offset, origin) < 0 ? -1 : 0;
}

long ftell(void *stream) {
  mt_i64 position;
  mt_stream_flush((MtFile *)stream);
  position = stream ? mt_file_seek((MtFile *)stream, 0, 1) : -1;
  if (position >= 0) {
    position -= (mt_i64)mt_stream_pending((MtFile *)stream);
  }
  return (long)position;
}

/* The 64-bit offset forms. `long` is 32 bits on Windows, so fseek/ftell above
 * cannot address past 2 GB -- and a GGUF model file is routinely larger than
 * that. The seek underneath has always been 64-bit (SetFilePointerEx takes an
 * mt_i64); these are the entry points that let a program reach it, under the
 * names the platform uses for them. */
int _fseeki64(void *stream, mt_i64 offset, int origin) {
  MtFile *file = (MtFile *)stream;
  if (!stream || origin < 0 || origin > 2) {
    mt_errno_value = 22;
    return -1;
  }
  mt_stream_flush(file);
  if (origin == 1) {
    offset -= (mt_i64)mt_stream_pending(file);
  }
  mt_stream_discard(file);
  return mt_file_seek(file, offset, origin) < 0 ? -1 : 0;
}

mt_i64 _ftelli64(void *stream) {
  mt_i64 position;
  mt_stream_flush((MtFile *)stream);
  position = stream ? mt_file_seek((MtFile *)stream, 0, 1) : -1;
  if (position >= 0) {
    position -= (mt_i64)mt_stream_pending((MtFile *)stream);
  }
  return position;
}

void rewind(void *stream) {
  if (stream) {
    mt_stream_flush((MtFile *)stream);
    (void)mt_file_seek((MtFile *)stream, 0, 0);
  }
}

int setvbuf(void *stream, char *buffer, int mode, mt_size size) {
  (void)stream;
  (void)buffer;
  (void)mode;
  (void)size;
  return 0;
}

int fileno(void *stream) {
  MtFile *file = (MtFile *)stream;
  return file ? (int)file->handle : -1;
}

int isatty(int descriptor) {
#if defined(_WIN32)
  MtFile *file = descriptor == 0 ? &mt_stdin_file
                 : descriptor == 1 ? &mt_stdout_file
                                   : &mt_stderr_file;
  mt_u32 mode = 0;
  return GetConsoleMode(mt_windows_std_handle(file), &mode) ? 1 : 0;
#else
  mt_u8 terminal_state[64];
  return mt_sys_result(mt_syscall6(MT_SYS_IOCTL, descriptor, 0x5401,
                                   (mt_i64)terminal_state, 0, 0, 0)) >= 0;
#endif
}

int ioctl(int descriptor, unsigned long request, ...) {
  mt_va_list arguments;
  __builtin_va_start(arguments, request);
  void *value = __builtin_va_arg(arguments, void *);
  __builtin_va_end(arguments);
#if defined(_WIN32)
  (void)descriptor;
  (void)request;
  (void)value;
  mt_errno_value = 38;
  return -1;
#else
  return (int)mt_sys_result(mt_syscall6(MT_SYS_IOCTL, descriptor,
                                        (mt_i64)request, (mt_i64)value, 0, 0,
                                        0));
#endif
}

int remove(const char *path) {
#if defined(_WIN32)
  return unlink(path);
#else
  return (int)mt_sys_result(mt_syscall6(MT_SYS_UNLINKAT, MT_AT_FDCWD,
                                        (mt_i64)path, 0, 0, 0, 0));
#endif
}

mt_i64 clock(void) {
#if defined(_WIN32)
  return (mt_i64)GetTickCount64() * 1000;
#else
  mt_i64 value[2] = {0, 0};
  if (clock_gettime(1, value) != 0) {
    return -1;
  }
  return value[0] * 1000000 + value[1] / 1000;
#endif
}

int *mettle_errno_location(void) { return &mt_errno_value; }

char *strerror(int error) {
  switch (error) {
  case 0:
    return "No error";
  case 2:
    return "File not found";
  case 12:
    return "Out of memory";
  case 13:
    return "Access denied";
  case 22:
    return "Invalid argument";
  default:
    return "Operating system error";
  }
}

char *strdup(const char *text) {
  mt_size length = strlen(text) + 1;
  char *copy = (char *)malloc(length);
  if (copy) {
    memcpy(copy, text, length);
  }
  return copy;
}

void mettle_crash_write_stderr_bytes(const char *text, mt_size length) {
  if (text && length) {
    (void)fwrite(text, 1, length, stderr);
  }
}

void mettle_crash_write_stderr(const char *text) {
  mettle_crash_write_stderr_bytes(text, strlen(text));
}

#define MT_EFFECT_THREADS 256
#define MT_EFFECT_FRAMES 4096

typedef struct {
  const char *name;
  const char *forbids;
  const char *provides;
} MtEffectFrame;

typedef struct {
  mt_u32 thread;
  mt_u32 depth;
  MtEffectFrame *frames;
} MtEffectStack;

static MtEffectStack mt_effect_stacks[MT_EFFECT_THREADS];
static volatile int mt_effect_lock;
static volatile int mt_effects_active;

static void mt_effects_acquire(void) {
  while (__atomic_exchange_n(&mt_effect_lock, 1, __ATOMIC_ACQUIRE)) {
  }
}

static void mt_effects_release(void) {
  __atomic_store_n(&mt_effect_lock, 0, __ATOMIC_RELEASE);
}

static MtEffectStack *mt_effect_stack(int create) {
  mt_u32 thread = mettle_thread_current_id();
  MtEffectStack *free_slot = MT_NULL;
  for (mt_size i = 0; i < MT_EFFECT_THREADS; i++) {
    MtEffectStack *stack = &mt_effect_stacks[i];
    if (stack->frames && stack->thread == thread) {
      return stack;
    }
    if (!stack->frames && !free_slot) {
      free_slot = stack;
    }
  }
  if (!create || !free_slot) {
    return MT_NULL;
  }
  free_slot->frames =
      (MtEffectFrame *)malloc(MT_EFFECT_FRAMES * sizeof(MtEffectFrame));
  free_slot->thread = thread;
  free_slot->depth = 0;
  return free_slot->frames ? free_slot : MT_NULL;
}

static int mt_effect_list_has(const char *list, const char *effect,
                              mt_size length) {
  const char *p = list ? list : "";
  while (*p) {
    const char *end = p;
    while (*end && *end != ',') {
      end++;
    }
    if ((mt_size)(end - p) == length && memcmp(p, effect, length) == 0) {
      return 1;
    }
    p = *end ? end + 1 : end;
  }
  return 0;
}

MT_NORETURN static void mt_effect_violation(const char *function,
                                            const char *effect,
                                            mt_size effect_length,
                                            const char *other,
                                            int is_requirement) {
  mettle_crash_write_stderr("Fatal error: effect violation: `");
  mettle_crash_write_stderr(function);
  mettle_crash_write_stderr(is_requirement ? "` requires `" : "` performs `");
  mettle_crash_write_stderr_bytes(effect, effect_length);
  if (is_requirement) {
    mettle_crash_write_stderr("` and no caller provides it\n");
  } else {
    mettle_crash_write_stderr("`, which `");
    mettle_crash_write_stderr(other);
    mettle_crash_write_stderr("` forbids\n");
  }
  _exit(1);
}

static void mt_effects_check_list(MtEffectStack *stack, const char *list,
                                  const char *function, int is_requirement) {
  const char *p = list ? list : "";
  while (*p) {
    const char *end = p;
    mt_size length;
    while (*end && *end != ',') {
      end++;
    }
    length = (mt_size)(end - p);
    if (is_requirement) {
      int provided = 0;
      for (mt_u32 i = stack ? stack->depth : 0; i > 0 && !provided; i--) {
        provided = mt_effect_list_has(stack->frames[i - 1].provides, p, length);
      }
      if (!provided) {
        mt_effects_release();
        mt_effect_violation(function, p, length, MT_NULL, 1);
      }
    } else {
      for (mt_u32 i = stack ? stack->depth : 0; i > 0; i--) {
        if (mt_effect_list_has(stack->frames[i - 1].forbids, p, length)) {
          const char *other = stack->frames[i - 1].name;
          mt_effects_release();
          mt_effect_violation(function, p, length, other, 0);
        }
      }
    }
    p = *end ? end + 1 : end;
  }
}

void mettle_effects_enter(const char *name, const char *with,
                          const char *forbids, const char *requires,
                          const char *provides) {
  MtEffectStack *stack;
  mt_effects_acquire();
  __atomic_store_n(&mt_effects_active, 1, __ATOMIC_RELEASE);
  stack = mt_effect_stack(1);
  mt_effects_check_list(stack, with, name, 0);
  mt_effects_check_list(stack, requires, name, 1);
  if (stack && stack->depth < MT_EFFECT_FRAMES) {
    stack->frames[stack->depth].name = name;
    stack->frames[stack->depth].forbids = forbids;
    stack->frames[stack->depth].provides = provides;
    stack->depth++;
  }
  mt_effects_release();
}

void mettle_effects_leave(void) {
  MtEffectStack *stack;
  mt_effects_acquire();
  stack = mt_effect_stack(0);
  if (stack && stack->depth > 0) {
    stack->depth--;
  }
  mt_effects_release();
}

void mettle_effects_perform(const char *effect, const char *function) {
  MtEffectStack *stack;
  if (!__atomic_load_n(&mt_effects_active, __ATOMIC_ACQUIRE)) {
    return;
  }
  mt_effects_acquire();
  stack = mt_effect_stack(0);
  mt_effects_check_list(stack, effect, function, 0);
  mt_effects_release();
}

long long (*mettle_crash_heap_classifier)(void *address) = MT_NULL;

void mettle_crash_set_heap_classifier(long long (*classifier)(void *)) {
  mettle_crash_heap_classifier = classifier;
}

mt_i64 atol(const char *text);

int atoi(const char *text) {
  return (int)atol(text);
}

mt_i64 atol(const char *text) {
  mt_i64 value = 0;
  int negative = 0;
  while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
    text++;
  }
  if (*text == '-' || *text == '+') {
    negative = *text++ == '-';
  }
  while (*text >= '0' && *text <= '9') {
    value = value * 10 + (*text++ - '0');
  }
  return negative ? -value : value;
}

mt_i64 atoll(const char *text) { return atol(text); }

static int mt_digit_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'Z') {
    return c - 'A' + 10;
  }
  return -1;
}

mt_u64 strtoull(const char *text, char **end, int base) {
  mt_u64 value = 0;
  while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
    text++;
  }
  if (*text == '+') {
    text++;
  }
  if ((base == 0 || base == 16) && text[0] == '0' &&
      (text[1] == 'x' || text[1] == 'X')) {
    base = 16;
    text += 2;
  } else if (base == 0) {
    base = text[0] == '0' ? 8 : 10;
  }
  const char *start = text;
  for (;;) {
    int digit = mt_digit_value(*text);
    if (digit < 0 || digit >= base) {
      break;
    }
    value = value * (mt_u64)base + (mt_u64)digit;
    text++;
  }
  if (end) {
    *end = (char *)(text == start ? start : text);
  }
  return value;
}

mt_u64 strtoul(const char *text, char **end, int base) {
  return strtoull(text, end, base);
}

mt_i64 strtoll(const char *text, char **end, int base) {
  int negative = 0;
  while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
    text++;
  }
  if (*text == '-' || *text == '+') {
    negative = *text++ == '-';
  }
  mt_u64 value = strtoull(text, end, base);
  return negative ? -(mt_i64)value : (mt_i64)value;
}

mt_i64 _strtoi64(const char *text, char **end, int base) {
  return strtoll(text, end, base);
}

/* Decimal to binary conversion, correctly rounded on every path.

   The previous conversion scaled the mantissa once per exponent step, and
   each of those multiplies rounds: 3.141592653589793 landed five ulp away
   from the double gcc produces for the same text. Every float literal in a
   Mettle program flows through this one function (the parser binds atof to
   it through the host redirect; comptime, the debugger, and linked programs
   call it at run time), so one wrong bit here forks a program's arithmetic
   from the same program built by any other toolchain.

   Two paths, both exact. When the digits fit in 53 bits and the power of
   ten is one a double holds exactly, a single multiply or divide performs
   the only rounding (Clinger's fast case). Every other input converts on a
   decimal digit array scaled by powers of two, where each step is exact and
   the one rounding happens at the final 53-bit extraction; digits past the
   array feed a sticky flag, which is all round-to-nearest-even needs from
   them. */

static const double MT_POW10_F64[23] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};

static const mt_u64 MT_POW10_U64[20] = {1ULL,
                                        10ULL,
                                        100ULL,
                                        1000ULL,
                                        10000ULL,
                                        100000ULL,
                                        1000000ULL,
                                        10000000ULL,
                                        100000000ULL,
                                        1000000000ULL,
                                        10000000000ULL,
                                        100000000000ULL,
                                        1000000000000ULL,
                                        10000000000000ULL,
                                        100000000000000ULL,
                                        1000000000000000ULL,
                                        10000000000000000ULL,
                                        100000000000000000ULL,
                                        1000000000000000000ULL,
                                        10000000000000000000ULL};

#define MT_F64_EXACT_INT 9007199254740992ULL /* 2^53 */
#define MT_F64_INF_BITS 0x7FF0000000000000ULL

static double mt_f64_from_bits(mt_u64 bits) {
  union {
    mt_u64 u;
    double f;
  } pun;
  pun.u = bits;
  return pun.f;
}

/* mant * 10^exp when both convert exactly, so the one multiply or divide is
   the only rounding. Returns 0 when that cannot be guaranteed. */
static int mt_pow10_exact(mt_u64 mant, int exp, double *out) {
  if (mant > MT_F64_EXACT_INT) {
    return 0;
  }
  if (exp >= 0) {
    if (exp <= 22) {
      *out = (double)mant * MT_POW10_F64[exp];
      return 1;
    }
    if (exp <= 22 + 19) {
      int lift = exp - 22;
      if (mant <= MT_F64_EXACT_INT / MT_POW10_U64[lift]) {
        *out = (double)(mant * MT_POW10_U64[lift]) * MT_POW10_F64[22];
        return 1;
      }
    }
    return 0;
  }
  if (exp >= -22) {
    *out = (double)mant / MT_POW10_F64[-exp];
    return 1;
  }
  return 0;
}

/* Arbitrary-precision decimal: value = 0.d[0]d[1]... * 10^dp. 800 digits
   covers the longest string that can distinguish two doubles (767); anything
   dropped past the end only sets `truncated`. */
#define MT_BIGDEC_DIGITS 800
#define MT_BIGDEC_MAX_SHIFT 60

typedef struct {
  unsigned char d[MT_BIGDEC_DIGITS]; /* digit values 0..9 */
  int nd;
  int dp;
  int truncated;
} mt_bigdec;

static void mt_bigdec_trim(mt_bigdec *a) {
  while (a->nd > 0 && a->d[a->nd - 1] == 0) {
    a->nd--;
  }
  if (a->nd == 0) {
    a->dp = 0;
  }
}

/* Divide by 2^k, 0 < k <= 60. Digits stream left to right through a binary
   accumulator, so every kept digit is exact. */
static void mt_bigdec_right_shift(mt_bigdec *a, int k) {
  int r = 0;
  int w = 0;
  mt_u64 n = 0;
  mt_u64 mask = ((mt_u64)1 << k) - 1;

  for (; (n >> k) == 0; r++) {
    if (r >= a->nd) {
      if (n == 0) {
        a->nd = 0;
        return;
      }
      while ((n >> k) == 0) {
        n = n * 10ULL;
        r++;
      }
      break;
    }
    n = n * 10ULL + a->d[r];
  }
  a->dp -= r - 1;

  for (; r < a->nd; r++) {
    unsigned char c = a->d[r];
    a->d[w++] = (unsigned char)(n >> k);
    n = (n & mask) * 10ULL + c;
  }
  while (n > 0) {
    unsigned char dig = (unsigned char)(n >> k);
    if (w < MT_BIGDEC_DIGITS) {
      a->d[w++] = dig;
    } else if (dig != 0) {
      a->truncated = 1;
    }
    n = (n & mask) * 10ULL;
  }
  a->nd = w;
  mt_bigdec_trim(a);
}

/* Multiplying by 2^k adds `delta` digits when the leading digits are at or
   above the decimal expansion of 5^k, and delta-1 digits below it. */
typedef struct {
  int delta;
  const char *cutoff;
} mt_bigdec_cheat;

static const mt_bigdec_cheat MT_BIGDEC_LSHIFT[MT_BIGDEC_MAX_SHIFT + 1] = {
    {0, ""},
    {1, "5"},
    {1, "25"},
    {1, "125"},
    {2, "625"},
    {2, "3125"},
    {2, "15625"},
    {3, "78125"},
    {3, "390625"},
    {3, "1953125"},
    {4, "9765625"},
    {4, "48828125"},
    {4, "244140625"},
    {4, "1220703125"},
    {5, "6103515625"},
    {5, "30517578125"},
    {5, "152587890625"},
    {6, "762939453125"},
    {6, "3814697265625"},
    {6, "19073486328125"},
    {7, "95367431640625"},
    {7, "476837158203125"},
    {7, "2384185791015625"},
    {7, "11920928955078125"},
    {8, "59604644775390625"},
    {8, "298023223876953125"},
    {8, "1490116119384765625"},
    {9, "7450580596923828125"},
    {9, "37252902984619140625"},
    {9, "186264514923095703125"},
    {10, "931322574615478515625"},
    {10, "4656612873077392578125"},
    {10, "23283064365386962890625"},
    {10, "116415321826934814453125"},
    {11, "582076609134674072265625"},
    {11, "2910383045673370361328125"},
    {11, "14551915228366851806640625"},
    {12, "72759576141834259033203125"},
    {12, "363797880709171295166015625"},
    {12, "1818989403545856475830078125"},
    {13, "9094947017729282379150390625"},
    {13, "45474735088646411895751953125"},
    {13, "227373675443232059478759765625"},
    {13, "1136868377216160297393798828125"},
    {14, "5684341886080801486968994140625"},
    {14, "28421709430404007434844970703125"},
    {14, "142108547152020037174224853515625"},
    {15, "710542735760100185871124267578125"},
    {15, "3552713678800500929355621337890625"},
    {15, "17763568394002504646778106689453125"},
    {16, "88817841970012523233890533447265625"},
    {16, "444089209850062616169452667236328125"},
    {16, "2220446049250313080847263336181640625"},
    {16, "11102230246251565404236316680908203125"},
    {17, "55511151231257827021181583404541015625"},
    {17, "277555756156289135105907917022705078125"},
    {17, "1387778780781445675529539585113525390625"},
    {18, "6938893903907228377647697925567626953125"},
    {18, "34694469519536141888238489627838134765625"},
    {18, "173472347597680709441192448139190673828125"},
    {19, "867361737988403547205962240695953369140625"},
};

static int mt_bigdec_prefix_less(const mt_bigdec *a, const char *s) {
  int i;
  for (i = 0; s[i] != 0; i++) {
    if (i >= a->nd) {
      return 1;
    }
    if (a->d[i] != (unsigned char)(s[i] - '0')) {
      return a->d[i] < (unsigned char)(s[i] - '0');
    }
  }
  return 0;
}

/* Multiply by 2^k, 0 < k <= 60. Digits stream right to left. */
static void mt_bigdec_left_shift(mt_bigdec *a, int k) {
  int delta = MT_BIGDEC_LSHIFT[k].delta;
  int r = a->nd - 1;
  int w;
  mt_u64 n = 0;

  if (mt_bigdec_prefix_less(a, MT_BIGDEC_LSHIFT[k].cutoff)) {
    delta--;
  }
  w = a->nd + delta;

  for (; r >= 0; r--) {
    mt_u64 quo;
    n += (mt_u64)a->d[r] << k;
    quo = n / 10ULL;
    w--;
    if (w < MT_BIGDEC_DIGITS) {
      a->d[w] = (unsigned char)(n - 10ULL * quo);
    } else if (n != 10ULL * quo) {
      a->truncated = 1;
    }
    n = quo;
  }
  while (n > 0) {
    mt_u64 quo = n / 10ULL;
    w--;
    if (w < MT_BIGDEC_DIGITS) {
      a->d[w] = (unsigned char)(n - 10ULL * quo);
    } else if (n != 10ULL * quo) {
      a->truncated = 1;
    }
    n = quo;
  }
  a->nd += delta;
  if (a->nd > MT_BIGDEC_DIGITS) {
    a->nd = MT_BIGDEC_DIGITS;
  }
  a->dp += delta;
  mt_bigdec_trim(a);
}

static void mt_bigdec_shift(mt_bigdec *a, int k) {
  if (a->nd == 0) {
    return;
  }
  while (k > MT_BIGDEC_MAX_SHIFT) {
    mt_bigdec_left_shift(a, MT_BIGDEC_MAX_SHIFT);
    k -= MT_BIGDEC_MAX_SHIFT;
  }
  while (k < -MT_BIGDEC_MAX_SHIFT) {
    mt_bigdec_right_shift(a, MT_BIGDEC_MAX_SHIFT);
    k += MT_BIGDEC_MAX_SHIFT;
    if (a->nd == 0) {
      return;
    }
  }
  if (k > 0) {
    mt_bigdec_left_shift(a, k);
  } else if (k < 0) {
    mt_bigdec_right_shift(a, -k);
  }
}

/* Round-half-even at digit position nd. The sticky flag settles the case
   where the stored digits alone read as exactly halfway. */
static int mt_bigdec_round_up(const mt_bigdec *a, int nd) {
  if (nd < 0 || nd >= a->nd) {
    return 0;
  }
  if (a->d[nd] == 5 && nd + 1 == a->nd) {
    if (a->truncated) {
      return 1;
    }
    return nd > 0 && (a->d[nd - 1] & 1) != 0;
  }
  return a->d[nd] >= 5;
}

static mt_u64 mt_bigdec_rounded_integer(const mt_bigdec *a) {
  mt_u64 n = 0;
  int i = 0;
  if (a->dp > 20) {
    return 0xFFFFFFFFFFFFFFFFULL;
  }
  for (; i < a->dp && i < a->nd; i++) {
    n = n * 10ULL + a->d[i];
  }
  for (; i < a->dp; i++) {
    n *= 10ULL;
  }
  if (mt_bigdec_round_up(a, a->dp)) {
    n++;
  }
  return n;
}

static double mt_bigdec_to_double(mt_bigdec *a) {
  /* powtab[i]: the largest k with 2^k <= 10^i, so one shift never overshoots
     the [0.5, 1) target. */
  static const int powtab[9] = {1, 3, 6, 9, 13, 16, 19, 23, 26};
  int exp2 = 0;
  mt_u64 mant;
  mt_u64 bits;

  if (a->nd == 0) {
    return 0.0;
  }
  if (a->dp > 310) {
    return mt_f64_from_bits(MT_F64_INF_BITS);
  }
  if (a->dp < -330) {
    return 0.0;
  }

  while (a->dp > 0) {
    int n = a->dp >= 9 ? 27 : powtab[a->dp];
    mt_bigdec_shift(a, -n);
    exp2 += n;
    if (a->nd == 0) {
      return 0.0;
    }
  }
  while (a->dp < 0 || (a->dp == 0 && a->d[0] < 5)) {
    int n = -a->dp >= 9 ? 27 : powtab[-a->dp];
    mt_bigdec_shift(a, n);
    exp2 -= n;
    if (a->nd == 0) {
      return 0.0;
    }
  }
  /* Value is in [0.5, 1) times 2^exp2; renormalize to [1, 2). */
  exp2--;

  if (exp2 < -1022) {
    int n = -1022 - exp2;
    mt_bigdec_shift(a, -n);
    exp2 += n;
  }
  if (exp2 + 1023 >= 0x7FF) {
    return mt_f64_from_bits(MT_F64_INF_BITS);
  }

  mt_bigdec_shift(a, 53);
  mant = mt_bigdec_rounded_integer(a);
  if (mant == (2ULL << 52)) {
    mant >>= 1;
    exp2++;
    if (exp2 + 1023 >= 0x7FF) {
      return mt_f64_from_bits(MT_F64_INF_BITS);
    }
  }
  if ((mant & (1ULL << 52)) == 0) {
    exp2 = -1023; /* subnormal: exponent field 0 */
  }
  bits = mant & ((1ULL << 52) - 1);
  bits |= (mt_u64)((exp2 + 1023) & 0x7FF) << 52;
  return mt_f64_from_bits(bits);
}

double strtod(const char *text, char **end) {
  const char *start = text;
  mt_bigdec dec;
  double value = 0.0;
  int negative = 0;
  int saw_digit = 0;
  int sig_seen = 0;

  dec.nd = 0;
  dec.dp = 0;
  dec.truncated = 0;

  while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
    text++;
  }
  if (*text == '-' || *text == '+') {
    negative = *text++ == '-';
  }
  while (*text >= '0' && *text <= '9') {
    int c = *text++ - '0';
    saw_digit = 1;
    if (c == 0 && !sig_seen) {
      continue;
    }
    sig_seen = 1;
    dec.dp++;
    if (dec.nd < MT_BIGDEC_DIGITS) {
      dec.d[dec.nd++] = (unsigned char)c;
    } else if (c != 0) {
      dec.truncated = 1;
    }
  }
  if (*text == '.') {
    text++;
    while (*text >= '0' && *text <= '9') {
      int c = *text++ - '0';
      saw_digit = 1;
      if (c == 0 && !sig_seen) {
        dec.dp--;
        continue;
      }
      sig_seen = 1;
      if (dec.nd < MT_BIGDEC_DIGITS) {
        dec.d[dec.nd++] = (unsigned char)c;
      } else if (c != 0) {
        dec.truncated = 1;
      }
    }
  }
  if (saw_digit && (*text == 'e' || *text == 'E')) {
    const char *mark = text++;
    int exp_negative = 0;
    int exp_value = 0;
    int exp_digits = 0;
    if (*text == '-' || *text == '+') {
      exp_negative = *text++ == '-';
    }
    while (*text >= '0' && *text <= '9') {
      if (exp_value < 10000) {
        exp_value = exp_value * 10 + (*text - '0');
      }
      text++;
      exp_digits = 1;
    }
    if (exp_digits) {
      dec.dp += exp_negative ? -exp_value : exp_value;
    } else {
      text = mark;
    }
  }

  if (!saw_digit) {
    text = start;
  } else {
    mt_bigdec_trim(&dec);
    if (dec.nd > 0) {
      int have = 0;
      if (!dec.truncated && dec.nd <= 19) {
        mt_u64 mant = 0;
        int i = 0;
        while (i < dec.nd) {
          mant = mant * 10ULL + (mt_u64)dec.d[i];
          i++;
        }
        have = mt_pow10_exact(mant, dec.dp - dec.nd, &value);
      }
      if (!have) {
        value = mt_bigdec_to_double(&dec);
      }
    }
  }
  if (end) {
    *end = (char *)text;
  }
  return negative ? -value : value;
}

double atof(const char *text) { return strtod(text, MT_NULL); }

mt_u64 _strtoui64(const char *text, char **end, int base) {
  return strtoull(text, end, base);
}

static void mt_swap_bytes(mt_u8 *left, mt_u8 *right, mt_size width) {
  for (mt_size i = 0; i < width; i++) {
    mt_u8 value = left[i];
    left[i] = right[i];
    right[i] = value;
  }
}

void qsort(void *base, mt_size count, mt_size width,
           int (*compare)(const void *, const void *)) {
  mt_u8 *bytes = (mt_u8 *)base;
  if (!bytes || !compare || width == 0 || count < 2) {
    return;
  }
  for (mt_size start = count / 2; start != 0; start--) {
    mt_size root = start - 1;
    for (;;) {
      mt_size child = root * 2 + 1;
      if (child >= count) {
        break;
      }
      if (child + 1 < count &&
          compare(bytes + child * width, bytes + (child + 1) * width) < 0) {
        child++;
      }
      if (compare(bytes + root * width, bytes + child * width) >= 0) {
        break;
      }
      mt_swap_bytes(bytes + root * width, bytes + child * width, width);
      root = child;
    }
  }
  for (mt_size end = count - 1; end != 0; end--) {
    mt_swap_bytes(bytes, bytes + end * width, width);
    mt_size root = 0;
    for (;;) {
      mt_size child = root * 2 + 1;
      if (child >= end) {
        break;
      }
      if (child + 1 < end &&
          compare(bytes + child * width, bytes + (child + 1) * width) < 0) {
        child++;
      }
      if (compare(bytes + root * width, bytes + child * width) >= 0) {
        break;
      }
      mt_swap_bytes(bytes + root * width, bytes + child * width, width);
      root = child;
    }
  }
}

void *bsearch(const void *key, const void *base, mt_size count, mt_size width,
              int (*compare)(const void *, const void *)) {
  const mt_u8 *bytes = (const mt_u8 *)base;
  mt_size low = 0;
  mt_size high = count;
  while (low < high) {
    mt_size middle = low + (high - low) / 2;
    const void *item = bytes + middle * width;
    int order = compare(key, item);
    if (order < 0) {
      high = middle;
    } else if (order > 0) {
      low = middle + 1;
    } else {
      return (void *)item;
    }
  }
  return MT_NULL;
}

typedef struct MtFormatOutput {
  char *buffer;
  mt_size capacity;
  mt_size count;
} MtFormatOutput;

static void mt_format_put(MtFormatOutput *out, char character) {
  if (out->buffer && out->capacity && out->count + 1 < out->capacity) {
    out->buffer[out->count] = character;
  }
  out->count++;
}

static void mt_format_repeat(MtFormatOutput *out, char character, int count) {
  while (count-- > 0) {
    mt_format_put(out, character);
  }
}

static void mt_format_text(MtFormatOutput *out, const char *text,
                           mt_size length) {
  for (mt_size i = 0; i < length; i++) {
    mt_format_put(out, text[i]);
  }
}

static mt_size mt_unsigned_text(char *out, mt_u64 value, unsigned base,
                                int upper) {
  const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  char reverse[64];
  mt_size count = 0;
  do {
    reverse[count++] = digits[value % base];
    value /= base;
  } while (value);
  for (mt_size i = 0; i < count; i++) {
    out[i] = reverse[count - i - 1];
  }
  return count;
}

static void mt_format_field(MtFormatOutput *out, const char *prefix,
                            mt_size prefix_length, const char *text,
                            mt_size text_length, int width, int precision,
                            int left, int zero) {
  int zero_count = precision > (int)text_length ? precision - (int)text_length
                                                 : 0;
  int padding = width - (int)prefix_length - zero_count - (int)text_length;
  if (padding < 0) {
    padding = 0;
  }
  if (!left && !zero) {
    mt_format_repeat(out, ' ', padding);
  }
  mt_format_text(out, prefix, prefix_length);
  if (!left && zero) {
    mt_format_repeat(out, '0', padding);
  }
  mt_format_repeat(out, '0', zero_count);
  mt_format_text(out, text, text_length);
  if (left) {
    mt_format_repeat(out, ' ', padding);
  }
}

static int mt_double_special(double value, char *out, int upper,
                             int *negative) {
  union {
    double value;
    mt_u64 bits;
  } bits;
  bits.value = value;
  *negative = (int)(bits.bits >> 63);
  mt_u64 exponent_bits = (bits.bits >> 52) & 0x7ffu;
  mt_u64 fraction = bits.bits & 0x000fffffffffffffULL;
  if (exponent_bits != 0x7ffu) {
    return 0;
  }
  const char *word = fraction ? (upper ? "NAN" : "nan")
                              : (upper ? "INF" : "inf");
  out[0] = word[0];
  out[1] = word[1];
  out[2] = word[2];
  return 3;
}

static double mt_decimal_round(int precision) {
  double round = 0.5;
  while (precision-- > 0) {
    round *= 0.1;
  }
  return round;
}

static mt_size mt_float_fixed(char *out, double value, int precision) {
  if (precision < 0) {
    precision = 6;
  }
  if (precision > 30) {
    precision = 30;
  }
  value += mt_decimal_round(precision);
  mt_u64 whole = (mt_u64)value;
  double fraction = value - (double)whole;
  mt_size used = mt_unsigned_text(out, whole, 10, 0);
  if (precision > 0) {
    out[used++] = '.';
    for (int i = 0; i < precision; i++) {
      fraction *= 10.0;
      int digit = (int)fraction;
      if (digit < 0) {
        digit = 0;
      } else if (digit > 9) {
        digit = 9;
      }
      out[used++] = (char)('0' + digit);
      fraction -= digit;
    }
  }
  return used;
}

static int mt_float_exponent(double *value) {
  int exponent = 0;
  if (*value != 0.0) {
    while (*value >= 10.0 && exponent < 400) {
      *value *= 0.1;
      exponent++;
    }
    while (*value < 1.0 && exponent > -400) {
      *value *= 10.0;
      exponent--;
    }
  }
  return exponent;
}

static mt_size mt_float_exponential(char *out, double value, int precision,
                                    int upper) {
  int exponent = mt_float_exponent(&value);
  if (precision < 0) {
    precision = 6;
  }
  value += mt_decimal_round(precision);
  if (value >= 10.0) {
    value *= 0.1;
    exponent++;
  }
  mt_size used = mt_float_fixed(out, value, precision);
  out[used++] = upper ? 'E' : 'e';
  out[used++] = exponent < 0 ? '-' : '+';
  if (exponent < 0) {
    exponent = -exponent;
  }
  char digits[16];
  mt_size digit_count = mt_unsigned_text(digits, (mt_u64)exponent, 10, 0);
  if (digit_count < 2) {
    out[used++] = '0';
  }
  memcpy(out + used, digits, digit_count);
  return used + digit_count;
}

static mt_size mt_trim_float(char *text, mt_size length) {
  mt_size exponent = length;
  for (mt_size i = 0; i < length; i++) {
    if (text[i] == 'e' || text[i] == 'E') {
      exponent = i;
      break;
    }
  }
  mt_size end = exponent;
  while (end && text[end - 1] == '0') {
    end--;
  }
  if (end && text[end - 1] == '.') {
    end--;
  }
  if (exponent < length) {
    memmove(text + end, text + exponent, length - exponent);
    end += length - exponent;
  }
  return end;
}

static mt_size mt_float_text(char *out, double value, char spec,
                             int precision, int alternate, int *negative) {
  int upper = spec == 'F' || spec == 'E' || spec == 'G';
  int special = mt_double_special(value, out, upper, negative);
  if (special) {
    return (mt_size)special;
  }
  if (*negative) {
    value = -value;
  }
  double normalized = value;
  int exponent = mt_float_exponent(&normalized);
  mt_size length;
  if (spec == 'e' || spec == 'E') {
    length = mt_float_exponential(out, value, precision, upper);
  } else if (spec == 'g' || spec == 'G') {
    if (precision < 0) {
      precision = 6;
    }
    if (precision == 0) {
      precision = 1;
    }
    if (exponent < -4 || exponent >= precision) {
      length = mt_float_exponential(out, value, precision - 1, upper);
    } else {
      int places = precision - exponent - 1;
      length = mt_float_fixed(out, value, places > 0 ? places : 0);
    }
    if (!alternate) {
      length = mt_trim_float(out, length);
    }
  } else if (exponent > 18) {
    length = mt_float_exponential(out, value, precision, upper);
  } else {
    length = mt_float_fixed(out, value, precision);
  }
  return length;
}

int vsnprintf(char *buffer, mt_size capacity, const char *format,
              mt_va_list arguments) {
  MtFormatOutput out = {buffer, capacity, 0};
  while (*format) {
    if (*format != '%') {
      mt_format_put(&out, *format++);
      continue;
    }
    format++;
    if (*format == '%') {
      mt_format_put(&out, *format++);
      continue;
    }
    int left = 0, plus = 0, space = 0, alternate = 0, zero = 0;
    for (;;) {
      if (*format == '-') left = 1;
      else if (*format == '+') plus = 1;
      else if (*format == ' ') space = 1;
      else if (*format == '#') alternate = 1;
      else if (*format == '0') zero = 1;
      else break;
      format++;
    }
    int width = 0;
    if (*format == '*') {
      width = __builtin_va_arg(arguments, int);
      format++;
      if (width < 0) {
        left = 1;
        width = -width;
      }
    } else {
      while (*format >= '0' && *format <= '9') {
        width = width * 10 + (*format++ - '0');
      }
    }
    int precision = -1;
    if (*format == '.') {
      precision = 0;
      format++;
      if (*format == '*') {
        precision = __builtin_va_arg(arguments, int);
        format++;
      } else {
        while (*format >= '0' && *format <= '9') {
          precision = precision * 10 + (*format++ - '0');
        }
      }
    }
    int length = 0;
    if (*format == 'h') {
      format++;
      length = *format == 'h' ? (format++, -2) : -1;
    } else if (*format == 'l') {
      format++;
      length = *format == 'l' ? (format++, 2) : 1;
    } else if (*format == 'z' || *format == 't' || *format == 'j') {
      format++;
      length = 2;
    } else if (*format == 'L') {
      format++;
      length = 3;
    }
    char spec = *format ? *format++ : 0;
    if (spec == 's') {
      const char *text = __builtin_va_arg(arguments, const char *);
      if (!text) text = "(null)";
      mt_size text_length = strlen(text);
      if (precision >= 0 && text_length > (mt_size)precision) {
        text_length = (mt_size)precision;
      }
      int padding = width > (int)text_length ? width - (int)text_length : 0;
      if (!left) mt_format_repeat(&out, ' ', padding);
      mt_format_text(&out, text, text_length);
      if (left) mt_format_repeat(&out, ' ', padding);
    } else if (spec == 'c') {
      char character = (char)__builtin_va_arg(arguments, int);
      int padding = width > 1 ? width - 1 : 0;
      if (!left) mt_format_repeat(&out, ' ', padding);
      mt_format_put(&out, character);
      if (left) mt_format_repeat(&out, ' ', padding);
    } else if (spec == 'd' || spec == 'i' || spec == 'u' || spec == 'o' ||
               spec == 'x' || spec == 'X' || spec == 'p') {
      mt_u64 value;
      int negative = 0;
      int is_signed = spec == 'd' || spec == 'i';
      if (spec == 'p') {
        value = (mt_u64)__builtin_va_arg(arguments, void *);
      } else if (is_signed) {
        mt_i64 signed_value = length >= 2 ? __builtin_va_arg(arguments, mt_i64)
                              : length == 1 ? (mt_i64)__builtin_va_arg(arguments, long)
                                            : (mt_i64)__builtin_va_arg(arguments, int);
        negative = signed_value < 0;
        value = negative ? (mt_u64)(-(signed_value + 1)) + 1 : (mt_u64)signed_value;
      } else {
        value = length >= 2 ? __builtin_va_arg(arguments, mt_u64)
                : length == 1 ? (mt_u64)__builtin_va_arg(arguments, unsigned long)
                              : (mt_u64)__builtin_va_arg(arguments, unsigned int);
      }
      unsigned base = spec == 'o' ? 8u : (spec == 'x' || spec == 'X' || spec == 'p') ? 16u : 10u;
      char number[64];
      mt_size number_length = (precision == 0 && value == 0)
                                  ? 0
                                  : mt_unsigned_text(number, value, base, spec == 'X');
      char prefix[3];
      mt_size prefix_length = 0;
      if (negative) prefix[prefix_length++] = '-';
      else if (is_signed && plus) prefix[prefix_length++] = '+';
      else if (is_signed && space) prefix[prefix_length++] = ' ';
      if (spec == 'p' || (alternate && value && base == 16)) {
        prefix[prefix_length++] = '0';
        prefix[prefix_length++] = spec == 'X' ? 'X' : 'x';
      } else if (alternate && value && base == 8) {
        prefix[prefix_length++] = '0';
      }
      mt_format_field(&out, prefix, prefix_length, number, number_length, width,
                      precision, left, zero && precision < 0);
    } else if (spec == 'f' || spec == 'F' || spec == 'e' || spec == 'E' ||
               spec == 'g' || spec == 'G') {
      double value = __builtin_va_arg(arguments, double);
      char number[128];
      int negative = 0;
      mt_size number_length = mt_float_text(number, value, spec, precision,
                                             alternate, &negative);
      char prefix[1];
      mt_size prefix_length = 0;
      if (negative) prefix[prefix_length++] = '-';
      else if (plus) prefix[prefix_length++] = '+';
      else if (space) prefix[prefix_length++] = ' ';
      mt_format_field(&out, prefix, prefix_length, number, number_length, width,
                      -1, left, zero);
    } else if (spec == 'n') {
      if (length >= 2) *__builtin_va_arg(arguments, mt_i64 *) = (mt_i64)out.count;
      else if (length == 1) *__builtin_va_arg(arguments, long *) = (long)out.count;
      else *__builtin_va_arg(arguments, int *) = (int)out.count;
    } else if (spec) {
      mt_format_put(&out, '%');
      mt_format_put(&out, spec);
    }
  }
  if (out.buffer && out.capacity) {
    mt_size end = out.count < out.capacity ? out.count : out.capacity - 1;
    out.buffer[end] = 0;
  }
  return out.count > 0x7fffffffu ? -1 : (int)out.count;
}

int snprintf(char *buffer, mt_size capacity, const char *format, ...) {
  mt_va_list arguments;
  __builtin_va_start(arguments, format);
  int result = vsnprintf(buffer, capacity, format, arguments);
  __builtin_va_end(arguments);
  return result;
}

int sprintf(char *buffer, const char *format, ...) {
  mt_va_list arguments;
  __builtin_va_start(arguments, format);
  int result = vsnprintf(buffer, MT_SIZE_MAX, format, arguments);
  __builtin_va_end(arguments);
  return result;
}

int vfprintf(void *stream, const char *format, mt_va_list arguments) {
  mt_va_list copy;
  __builtin_va_copy(copy, arguments);
  int needed = vsnprintf(MT_NULL, 0, format, copy);
  __builtin_va_end(copy);
  if (needed < 0) return -1;
  char local[512];
  char *text = local;
  if ((mt_size)needed + 1 > sizeof(local)) {
    text = (char *)malloc((mt_size)needed + 1);
    if (!text) return -1;
  }
  (void)vsnprintf(text, (mt_size)needed + 1, format, arguments);
  int result = fwrite(text, 1, (mt_size)needed, stream) == (mt_size)needed
                   ? needed
                   : -1;
  if (text != local) free(text);
  return result;
}

int fprintf(void *stream, const char *format, ...) {
  mt_va_list arguments;
  __builtin_va_start(arguments, format);
  int result = vfprintf(stream, format, arguments);
  __builtin_va_end(arguments);
  return result;
}

int printf(const char *format, ...) {
  mt_va_list arguments;
  __builtin_va_start(arguments, format);
  int result = vfprintf(stdout, format, arguments);
  __builtin_va_end(arguments);
  return result;
}

static int mt_scan_contains(const char *begin, const char *end, int invert,
                            char character) {
  int found = 0;
  for (const char *item = begin; item < end; item++) {
    if (item + 2 < end && item[1] == '-') {
      if (character >= item[0] && character <= item[2]) found = 1;
      item += 2;
    } else if (*item == character) {
      found = 1;
    }
  }
  return invert ? !found : found;
}

int vsscanf(const char *input, const char *format, mt_va_list arguments) {
  const char *cursor = input;
  int assigned = 0;
  while (*format) {
    if (isspace((mt_u8)*format)) {
      while (isspace((mt_u8)*format)) format++;
      while (isspace((mt_u8)*cursor)) cursor++;
      continue;
    }
    if (*format != '%') {
      if (*cursor != *format) break;
      cursor++;
      format++;
      continue;
    }
    format++;
    if (*format == '%') {
      if (*cursor != '%') break;
      cursor++;
      format++;
      continue;
    }
    int suppress = 0;
    if (*format == '*') {
      suppress = 1;
      format++;
    }
    int width = 0;
    while (*format >= '0' && *format <= '9') {
      width = width * 10 + (*format++ - '0');
    }
    int length = 0;
    if (*format == 'h') {
      format++;
      length = *format == 'h' ? (format++, -2) : -1;
    } else if (*format == 'l') {
      format++;
      length = *format == 'l' ? (format++, 2) : 1;
    } else if (*format == 'z' || *format == 't' || *format == 'j') {
      format++;
      length = 2;
    }
    char spec = *format ? *format++ : 0;
    if (spec != 'c' && spec != '[' && spec != 'n') {
      while (isspace((mt_u8)*cursor)) cursor++;
    }
    if (spec == 'n') {
      if (!suppress) {
        mt_size count = (mt_size)(cursor - input);
        if (length >= 2) *__builtin_va_arg(arguments, mt_i64 *) = (mt_i64)count;
        else if (length == 1) *__builtin_va_arg(arguments, long *) = (long)count;
        else *__builtin_va_arg(arguments, int *) = (int)count;
      }
      continue;
    }
    if (spec == 's' || spec == 'c') {
      int limit = width ? width : (spec == 'c' ? 1 : 0x7fffffff);
      const char *start = cursor;
      while (*cursor && limit > 0 && (spec == 'c' || !isspace((mt_u8)*cursor))) {
        cursor++;
        limit--;
      }
      if (cursor == start) break;
      if (!suppress) {
        char *out = __builtin_va_arg(arguments, char *);
        mt_size count = (mt_size)(cursor - start);
        memcpy(out, start, count);
        if (spec == 's') out[count] = 0;
        assigned++;
      }
    } else if (spec == '[') {
      int invert = 0;
      if (*format == '^') {
        invert = 1;
        format++;
      }
      const char *set = format;
      if (*format == ']') format++;
      while (*format && *format != ']') format++;
      const char *set_end = format;
      if (*format == ']') format++;
      int limit = width ? width : 0x7fffffff;
      const char *start = cursor;
      while (*cursor && limit-- > 0 &&
             mt_scan_contains(set, set_end, invert, *cursor)) cursor++;
      if (cursor == start) break;
      if (!suppress) {
        char *out = __builtin_va_arg(arguments, char *);
        mt_size count = (mt_size)(cursor - start);
        memcpy(out, start, count);
        out[count] = 0;
        assigned++;
      }
    } else if (spec == 'd' || spec == 'i' || spec == 'u' || spec == 'x' || spec == 'X' || spec == 'o') {
      const char *start = cursor;
      int negative = 0;
      if ((*cursor == '-' || *cursor == '+') && (!width || cursor - start < width)) {
        negative = *cursor++ == '-';
      }
      unsigned base = spec == 'o' ? 8u : (spec == 'x' || spec == 'X') ? 16u : 10u;
      if (spec == 'i') base = 0;
      if ((base == 0 || base == 16) && cursor[0] == '0' &&
          (cursor[1] == 'x' || cursor[1] == 'X')) {
        cursor += 2;
        base = 16;
      } else if (base == 0) {
        base = cursor[0] == '0' ? 8u : 10u;
      }
      mt_u64 value = 0;
      int digits = 0;
      while (*cursor && (!width || cursor - start < width)) {
        int digit = mt_digit_value(*cursor);
        if (digit < 0 || (unsigned)digit >= base) break;
        value = value * base + (unsigned)digit;
        cursor++;
        digits++;
      }
      if (!digits) {
        cursor = start;
        break;
      }
      if (!suppress) {
        if (spec == 'd' || spec == 'i') {
          mt_i64 signed_value = negative ? -(mt_i64)value : (mt_i64)value;
          if (length >= 2) *__builtin_va_arg(arguments, mt_i64 *) = signed_value;
          else if (length == 1) *__builtin_va_arg(arguments, long *) = (long)signed_value;
          else if (length == -1) *__builtin_va_arg(arguments, short *) = (short)signed_value;
          else if (length == -2) *__builtin_va_arg(arguments, signed char *) = (signed char)signed_value;
          else *__builtin_va_arg(arguments, int *) = (int)signed_value;
        } else {
          if (negative) value = (mt_u64)(-(mt_i64)value);
          if (length >= 2) *__builtin_va_arg(arguments, mt_u64 *) = value;
          else if (length == 1) *__builtin_va_arg(arguments, unsigned long *) = (unsigned long)value;
          else if (length == -1) *__builtin_va_arg(arguments, unsigned short *) = (unsigned short)value;
          else if (length == -2) *__builtin_va_arg(arguments, unsigned char *) = (unsigned char)value;
          else *__builtin_va_arg(arguments, unsigned int *) = (unsigned int)value;
        }
        assigned++;
      }
    } else {
      break;
    }
  }
  return assigned;
}

int sscanf(const char *input, const char *format, ...) {
  mt_va_list arguments;
  __builtin_va_start(arguments, format);
  int result = vsscanf(input, format, arguments);
  __builtin_va_end(arguments);
  return result;
}

void mettle_alloc_report(void) {
  fprintf(stderr, "Owned runtime heap: %llu allocations, %llu frees\n",
          (unsigned long long)__atomic_load_n(&mt_allocation_count,
                                              __ATOMIC_RELAXED),
          (unsigned long long)__atomic_load_n(&mt_free_count,
                                              __ATOMIC_RELAXED));
}

static mt_u64 mt_random_state = 0x9e3779b97f4a7c15ULL;

void srand(unsigned int seed) {
  mt_random_state = seed ? seed : 1;
}

int rand(void) {
  mt_u64 x = mt_random_state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  mt_random_state = x;
  return (int)(x & 0x7fffffffu);
}

mt_u32 htons(mt_u32 value) {
  return ((value & 0xffu) << 8) | ((value >> 8) & 0xffu);
}

mt_u32 ntohs(mt_u32 value) { return htons(value); }

mt_u32 htonl(mt_u32 value) {
  return ((value & 0x000000ffu) << 24) | ((value & 0x0000ff00u) << 8) |
         ((value & 0x00ff0000u) >> 8) | ((value & 0xff000000u) >> 24);
}

mt_u32 ntohl(mt_u32 value) { return htonl(value); }

mt_u32 inet_addr(const char *text) {
  mt_u32 bytes[4] = {0, 0, 0, 0};
  for (int part = 0; part < 4; part++) {
    if (*text < '0' || *text > '9') {
      return 0xffffffffu;
    }
    while (*text >= '0' && *text <= '9') {
      bytes[part] = bytes[part] * 10u + (mt_u32)(*text++ - '0');
      if (bytes[part] > 255u) {
        return 0xffffffffu;
      }
    }
    if (part != 3) {
      if (*text++ != '.') {
        return 0xffffffffu;
      }
    }
  }
  if (*text) {
    return 0xffffffffu;
  }
  return bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
}

int isspace(int character) {
  return character == ' ' || character == '\t' || character == '\n' ||
         character == '\r' || character == '\f' || character == '\v';
}

int isalpha(int character) {
  return (character >= 'a' && character <= 'z') ||
         (character >= 'A' && character <= 'Z');
}

int isalnum(int character) {
  return isalpha(character) || (character >= '0' && character <= '9');
}

int isdigit(int character) { return character >= '0' && character <= '9'; }

int isxdigit(int character) {
  return isdigit(character) || (character >= 'a' && character <= 'f') ||
         (character >= 'A' && character <= 'F');
}

int isupper(int character) { return character >= 'A' && character <= 'Z'; }

int islower(int character) { return character >= 'a' && character <= 'z'; }

int isprint(int character) { return character >= 0x20 && character <= 0x7e; }

int isgraph(int character) { return character >= 0x21 && character <= 0x7e; }

int ispunct(int character) { return isgraph(character) && !isalnum(character); }

int iscntrl(int character) { return character < 0x20 || character == 0x7f; }

int tolower(int character) {
  return character >= 'A' && character <= 'Z' ? character + ('a' - 'A')
                                               : character;
}

int toupper(int character) {
  return character >= 'a' && character <= 'z' ? character - ('a' - 'A')
                                               : character;
}

#include "mt_math.h"

/* Program ABI: Mettle code binds these by name, e.g.
 * `extern fn expf(x: float32) -> float32 = "expf"`, and the float vectorizers
 * recognize a call to expf. */
float sqrtf(float value) { return mt_sqrtf(value); }

float expf(float value) { return (float)mt_exp((double)value); }

float tanhf(float value) { return (float)mt_tanh((double)value); }

/* The double forms of these live in std/math, written in Mettle; exporting
 * them here as well would make every program that imports std/math fail to
 * link on a duplicate symbol. The float forms have no Mettle counterpart, so
 * they belong here next to sqrtf and expf. */
float logf(float value) { return (float)mt_log((double)value); }

float powf(float base, float exponent) {
  return (float)mt_pow((double)base, (double)exponent);
}

float sinf(float value) { return (float)mt_sin((double)value); }

float cosf(float value) { return (float)mt_cos((double)value); }

#if defined(MTLC_HOST_PREFIX_H)
/*
 * Host-only. These back mtlc_host_fabs/exp/tanh for the compiler's own
 * optimizer and GNN. On the program side std/math implements fabs, exp and
 * tanh in Mettle, so exporting them here too would make every program that
 * imports std/math fail to link on a duplicate symbol.
 */
double fabs(double value) { return value < 0.0 ? -value : value; }

double exp(double value) { return mt_exp(value); }

double tanh(double value) { return mt_tanh(value); }
#endif

int __popcountdi2(mt_u64 value) {
  int count = 0;
  while (value) {
    value &= value - 1;
    count++;
  }
  return count;
}

#if defined(__SIZEOF_INT128__)
typedef unsigned __int128 mt_u128;
typedef __int128 mt_i128;

static mt_u128 mt_u128_divmod(mt_u128 numerator, mt_u128 denominator,
                              mt_u128 *remainder) {
  mt_u128 quotient = 0;
  mt_u128 current = 0;
  if (denominator == 0) {
    abort();
  }
  for (int bit = 127; bit >= 0; bit--) {
    current = (current << 1) | ((numerator >> bit) & 1);
    if (current >= denominator) {
      current -= denominator;
      quotient |= (mt_u128)1 << bit;
    }
  }
  if (remainder) {
    *remainder = current;
  }
  return quotient;
}

mt_u128 __udivti3(mt_u128 numerator, mt_u128 denominator) {
  return mt_u128_divmod(numerator, denominator, MT_NULL);
}

mt_u128 __umodti3(mt_u128 numerator, mt_u128 denominator) {
  mt_u128 remainder = 0;
  (void)mt_u128_divmod(numerator, denominator, &remainder);
  return remainder;
}

mt_i128 __divti3(mt_i128 numerator, mt_i128 denominator) {
  int negative = (numerator < 0) != (denominator < 0);
  mt_u128 a = numerator < 0 ? (~(mt_u128)numerator) + 1 : (mt_u128)numerator;
  mt_u128 b = denominator < 0 ? (~(mt_u128)denominator) + 1
                              : (mt_u128)denominator;
  mt_u128 value = mt_u128_divmod(a, b, MT_NULL);
  return negative ? (mt_i128)((~value) + 1) : (mt_i128)value;
}

mt_i128 __modti3(mt_i128 numerator, mt_i128 denominator) {
  int negative = numerator < 0;
  mt_u128 a = negative ? (~(mt_u128)numerator) + 1 : (mt_u128)numerator;
  mt_u128 b = denominator < 0 ? (~(mt_u128)denominator) + 1
                              : (mt_u128)denominator;
  mt_u128 remainder = 0;
  (void)mt_u128_divmod(a, b, &remainder);
  return negative ? (mt_i128)((~remainder) + 1) : (mt_i128)remainder;
}
#endif

MT_NORETURN void abort(void) { exit(134); }

void __stack_chk_fail(void) { abort(); }
