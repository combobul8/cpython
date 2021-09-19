#include <windows.h>

typedef enum _Py_memory_order {
    _Py_memory_order_relaxed,
    _Py_memory_order_acquire,
    _Py_memory_order_release,
    _Py_memory_order_acq_rel,
    _Py_memory_order_seq_cst
} _Py_memory_order;

inline intptr_t _Py_atomic_load_64bit_impl(volatile uintptr_t* value, int order) {
    __int64 old;
    switch (order) {
    case _Py_memory_order_acquire:
    {
      do {
        old = *value;
      } while(_InterlockedCompareExchange64_HLEAcquire((volatile __int64*)value, old, old) != old);
      break;
    }
    case _Py_memory_order_release:
    {
      do {
        old = *value;
      } while(_InterlockedCompareExchange64_HLERelease((volatile __int64*)value, old, old) != old);
      break;
    }
    case _Py_memory_order_relaxed:
      old = *value;
      break;
    default:
    {
      do {
        old = *value;
      } while(_InterlockedCompareExchange64((volatile __int64*)value, old, old) != old);
      break;
    }
    }
    return old;
}

#define _Py_atomic_load_64bit(ATOMIC_VAL, ORDER) \
    _Py_atomic_load_64bit_impl((volatile uintptr_t*)&((ATOMIC_VAL)->_value), (ORDER))

inline int _Py_atomic_load_32bit_impl(volatile int* value, int order) {
    long old;
    switch (order) {
    case _Py_memory_order_acquire:
    {
      do {
        old = *value;
      } while(_InterlockedCompareExchange_HLEAcquire((volatile long*)value, old, old) != old);
      break;
    }
    case _Py_memory_order_release:
    {
      do {
        old = *value;
      } while(_InterlockedCompareExchange_HLERelease((volatile long*)value, old, old) != old);
      break;
    }
    case _Py_memory_order_relaxed:
      old = *value;
      break;
    default:
    {
      do {
        old = *value;
      } while(_InterlockedCompareExchange((volatile long*)value, old, old) != old);
      break;
    }
    }
    return old;
}

#define _Py_atomic_load_32bit(ATOMIC_VAL, ORDER) \
    _Py_atomic_load_32bit_impl((volatile int*)&((ATOMIC_VAL)->_value), (ORDER))

#define _Py_atomic_load_explicit(ATOMIC_VAL, ORDER) \
  ( \
    sizeof((ATOMIC_VAL)->_value) == 8 ? \
    _Py_atomic_load_64bit((ATOMIC_VAL), ORDER) : \
    _Py_atomic_load_32bit((ATOMIC_VAL), ORDER) \
  )
#define _Py_atomic_load_relaxed(ATOMIC_VAL) \
    _Py_atomic_load_explicit((ATOMIC_VAL), _Py_memory_order_relaxed)

typedef struct {
    PyObject_HEAD
    PyObject *first; /* first name */
    PyObject *last;  /* last name */
    int number;
} CustomObject;

typedef enum {
    DICT_KEYS_GENERAL = 0,
    DICT_KEYS_UNICODE = 1,
    DICT_KEYS_SPLIT = 2
} DictKeysKind;

struct _dictkeysobject {
    Py_ssize_t dk_refcnt;

    /* Size of the hash table (dk_indices). It must be a power of 2. */
    uint8_t dk_log2_size;

    /* Kind of keys */
    uint8_t dk_kind;

    /* Version number -- Reset to 0 by any modification to keys */
    uint32_t dk_version;

    /* Number of usable entries in dk_entries. */
    Py_ssize_t dk_usable;

    /* Number of used entries in dk_entries. */
    Py_ssize_t dk_nentries;

    /* Actual hash table of dk_size entries. It holds indices in dk_entries,
       or DKIX_EMPTY(-1) or DKIX_DUMMY(-2).

       Indices must be: 0 <= indice < USABLE_FRACTION(dk_size).

       The size in bytes of an indice depends on dk_size:

       - 1 byte if dk_size <= 0xff (char*)
       - 2 bytes if dk_size <= 0xffff (int16_t*)
       - 4 bytes if dk_size <= 0xffffffff (int32_t*)
       - 8 bytes otherwise (int64_t*)

       Dynamically sized, SIZEOF_VOID_P is minimum. */
    char dk_indices[];  /* char is required to avoid strict aliasing. */

    /* "PyDictKeyEntry dk_entries[dk_usable];" array follows:
       see the DK_ENTRIES() macro */
};

typedef struct _dictkeysobject PyDictKeysObject;

typedef struct {
    /* Cached hash code of me_key. */
    Py_hash_t me_hash;
    PyObject *me_key;
    PyObject *me_value; /* This field is only meaningful for combined tables */
} PyDictKeyEntry;

typedef struct _Py_atomic_address {
    uintptr_t _value;
} _Py_atomic_address;

typedef struct _Py_atomic_int {
    int _value;
} _Py_atomic_int;

typedef struct _PyCOND_T
{
    HANDLE sem;
    int waiting; /* to allow PyCOND_SIGNAL to be a no-op */
} PyCOND_T;

typedef SRWLOCK PyMUTEX_T;

struct _gil_runtime_state {
    /* microseconds (the Python API uses seconds, though) */
    unsigned long interval;
    /* Last PyThreadState holding / having held the GIL. This helps us
       know whether anyone else was scheduled after we dropped the GIL. */
    _Py_atomic_address last_holder;
    /* Whether the GIL is already taken (-1 if uninitialized). This is
       atomic because it can be read without any lock taken in ceval.c. */
    _Py_atomic_int locked;
    /* Number of GIL switches since the beginning. */
    unsigned long switch_number;
    /* This condition variable allows one or several threads to wait
       until the GIL is released. In addition, the mutex also protects
       the above variables. */
    PyCOND_T cond;
    PyMUTEX_T mutex;
#ifdef FORCE_SWITCHING
    /* This condition variable helps the GIL-releasing thread wait for
       a GIL-awaiting thread to be scheduled and take the GIL. */
    PyCOND_T switch_cond;
    PyMUTEX_T switch_mutex;
#endif
};

struct _ceval_runtime_state {
    /* Request for checking signals. It is shared by all interpreters (see
       bpo-40513). Any thread of any interpreter can receive a signal, but only
       the main thread of the main interpreter can handle signals: see
       _Py_ThreadCanHandleSignals(). */
    _Py_atomic_int signals_pending;
#ifndef EXPERIMENTAL_ISOLATED_SUBINTERPRETERS
    struct _gil_runtime_state gil;
#endif
};

struct _gilstate_runtime_state {
    /* bpo-26558: Flag to disable PyGILState_Check().
       If set to non-zero, PyGILState_Check() always return 1. */
    int check_enabled;
    /* Assuming the current thread holds the GIL, this is the
       PyThreadState for the current thread. */
    _Py_atomic_address tstate_current;
    /* The single PyInterpreterState used by this process'
       GILState implementation
    */
    /* TODO: Given interp_main, it may be possible to kill this ref */
    PyInterpreterState *autoInterpreterState;
    Py_tss_t autoTSSkey;
};

typedef struct _Py_AuditHookEntry {
    struct _Py_AuditHookEntry *next;
    Py_AuditHookFunction hookCFunction;
    void *userData;
} _Py_AuditHookEntry;

struct _Py_unicode_runtime_ids {
    PyThread_type_lock lock;
    // next_index value must be preserved when Py_Initialize()/Py_Finalize()
    // is called multiple times: see _PyUnicode_FromId() implementation.
    Py_ssize_t next_index;
};

typedef struct pyruntimestate {
    /* Is running Py_PreInitialize()? */
    int preinitializing;

    /* Is Python preinitialized? Set to 1 by Py_PreInitialize() */
    int preinitialized;

    /* Is Python core initialized? Set to 1 by _Py_InitializeCore() */
    int core_initialized;

    /* Is Python fully initialized? Set to 1 by Py_Initialize() */
    int initialized;

    /* Set by Py_FinalizeEx(). Only reset to NULL if Py_Initialize()
       is called again.

       Use _PyRuntimeState_GetFinalizing() and _PyRuntimeState_SetFinalizing()
       to access it, don't access it directly. */
    _Py_atomic_address _finalizing;

    struct pyinterpreters {
        PyThread_type_lock mutex;
        PyInterpreterState *head;
        PyInterpreterState *main;
        /* _next_interp_id is an auto-numbered sequence of small
           integers.  It gets initialized in _PyInterpreterState_Init(),
           which is called in Py_Initialize(), and used in
           PyInterpreterState_New().  A negative interpreter ID
           indicates an error occurred.  The main interpreter will
           always have an ID of 0.  Overflow results in a RuntimeError.
           If that becomes a problem later then we can adjust, e.g. by
           using a Python int. */
        int64_t next_id;
    } interpreters;
    // XXX Remove this field once we have a tp_* slot.
    struct _xidregistry {
        PyThread_type_lock mutex;
        struct _xidregitem *head;
    } xidregistry;

    unsigned long main_thread;

#define NEXITFUNCS 32
    void (*exitfuncs[NEXITFUNCS])(void);
    int nexitfuncs;

    struct _ceval_runtime_state ceval;
    struct _gilstate_runtime_state gilstate;

    PyPreConfig preconfig;

    // Audit values must be preserved when Py_Initialize()/Py_Finalize()
    // is called multiple times.
    Py_OpenCodeHookFunction open_code_hook;
    void *open_code_userdata;
    _Py_AuditHookEntry *audit_hook_head;

    struct _Py_unicode_runtime_ids unicode_ids;

    // XXX Consolidate globals found via the check-c-globals script.
} _PyRuntimeState;

#define DK_LOG_SIZE(dk)  ((dk)->dk_log2_size)
#define DK_SIZE(dk)      (1<<DK_LOG_SIZE(dk))
#define DK_IXSIZE(dk)                     \
    (DK_LOG_SIZE(dk) <= 7 ?               \
        1 : DK_LOG_SIZE(dk) <= 15 ?       \
            2 : DK_LOG_SIZE(dk) <= 31 ?   \
                4 : sizeof(int64_t))
#define DK_ENTRIES(dk) \
    ((PyDictKeyEntry*)(&((int8_t*)((dk)->dk_indices))[DK_SIZE(dk) * DK_IXSIZE(dk)]))

#define DK_MASK(dk) (DK_SIZE(dk)-1)
#define DKIX_EMPTY (-1)
#define DKIX_ERROR (-3)