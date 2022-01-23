#include <windows.h>
#include "stringlib/eq.h"

#define NUM_GENERATIONS 3
#define _PY_NSMALLNEGINTS           5
#define _PY_NSMALLPOSINTS           257
#define MCACHE_SIZE_EXP 12
#define PyDict_LOG_MINSIZE 3
#define PyDict_MINSIZE 8

/* Speed optimization to avoid frequent malloc/free of small tuples */
#ifndef PyTuple_MAXSAVESIZE
   // Largest tuple to save on free list
#  define PyTuple_MAXSAVESIZE 20
#endif

/* Empty list reuse scheme to save calls to malloc and free */
#ifndef PyList_MAXFREELIST
#  define PyList_MAXFREELIST 80
#endif

#ifndef PyDict_MAXFREELIST
#  define PyDict_MAXFREELIST 80
#endif

#ifndef _PyAsyncGen_MAXFREELIST
#  define _PyAsyncGen_MAXFREELIST 80
#endif

#define DKIX_DUMMY (-2)  /* Used internally */
#define DKIX_EMPTY (-1)
#define DKIX_ERROR (-3)

#define USABLE_FRACTION(n) (((n) << 1)/3)

#define PERTURB_SHIFT 5

// This undocumented flag gives certain built-ins their unique pattern-matching
// behavior, which allows a single positional subpattern to match against the
// subject itself (rather than a mapped attribute on it):
#define _Py_TPFLAGS_MATCH_SELF (1UL << 22)

/* Set if instances of the type object are treated as mappings for pattern matching */
#define Py_TPFLAGS_MAPPING (1 << 6)

typedef struct {
    /* Cached hash code of me_key. */
    Py_hash_t me_hash;
    PyObject *me_key;
    PyObject *me_value; /* This field is only meaningful for combined tables */
    size_t i; /* This entry's collision-free index in the hash table */
} PyDictKeyEntry;

typedef struct {
    // PyDictObject *mp;
    PyDictKeyEntry **keys;
    int used;
    int n;
} Layer;

/* The ma_values pointer is NULL for a combined table
 * or points to an array of PyObject* for a split table
 */
typedef struct {
    PyObject_HEAD

    /* Number of items in the primary layer */
    Py_ssize_t ma_used;

    /* Number of items in the dictionary */
    Py_ssize_t ma_num_items;

    /* Dictionary version: globally unique, value change each time
       the dictionary is modified */
    uint64_t ma_version_tag;

    PyDictKeysObject *ma_keys;

    /* If ma_values is NULL, the table is "combined": keys and values
       are stored in ma_keys.

       If ma_values is not NULL, the table is splitted:
       keys are stored in ma_keys and values are stored in ma_values */
    PyObject **ma_values;

    Layer* ma_layers;

    Py_ssize_t *ma_indices_stack;
    int ma_indices_stack_idx;

    Py_ssize_t *ma_indices_to_hashpos;

    PyDictKeyEntry *keys;
} CustomPyDictObject;

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

    // Layer* dk_layer;

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
};

int (*emptydictinsertion)(CustomPyDictObject *, PyObject *, Py_hash_t, PyObject *) = NULL;
Py_ssize_t (*lookup)(CustomPyDictObject *, PyObject *, Py_hash_t, PyObject **, int *) = NULL;
Py_ssize_t (*empty_slot)(PyDictKeysObject *, Py_hash_t, size_t *, int *) = NULL;
void (*build_idxs)(CustomPyDictObject *, PyDictKeyEntry *, Py_ssize_t) = NULL;
int (*insert)(CustomPyDictObject *, PyObject *, Py_hash_t, PyObject *) = NULL;
int (*resize)(CustomPyDictObject *, uint8_t) = NULL;

typedef enum {
    DICT_KEYS_GENERAL = 0,
    DICT_KEYS_UNICODE = 1,
    DICT_KEYS_SPLIT = 2
} DictKeysKind;

/* This immutable, empty PyDictKeysObject is used for PyDict_Clear()
 * (which cannot fail and thus can do no allocation).
 */
static PyDictKeysObject empty_keys_struct = {
        1, /* dk_refcnt */
        0, /* dk_log2_size */
        DICT_KEYS_SPLIT, /* dk_kind */
        1, /* dk_version */
        0, /* dk_usable (immutable) */
        0, /* dk_nentries */
        {DKIX_EMPTY, DKIX_EMPTY, DKIX_EMPTY, DKIX_EMPTY,
         DKIX_EMPTY, DKIX_EMPTY, DKIX_EMPTY, DKIX_EMPTY}, /* dk_indices */
};

#define Py_EMPTY_KEYS &empty_keys_struct

/*Global counter used to set ma_version_tag field of dictionary.
 * It is incremented each time that a dictionary is created and each
 * time that a dictionary is modified. */
uint64_t _pydict_global_version = 0;
#define DICT_NEXT_VERSION() (++_pydict_global_version)

#ifdef DEBUG_PYDICT
#  define ASSERT_CONSISTENT(op) assert(_PyDict_CheckConsistency((PyObject *)(op), 1))
#else
#  define ASSERT_CONSISTENT(op) assert(_PyDict_CheckConsistency((PyObject *)(op), 0))
#endif

static PyObject *empty_values[1] = { NULL };

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

PyDoc_STRVAR(dict___contains____doc__,
"__contains__($self, key, /)\n"
"--\n"
"\n"
"True if the dictionary has the specified key, else False.");
#define DICT___CONTAINS___METHODDEF    \
    {"__contains__", (PyCFunction)dict___contains__, METH_O|METH_COEXIST, dict___contains____doc__},
#define CUSTOM_DICT___CONTAINS___METHODDEF    \
    {"__contains__", (PyCFunction)custom_dict___contains__, METH_O|METH_COEXIST, dict___contains____doc__},

PyDoc_STRVAR(getitem__doc__, "x.__getitem__(y) <==> x[y]");

PyDoc_STRVAR(sizeof__doc__,
"D.__sizeof__() -> size of D in memory, in bytes");

#define DICT_GET_METHODDEF    \
    {"get", (PyCFunction)(void(*)(void))dict_get, METH_FASTCALL, dict_get__doc__},
#define CUSTOM_DICT_GET_METHODDEF    \
    {"get", (PyCFunction)(void(*)(void))custom_dict_get, METH_FASTCALL, dict_get__doc__},

PyDoc_STRVAR(dict_get__doc__,
"get($self, key, default=None, /)\n"
"--\n"
"\n"
"Return the value for key if key is in the dictionary, else default.");

#define DICT_SETDEFAULT_METHODDEF    \
    {"setdefault", (PyCFunction)(void(*)(void))dict_setdefault, METH_FASTCALL, dict_setdefault__doc__},
#define CUSTOM_DICT_SETDEFAULT_METHODDEF    \
    {"setdefault", (PyCFunction)(void(*)(void))custom_dict_setdefault, METH_FASTCALL, dict_setdefault__doc__},

PyDoc_STRVAR(dict_setdefault__doc__,
"setdefault($self, key, default=None, /)\n"
"--\n"
"\n"
"Insert key with a value of default if key is not in the dictionary.\n"
"\n"
"Return the value for key if key is in the dictionary, else default.");

#define DICT_POP_METHODDEF    \
    {"pop", (PyCFunction)(void(*)(void))dict_pop, METH_FASTCALL, dict_pop__doc__},
#define CUSTOM_DICT_POP_METHODDEF    \
    {"pop", (PyCFunction)(void(*)(void))custom_dict_pop, METH_FASTCALL, dict_pop__doc__},

PyDoc_STRVAR(dict_pop__doc__,
"pop($self, key, default=<unrepresentable>, /)\n"
"--\n"
"\n"
"D.pop(k[,d]) -> v, remove specified key and return the corresponding value.\n"
"\n"
"If the key is not found, return the default if given; otherwise,\n"
"raise a KeyError.");

#define DICT_POPITEM_METHODDEF    \
    {"popitem", (PyCFunction)dict_popitem, METH_NOARGS, dict_popitem__doc__},
#define CUSTOM_DICT_POPITEM_METHODDEF    \
    {"popitem", (PyCFunction)custom_dict_popitem, METH_NOARGS, dict_popitem__doc__},

PyDoc_STRVAR(dict_popitem__doc__,
"popitem($self, /)\n"
"--\n"
"\n"
"Remove and return a (key, value) pair as a 2-tuple.\n"
"\n"
"Pairs are returned in LIFO (last-in, first-out) order.\n"
"Raises KeyError if the dict is empty.");

PyDoc_STRVAR(keys__doc__,
             "D.keys() -> a set-like object providing a view on D's keys");
PyDoc_STRVAR(items__doc__,
             "D.items() -> a set-like object providing a view on D's items");
PyDoc_STRVAR(values__doc__,
             "D.values() -> an object providing a view on D's values");
PyDoc_STRVAR(update__doc__,
"D.update([E, ]**F) -> None.  Update D from dict/iterable E and F.\n\
If E is present and has a .keys() method, then does:  for k in E: D[k] = E[k]\n\
If E is present and lacks a .keys() method, then does:  for k, v in E: D[k] = v\n\
In either case, this is followed by: for k in F:  D[k] = F[k]");

#define DICT_FROMKEYS_METHODDEF    \
    {"fromkeys", (PyCFunction)(void(*)(void))dict_fromkeys, METH_FASTCALL|METH_CLASS, dict_fromkeys__doc__},

PyDoc_STRVAR(dict_fromkeys__doc__,
"fromkeys($type, iterable, value=None, /)\n"
"--\n"
"\n"
"Create a new dictionary with keys from iterable and values set to value.");
PyDoc_STRVAR(clear__doc__,
"D.clear() -> None.  Remove all items from D.");
PyDoc_STRVAR(copy__doc__,
"D.copy() -> a shallow copy of D");

#define DICT___REVERSED___METHODDEF    \
    {"__reversed__", (PyCFunction)dict___reversed__, METH_NOARGS, dict___reversed____doc__},

PyDoc_STRVAR(dict___reversed____doc__,
"__reversed__($self, /)\n"
"--\n"
"\n"
"Return a reverse iterator over the dict keys.");

#define _Py_AS_GC(o) ((PyGC_Head *)(o)-1)

#define _PyGCHead_SET_NEXT(g, p) ((g)->_gc_next = (uintptr_t)(p))

/* The (N-2) most significant bits contain the real address. */
#define _PyGC_PREV_SHIFT           (2)
#define _PyGC_PREV_MASK            (((uintptr_t) -1) << _PyGC_PREV_SHIFT)

#define _PyGCHead_SET_PREV(g, p) do { \
    assert(((uintptr_t)p & ~_PyGC_PREV_MASK) == 0); \
    (g)->_gc_prev = ((g)->_gc_prev & ~_PyGC_PREV_MASK) \
        | ((uintptr_t)(p)); \
    } while (0)

typedef struct {
    PyObject_HEAD
    CustomPyDictObject *di_dict; /* Set to NULL when iterator is exhausted */
    Py_ssize_t di_used;
    Py_ssize_t di_pos;
    PyObject* di_result; /* reusable result tuple for iteritems */
    Py_ssize_t len;
} customdictiterobject;

typedef struct {
    PyObject_HEAD
    PyDictObject *di_dict; /* Set to NULL when iterator is exhausted */
    Py_ssize_t di_used;
    Py_ssize_t di_pos;
    PyObject* di_result; /* reusable result tuple for iteritems */
    Py_ssize_t len;
} dictiterobject;

typedef struct _dictkeysobject PyDictKeysObject;

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

struct _pending_calls {
    PyThread_type_lock lock;
    /* Request for running pending calls. */
    _Py_atomic_int calls_to_do;
    /* Request for looking at the `async_exc` field of the current
       thread state.
       Guarded by the GIL. */
    int async_exc;
#define NPENDINGCALLS 32
    struct {
        int (*func)(void *);
        void *arg;
    } calls[NPENDINGCALLS];
    int first;
    int last;
};

struct _ceval_state {
    int recursion_limit;
    /* This single variable consolidates all requests to break out of
       the fast path in the eval loop. */
    _Py_atomic_int eval_breaker;
    /* Request for dropping the GIL */
    _Py_atomic_int gil_drop_request;
    struct _pending_calls pending;
#ifdef EXPERIMENTAL_ISOLATED_SUBINTERPRETERS
    struct _gil_runtime_state gil;
#endif
};

typedef struct {
    // Pointer to next object in the list.
    // 0 means the object is not tracked
    uintptr_t _gc_next;

    // Pointer to previous object in the list.
    // Lowest two bits are used for flags documented later.
    uintptr_t _gc_prev;
} PyGC_Head;

struct gc_generation {
    PyGC_Head head;
    int threshold; /* collection threshold */
    int count; /* count of allocations or collections of younger
                  generations */
};

struct gc_generation_stats {
    /* total number of collections */
    Py_ssize_t collections;
    /* total number of collected objects */
    Py_ssize_t collected;
    /* total number of uncollectable objects (put into gc.garbage) */
    Py_ssize_t uncollectable;
};

struct _gc_runtime_state {
    /* List of objects that still need to be cleaned up, singly linked
     * via their gc headers' gc_prev pointers.  */
    PyObject *trash_delete_later;
    /* Current call-stack depth of tp_dealloc calls. */
    int trash_delete_nesting;

    int enabled;
    int debug;
    /* linked lists of container objects */
    struct gc_generation generations[NUM_GENERATIONS];
    PyGC_Head *generation0;
    /* a permanent generation which won't be collected */
    struct gc_generation permanent_generation;
    struct gc_generation_stats generation_stats[NUM_GENERATIONS];
    /* true if we are currently running the collector */
    int collecting;
    /* list of uncollectable objects */
    PyObject *garbage;
    /* a list of callbacks to be invoked when collection is performed */
    PyObject *callbacks;
    /* This is the number of objects that survived the last full
       collection. It approximates the number of long lived objects
       tracked by the GC.

       (by "full collection", we mean a collection of the oldest
       generation). */
    Py_ssize_t long_lived_total;
    /* This is the number of objects that survived all "non-full"
       collections, and are awaiting to undergo a full collection for
       the first time. */
    Py_ssize_t long_lived_pending;
};

struct _warnings_runtime_state {
    /* Both 'filters' and 'onceregistry' can be set in warnings.py;
       get_warnings_attr() will reset these variables accordingly. */
    PyObject *filters;  /* List */
    PyObject *once_registry;  /* Dict */
    PyObject *default_action; /* String */
    long filters_version;
};

typedef struct {
    PyObject *func;
    PyObject *args;
    PyObject *kwargs;
} atexit_callback;

struct atexit_state {
    atexit_callback **callbacks;
    int ncallbacks;
    int callback_len;
};

struct _Py_bytes_state {
    PyObject *empty_string;
    PyBytesObject *characters[256];
};

struct _Py_unicode_fs_codec {
    char *encoding;   // Filesystem encoding (encoded to UTF-8)
    int utf8;         // encoding=="utf-8"?
    char *errors;     // Filesystem errors (encoded to UTF-8)
    _Py_error_handler error_handler;
};

struct _Py_unicode_ids {
    Py_ssize_t size;
    PyObject **array;
};

struct _Py_unicode_state {
    // The empty Unicode object is a singleton to improve performance.
    PyObject *empty_string;
    /* Single character Unicode strings in the Latin-1 range are being
       shared as well. */
    PyObject *latin1[256];
    struct _Py_unicode_fs_codec fs_codec;

    /* This dictionary holds all interned unicode strings.  Note that references
       to strings in this dictionary are *not* counted in the string's ob_refcnt.
       When the interned string reaches a refcnt of 0 the string deallocation
       function will delete the reference from this dictionary.

       Another way to look at this is that to say that the actual reference
       count of a string is:  s->ob_refcnt + (s->state ? 2 : 0)
    */
    PyObject *interned;

    // Unicode identifiers (_Py_Identifier): see _PyUnicode_FromId()
    struct _Py_unicode_ids ids;
};

struct _Py_float_state {
    /* Special free list
       free_list is a singly-linked list of available PyFloatObjects,
       linked via abuse of their ob_type members. */
    int numfree;
    PyFloatObject *free_list;
};

struct _Py_tuple_state {
#if PyTuple_MAXSAVESIZE > 0
    /* Entries 1 up to PyTuple_MAXSAVESIZE are free lists,
       entry 0 is the empty tuple () of which at most one instance
       will be allocated. */
    PyTupleObject *free_list[PyTuple_MAXSAVESIZE];
    int numfree[PyTuple_MAXSAVESIZE];
#endif
};

struct _Py_list_state {
    PyListObject *free_list[PyList_MAXFREELIST];
    int numfree;
};

struct _Custom_Py_dict_state {
    /* Dictionary reuse scheme to save calls to malloc and free */
    CustomPyDictObject *free_list[PyDict_MAXFREELIST];
    int numfree;
    PyDictKeysObject *keys_free_list[PyDict_MAXFREELIST];
    int keys_numfree;
};

struct _Py_dict_state {
    /* Dictionary reuse scheme to save calls to malloc and free */
    PyDictObject *free_list[PyDict_MAXFREELIST];
    int numfree;
    PyDictKeysObject *keys_free_list[PyDict_MAXFREELIST];
    int keys_numfree;
};

struct _Py_dict_state *dstate;

struct _Py_frame_state {
    PyFrameObject *free_list;
    /* number of frames currently in free_list */
    int numfree;
};

struct _Py_async_gen_state {
    /* Freelists boost performance 6-10%; they also reduce memory
       fragmentation, as _PyAsyncGenWrappedValue and PyAsyncGenASend
       are short-living objects that are instantiated for every
       __anext__() call. */
    struct _PyAsyncGenWrappedValue* value_freelist[_PyAsyncGen_MAXFREELIST];
    int value_numfree;

    struct PyAsyncGenASend* asend_freelist[_PyAsyncGen_MAXFREELIST];
    int asend_numfree;
};

struct _Py_context_state {
    // List of free PyContext objects
    PyContext *freelist;
    int numfree;
};

struct _Py_exc_state {
    // The dict mapping from errno codes to OSError subclasses
    PyObject *errnomap;
    PyBaseExceptionObject *memerrors_freelist;
    int memerrors_numfree;
};

struct ast_state {
    int initialized;
    PyObject *AST_type;
    PyObject *Add_singleton;
    PyObject *Add_type;
    PyObject *And_singleton;
    PyObject *And_type;
    PyObject *AnnAssign_type;
    PyObject *Assert_type;
    PyObject *Assign_type;
    PyObject *AsyncFor_type;
    PyObject *AsyncFunctionDef_type;
    PyObject *AsyncWith_type;
    PyObject *Attribute_type;
    PyObject *AugAssign_type;
    PyObject *Await_type;
    PyObject *BinOp_type;
    PyObject *BitAnd_singleton;
    PyObject *BitAnd_type;
    PyObject *BitOr_singleton;
    PyObject *BitOr_type;
    PyObject *BitXor_singleton;
    PyObject *BitXor_type;
    PyObject *BoolOp_type;
    PyObject *Break_type;
    PyObject *Call_type;
    PyObject *ClassDef_type;
    PyObject *Compare_type;
    PyObject *Constant_type;
    PyObject *Continue_type;
    PyObject *Del_singleton;
    PyObject *Del_type;
    PyObject *Delete_type;
    PyObject *DictComp_type;
    PyObject *Dict_type;
    PyObject *Div_singleton;
    PyObject *Div_type;
    PyObject *Eq_singleton;
    PyObject *Eq_type;
    PyObject *ExceptHandler_type;
    PyObject *Expr_type;
    PyObject *Expression_type;
    PyObject *FloorDiv_singleton;
    PyObject *FloorDiv_type;
    PyObject *For_type;
    PyObject *FormattedValue_type;
    PyObject *FunctionDef_type;
    PyObject *FunctionType_type;
    PyObject *GeneratorExp_type;
    PyObject *Global_type;
    PyObject *GtE_singleton;
    PyObject *GtE_type;
    PyObject *Gt_singleton;
    PyObject *Gt_type;
    PyObject *IfExp_type;
    PyObject *If_type;
    PyObject *ImportFrom_type;
    PyObject *Import_type;
    PyObject *In_singleton;
    PyObject *In_type;
    PyObject *Interactive_type;
    PyObject *Invert_singleton;
    PyObject *Invert_type;
    PyObject *IsNot_singleton;
    PyObject *IsNot_type;
    PyObject *Is_singleton;
    PyObject *Is_type;
    PyObject *JoinedStr_type;
    PyObject *LShift_singleton;
    PyObject *LShift_type;
    PyObject *Lambda_type;
    PyObject *ListComp_type;
    PyObject *List_type;
    PyObject *Load_singleton;
    PyObject *Load_type;
    PyObject *LtE_singleton;
    PyObject *LtE_type;
    PyObject *Lt_singleton;
    PyObject *Lt_type;
    PyObject *MatMult_singleton;
    PyObject *MatMult_type;
    PyObject *MatchAs_type;
    PyObject *MatchClass_type;
    PyObject *MatchMapping_type;
    PyObject *MatchOr_type;
    PyObject *MatchSequence_type;
    PyObject *MatchSingleton_type;
    PyObject *MatchStar_type;
    PyObject *MatchValue_type;
    PyObject *Match_type;
    PyObject *Mod_singleton;
    PyObject *Mod_type;
    PyObject *Module_type;
    PyObject *Mult_singleton;
    PyObject *Mult_type;
    PyObject *Name_type;
    PyObject *NamedExpr_type;
    PyObject *Nonlocal_type;
    PyObject *NotEq_singleton;
    PyObject *NotEq_type;
    PyObject *NotIn_singleton;
    PyObject *NotIn_type;
    PyObject *Not_singleton;
    PyObject *Not_type;
    PyObject *Or_singleton;
    PyObject *Or_type;
    PyObject *Pass_type;
    PyObject *Pow_singleton;
    PyObject *Pow_type;
    PyObject *RShift_singleton;
    PyObject *RShift_type;
    PyObject *Raise_type;
    PyObject *Return_type;
    PyObject *SetComp_type;
    PyObject *Set_type;
    PyObject *Slice_type;
    PyObject *Starred_type;
    PyObject *Store_singleton;
    PyObject *Store_type;
    PyObject *Sub_singleton;
    PyObject *Sub_type;
    PyObject *Subscript_type;
    PyObject *Try_type;
    PyObject *Tuple_type;
    PyObject *TypeIgnore_type;
    PyObject *UAdd_singleton;
    PyObject *UAdd_type;
    PyObject *USub_singleton;
    PyObject *USub_type;
    PyObject *UnaryOp_type;
    PyObject *While_type;
    PyObject *With_type;
    PyObject *YieldFrom_type;
    PyObject *Yield_type;
    PyObject *__dict__;
    PyObject *__doc__;
    PyObject *__match_args__;
    PyObject *__module__;
    PyObject *_attributes;
    PyObject *_fields;
    PyObject *alias_type;
    PyObject *annotation;
    PyObject *arg;
    PyObject *arg_type;
    PyObject *args;
    PyObject *argtypes;
    PyObject *arguments_type;
    PyObject *asname;
    PyObject *ast;
    PyObject *attr;
    PyObject *bases;
    PyObject *body;
    PyObject *boolop_type;
    PyObject *cases;
    PyObject *cause;
    PyObject *cls;
    PyObject *cmpop_type;
    PyObject *col_offset;
    PyObject *comparators;
    PyObject *comprehension_type;
    PyObject *context_expr;
    PyObject *conversion;
    PyObject *ctx;
    PyObject *decorator_list;
    PyObject *defaults;
    PyObject *elt;
    PyObject *elts;
    PyObject *end_col_offset;
    PyObject *end_lineno;
    PyObject *exc;
    PyObject *excepthandler_type;
    PyObject *expr_context_type;
    PyObject *expr_type;
    PyObject *finalbody;
    PyObject *format_spec;
    PyObject *func;
    PyObject *generators;
    PyObject *guard;
    PyObject *handlers;
    PyObject *id;
    PyObject *ifs;
    PyObject *is_async;
    PyObject *items;
    PyObject *iter;
    PyObject *key;
    PyObject *keys;
    PyObject *keyword_type;
    PyObject *keywords;
    PyObject *kind;
    PyObject *kw_defaults;
    PyObject *kwarg;
    PyObject *kwd_attrs;
    PyObject *kwd_patterns;
    PyObject *kwonlyargs;
    PyObject *left;
    PyObject *level;
    PyObject *lineno;
    PyObject *lower;
    PyObject *match_case_type;
    PyObject *mod_type;
    PyObject *module;
    PyObject *msg;
    PyObject *name;
    PyObject *names;
    PyObject *op;
    PyObject *operand;
    PyObject *operator_type;
    PyObject *ops;
    PyObject *optional_vars;
    PyObject *orelse;
    PyObject *pattern;
    PyObject *pattern_type;
    PyObject *patterns;
    PyObject *posonlyargs;
    PyObject *rest;
    PyObject *returns;
    PyObject *right;
    PyObject *simple;
    PyObject *slice;
    PyObject *step;
    PyObject *stmt_type;
    PyObject *subject;
    PyObject *tag;
    PyObject *target;
    PyObject *targets;
    PyObject *test;
    PyObject *type;
    PyObject *type_comment;
    PyObject *type_ignore_type;
    PyObject *type_ignores;
    PyObject *unaryop_type;
    PyObject *upper;
    PyObject *value;
    PyObject *values;
    PyObject *vararg;
    PyObject *withitem_type;
};

// Type attribute lookup cache: speed up attribute and method lookups,
// see _PyType_Lookup().
struct type_cache_entry {
    unsigned int version;  // initialized from type->tp_version_tag
    PyObject *name;        // reference to exactly a str or None
    PyObject *value;       // borrowed reference or NULL
};

struct type_cache {
    struct type_cache_entry hashtable[1 << MCACHE_SIZE_EXP];
#if MCACHE_STATS
    size_t hits;
    size_t misses;
    size_t collisions;
#endif
};

// The PyInterpreterState typedef is in Include/pystate.h.
struct _is {

    struct _is *next;
    struct _ts *tstate_head;

    /* Reference to the _PyRuntime global variable. This field exists
       to not have to pass runtime in addition to tstate to a function.
       Get runtime from tstate: tstate->interp->runtime. */
    struct pyruntimestate *runtime;

    int64_t id;
    int64_t id_refcount;
    int requires_idref;
    PyThread_type_lock id_mutex;

    int finalizing;

    struct _ceval_state ceval;
    struct _gc_runtime_state gc;

    // sys.modules dictionary
    PyObject *modules;
    PyObject *modules_by_index;
    // Dictionary of the sys module
    PyObject *sysdict;
    // Dictionary of the builtins module
    PyObject *builtins;
    // importlib module
    PyObject *importlib;

    /* Used in Modules/_threadmodule.c. */
    long num_threads;
    /* Support for runtime thread stack size tuning.
       A value of 0 means using the platform's default stack size
       or the size specified by the THREAD_STACK_SIZE macro. */
    /* Used in Python/thread.c. */
    size_t pythread_stacksize;

    PyObject *codec_search_path;
    PyObject *codec_search_cache;
    PyObject *codec_error_registry;
    int codecs_initialized;

    PyConfig config;
#ifdef HAVE_DLOPEN
    int dlopenflags;
#endif

    PyObject *dict;  /* Stores per-interpreter state */

    PyObject *builtins_copy;
    PyObject *import_func;
    // Initialized to _PyEval_EvalFrameDefault().
    _PyFrameEvalFunction eval_frame;

    Py_ssize_t co_extra_user_count;
    freefunc co_extra_freefuncs[MAX_CO_EXTRA_USERS];

#ifdef HAVE_FORK
    PyObject *before_forkers;
    PyObject *after_forkers_parent;
    PyObject *after_forkers_child;
#endif

    uint64_t tstate_next_unique_id;

    struct _warnings_runtime_state warnings;
    struct atexit_state atexit;

    PyObject *audit_hooks;

    /* Small integers are preallocated in this array so that they
       can be shared.
       The integers that are preallocated are those in the range
       -_PY_NSMALLNEGINTS (inclusive) to _PY_NSMALLPOSINTS (not inclusive).
    */
    PyLongObject* small_ints[_PY_NSMALLNEGINTS + _PY_NSMALLPOSINTS];
    struct _Py_bytes_state bytes;
    struct _Py_unicode_state unicode;
    struct _Py_float_state float_state;
    /* Using a cache is very effective since typically only a single slice is
       created and then deleted again. */
    PySliceObject *slice_cache;

    struct _Py_tuple_state tuple;
    struct _Py_list_state list;
    struct _Py_dict_state dict_state;
    struct _Py_frame_state frame;
    struct _Py_async_gen_state async_gen;
    struct _Py_context_state context;
    struct _Py_exc_state exc_state;

    struct ast_state ast;
    struct type_cache type_cache;
};

/* struct _is is defined in internal/pycore_interp.h */
typedef struct _is PyInterpreterState;

#define _PyUnicode_HASH(op)                             \
    (((PyASCIIObject *)(op))->hash)

static inline PyThreadState*
_PyRuntimeState_GetThreadState(_PyRuntimeState *runtime)
{
#ifdef EXPERIMENTAL_ISOLATED_SUBINTERPRETERS
    return _PyThreadState_GetTSS();
#else
    return (PyThreadState*)_Py_atomic_load_relaxed(&runtime->gilstate.tstate_current);
#endif
}

PyAPI_DATA(_PyRuntimeState) _PyRuntime;

/* Get the current Python thread state.

   Efficient macro reading directly the 'gilstate.tstate_current' atomic
   variable. The macro is unsafe: it does not check for error and it can
   return NULL.

   The caller must hold the GIL.

   See also PyThreadState_Get() and PyThreadState_GET(). */
static inline PyThreadState*
_PyThreadState_GET(void)
{
#ifdef EXPERIMENTAL_ISOLATED_SUBINTERPRETERS
    return _PyThreadState_GetTSS();
#else
    return _PyRuntimeState_GetThreadState(&_PyRuntime);
#endif
}

/* Get the current interpreter state.

   The macro is unsafe: it does not check for error and it can return NULL.

   The caller must hold the GIL.

   See also _PyInterpreterState_Get()
   and _PyGILState_GetInterpreterStateUnsafe(). */
static inline PyInterpreterState* _PyInterpreterState_GET(void) {
    PyThreadState *tstate = _PyThreadState_GET();

#ifdef Py_DEBUG
    _Py_EnsureTstateNotNULL(tstate);
#endif
    printf("_PyInterpreterState_GET returning\n");
    return tstate->interp;
}

static inline void _PyObject_GC_TRACK(
// The preprocessor removes _PyObject_ASSERT_FROM() calls if NDEBUG is defined
#ifndef NDEBUG
    const char *filename, int lineno,
#endif
    PyObject *op)
{
    _PyObject_ASSERT_FROM(op, !_PyObject_GC_IS_TRACKED(op),
                          "object already tracked by the garbage collector",
                          filename, lineno, __func__);

    PyGC_Head *gc = _Py_AS_GC(op);
    _PyObject_ASSERT_FROM(op,
                          (gc->_gc_prev & _PyGC_PREV_MASK_COLLECTING) == 0,
                          "object is in generation which is garbage collected",
                          filename, lineno, __func__);

    PyInterpreterState *interp = _PyInterpreterState_GET();
    PyGC_Head *generation0 = interp->gc.generation0;
    PyGC_Head *last = (PyGC_Head*)(generation0->_gc_prev);
    _PyGCHead_SET_NEXT(last, gc);
    _PyGCHead_SET_PREV(gc, last);
    _PyGCHead_SET_NEXT(gc, generation0);
    generation0->_gc_prev = (uintptr_t)gc;
}

// Macros to accept any type for the parameter, and to automatically pass
// the filename and the filename (if NDEBUG is not defined) where the macro
// is called.
#ifdef NDEBUG
#  define _PyObject_GC_TRACK(op) \
        _PyObject_GC_TRACK(_PyObject_CAST(op))
#  define _PyObject_GC_UNTRACK(op) \
        _PyObject_GC_UNTRACK(_PyObject_CAST(op))
#else
#  define _PyObject_GC_TRACK(op) \
        _PyObject_GC_TRACK(__FILE__, __LINE__, _PyObject_CAST(op))
#  define _PyObject_GC_UNTRACK(op) \
        _PyObject_GC_UNTRACK(__FILE__, __LINE__, _PyObject_CAST(op))
#endif

static PyObject *
custom_dictiter_new(CustomPyDictObject *dict, PyTypeObject *itertype)
{
    customdictiterobject *di;
    di = PyObject_GC_New(customdictiterobject, itertype);
    if (di == NULL) {
        return NULL;
    }
    Py_INCREF(dict);
    di->di_dict = dict;
    di->di_used = dict->ma_used;
    di->len = dict->ma_used;
    if (itertype == &PyDictRevIterKey_Type ||
         itertype == &PyDictRevIterItem_Type ||
         itertype == &PyDictRevIterValue_Type) {
        if (dict->ma_values) {
            di->di_pos = dict->ma_used - 1;
        }
        else {
            di->di_pos = dict->ma_keys->dk_nentries - 1;
        }
    }
    else {
        di->di_pos = 0;
    }
    if (itertype == &PyDictIterItem_Type ||
        itertype == &PyDictRevIterItem_Type) {
        di->di_result = PyTuple_Pack(2, Py_None, Py_None);
        if (di->di_result == NULL) {
            Py_DECREF(di);
            return NULL;
        }
    }
    else {
        di->di_result = NULL;
    }
    _PyObject_GC_TRACK(di);
    return (PyObject *)di;
}

static PyObject *
dictiter_new(PyDictObject *dict, PyTypeObject *itertype)
{
    dictiterobject *di;
    di = PyObject_GC_New(dictiterobject, itertype);
    if (di == NULL) {
        return NULL;
    }
    Py_INCREF(dict);
    di->di_dict = dict;
    di->di_used = dict->ma_used;
    di->len = dict->ma_used;
    if (itertype == &PyDictRevIterKey_Type ||
         itertype == &PyDictRevIterItem_Type ||
         itertype == &PyDictRevIterValue_Type) {
        if (dict->ma_values) {
            di->di_pos = dict->ma_used - 1;
        }
        else {
            di->di_pos = dict->ma_keys->dk_nentries - 1;
        }
    }
    else {
        di->di_pos = 0;
    }
    if (itertype == &PyDictIterItem_Type ||
        itertype == &PyDictRevIterItem_Type) {
        di->di_result = PyTuple_Pack(2, Py_None, Py_None);
        if (di->di_result == NULL) {
            Py_DECREF(di);
            return NULL;
        }
    }
    else {
        di->di_result = NULL;
    }
    _PyObject_GC_TRACK(di);
    return (PyObject *)di;
}

static PyObject *
custom_dict_iter(CustomPyDictObject *dict)
{
    return custom_dictiter_new(dict, &PyDictIterKey_Type);
}

static PyObject *
dict_iter(PyDictObject *dict)
{
    return dictiter_new(dict, &PyDictIterKey_Type);
}

#define DK_LOG_SIZE(dk)  ((dk)->dk_log2_size)
#define DK_SIZE(dk)      (((int64_t)1)<<DK_LOG_SIZE(dk))
#define DK_IXSIZE(dk)                     \
    (DK_LOG_SIZE(dk) <= 7 ?               \
        1 : DK_LOG_SIZE(dk) <= 15 ?       \
            2 : DK_LOG_SIZE(dk) <= 31 ?   \
                4 : sizeof(int64_t))
#define DK_ENTRIES(dk) \
    ((PyDictKeyEntry*)(&((int8_t*)((dk)->dk_indices))[DK_SIZE(dk) * DK_IXSIZE(dk)]))
#define DK_MASK(dk) (DK_SIZE(dk)-1)

Py_ssize_t
_PyDict_KeysSize(PyDictKeysObject *keys)
{
    return (sizeof(PyDictKeysObject)
            + DK_IXSIZE(keys) * DK_SIZE(keys)
            + USABLE_FRACTION(DK_SIZE(keys)) * sizeof(PyDictKeyEntry));
}

static PyDictKeysObject *
custom_clone_combined_dict_keys(CustomPyDictObject *orig)
{
    assert(PyDict_Check(orig));
    assert(Py_TYPE(orig)->tp_iter == (getiterfunc)dict_iter);
    assert(orig->ma_values == NULL);
    assert(orig->ma_keys->dk_refcnt == 1);

    Py_ssize_t keys_size = _PyDict_KeysSize(orig->ma_keys);
    PyDictKeysObject *keys = PyObject_Malloc(keys_size);
    if (keys == NULL) {
        PyErr_NoMemory();
        return NULL;
    }

    memcpy(keys, orig->ma_keys, keys_size);

    /* After copying key/value pairs, we need to incref all
       keys and values and they are about to be co-owned by a
       new dict object. */
    PyDictKeyEntry *ep0 = DK_ENTRIES(keys);
    Py_ssize_t n = keys->dk_nentries;
    for (Py_ssize_t i = 0; i < n; i++) {
        PyDictKeyEntry *entry = &ep0[i];
        PyObject *value = entry->me_value;
        if (value != NULL) {
            Py_INCREF(value);
            Py_INCREF(entry->me_key);
        }
    }

    /* Since we copied the keys table we now have an extra reference
       in the system.  Manually call increment _Py_RefTotal to signal that
       we have it now; calling dictkeys_incref would be an error as
       keys->dk_refcnt is already set to 1 (after memcpy). */
#ifdef Py_REF_DEBUG
    _Py_RefTotal++;
#endif
    return keys;
}

static PyDictKeysObject *
clone_combined_dict_keys(CustomPyDictObject *orig)
{
    assert(PyDict_Check(orig));
    assert(Py_TYPE(orig)->tp_iter == (getiterfunc)dict_iter);
    assert(orig->ma_values == NULL);
    assert(orig->ma_keys->dk_refcnt == 1);

    Py_ssize_t keys_size = _PyDict_KeysSize(orig->ma_keys);
    PyDictKeysObject *keys = PyObject_Malloc(keys_size);
    if (keys == NULL) {
        PyErr_NoMemory();
        return NULL;
    }

    memcpy(keys, orig->ma_keys, keys_size);

    /* After copying key/value pairs, we need to incref all
       keys and values and they are about to be co-owned by a
       new dict object. */
    PyDictKeyEntry *ep0 = DK_ENTRIES(keys);
    Py_ssize_t n = keys->dk_nentries;
    for (Py_ssize_t i = 0; i < n; i++) {
        PyDictKeyEntry *entry = &ep0[i];
        PyObject *value = entry->me_value;
        if (value != NULL) {
            Py_INCREF(value);
            Py_INCREF(entry->me_key);
        }
    }

    /* Since we copied the keys table we now have an extra reference
       in the system.  Manually call increment _Py_RefTotal to signal that
       we have it now; calling dictkeys_incref would be an error as
       keys->dk_refcnt is already set to 1 (after memcpy). */
#ifdef Py_REF_DEBUG
    _Py_RefTotal++;
#endif
    return keys;
}

static struct _Py_dict_state *
get_dict_state(void)
{
#ifdef EBUG
    printf("called get_dict_state\n");
#endif
    // PyInterpreterState *interp = _PyInterpreterState_GET();

    if (dstate == NULL) {
        dstate = malloc(sizeof *dstate);
        dstate->keys_numfree = 0;
        assert(dstate);
    }

    // return &interp->dict_state;

    return dstate;
}

static void
free_keys_object(PyDictKeysObject *keys)
{
    PyDictKeyEntry *entries = DK_ENTRIES(keys);
    Py_ssize_t i, n;
    for (i = 0, n = keys->dk_nentries; i < n; i++) {
        Py_XDECREF(entries[i].me_key);
        Py_XDECREF(entries[i].me_value);
    }
    struct _Py_dict_state *state = get_dict_state();
#ifdef Py_DEBUG
    // free_keys_object() must not be called after _PyDict_Fini()
    assert(state->keys_numfree != -1);
#endif
    if (DK_SIZE(keys) == PyDict_MINSIZE && state->keys_numfree < PyDict_MAXFREELIST) {
        state->keys_free_list[state->keys_numfree++] = keys;
        return;
    }
    PyObject_Free(keys);
}

static inline void
dictkeys_decref(PyDictKeysObject *dk)
{
    assert(dk->dk_refcnt > 0);
#ifdef Py_REF_DEBUG
    _Py_RefTotal--;
#endif
    if (--dk->dk_refcnt == 0) {
        free_keys_object(dk);
    }
}

#define free_values(values) PyMem_Free(values)

/* True if the object is currently tracked by the GC. */
#define _PyObject_GC_IS_TRACKED(o) (_Py_AS_GC(o)->_gc_next != 0)

static PyDictKeysObject*
new_keys_object(uint8_t log2_size)
{
    PyDictKeysObject *dk;
    Py_ssize_t es, usable;

    assert(log2_size >= PyDict_LOG_MINSIZE);

    usable = USABLE_FRACTION(1<<log2_size);

#ifdef EBUG
    printf("usable: %lld.\n", usable);
#endif

    if (log2_size <= 7) {
        es = 1;
    }
    else if (log2_size <= 15) {
        es = 2;
    }
#if SIZEOF_VOID_P > 4
    else if (log2_size <= 31) {
        es = 4;
    }
#endif
    else {
        es = sizeof(Py_ssize_t);
    }

#ifdef EBUG
    printf("es: %zd\n", es);
    fflush(stdout);
#endif

    struct _Py_dict_state *state = get_dict_state();
#ifdef Py_DEBUG
    // new_keys_object() must not be called after _PyDict_Fini()
    assert(state->keys_numfree != -1);
#endif
    if (log2_size == PyDict_LOG_MINSIZE && state->keys_numfree > 0) {
        printf("state->keys_numfree: %d\n", state->keys_numfree);
        dk = state->keys_free_list[--state->keys_numfree];
    }
    else
    {
#ifdef EBUG
        printf("es<<log2_size: %lld.\n", (es<<log2_size));
        fflush(stdout);
#endif

        dk = PyObject_Malloc(sizeof(PyDictKeysObject)
                             + (es<<log2_size)
                             + sizeof(PyDictKeyEntry) * usable);
        if (dk == NULL) {
            PyErr_NoMemory();
            return NULL;
        }
    }
#ifdef Py_REF_DEBUG
    _Py_RefTotal++;
#endif
    dk->dk_refcnt = 1;
    dk->dk_log2_size = log2_size;
    dk->dk_usable = usable;
    dk->dk_kind = DICT_KEYS_UNICODE;
    dk->dk_nentries = 0;
    dk->dk_version = 0;
    memset(&dk->dk_indices[0], 0xff, es<<log2_size);
    memset(DK_ENTRIES(dk), 0, sizeof(PyDictKeyEntry) * usable);

    return dk;
}

/* lookup indices.  returns DKIX_EMPTY, DKIX_DUMMY, or ix >=0 */
static inline Py_ssize_t
dictkeys_get_index(const PyDictKeysObject *keys, Py_ssize_t i)
{
    Py_ssize_t s = DK_SIZE(keys);
    Py_ssize_t ix;

    if (s <= 0xff) {
        const int8_t *indices = (const int8_t*)(keys->dk_indices);
        ix = indices[i];
    }
    else if (s <= 0xffff) {
        const int16_t *indices = (const int16_t*)(keys->dk_indices);
        ix = indices[i];
    }
#if SIZEOF_VOID_P > 4
    else if (s > 0xffffffff) {
        const int64_t *indices = (const int64_t*)(keys->dk_indices);
        ix = indices[i];
    }
#endif
    else {
        const int32_t *indices = (const int32_t*)(keys->dk_indices);
        ix = indices[i];
    }
    assert(ix >= DKIX_DUMMY);

    return ix;
}

/* write to indices. */
static inline void
dictkeys_set_index(PyDictKeysObject *keys, Py_ssize_t i, Py_ssize_t ix)
{
    Py_ssize_t s = DK_SIZE(keys);

    assert(ix >= DKIX_DUMMY);
    assert(keys->dk_version == 0);

    if (s <= 0xff) {
        int8_t *indices = (int8_t*)(keys->dk_indices);
        assert(ix <= 0x7f);
        indices[i] = (char)ix;

#ifdef EBUG
        printf("dictkeys_set_index indices[%lld]: %d\n", i, indices[i]);
        fflush(stdout);
#endif
    }
    else if (s <= 0xffff) {
        int16_t *indices = (int16_t*)(keys->dk_indices);
        assert(ix <= 0x7fff);
        indices[i] = (int16_t)ix;

#ifdef EBUG
        printf("dictkeys_set_index indices[%lld]: %d\n", i, indices[i]);
        fflush(stdout);
#endif
    }
#if SIZEOF_VOID_P > 4
    else if (s > 0xffffffff) {
        int64_t *indices = (int64_t*)(keys->dk_indices);
        indices[i] = ix;
    }
#endif
    else {
        int32_t *indices = (int32_t*)(keys->dk_indices);
        assert(ix <= 0x7fffffff);
        indices[i] = (int32_t)ix;
    }
}

/*
Internal routine used by dictresize() to build a hashtable of entries.
*/
static void
build_indices(CustomPyDictObject *mp, PyDictKeyEntry *ep, Py_ssize_t n)
{
    PyDictKeysObject *keys = mp->ma_keys;
    size_t mask = (size_t)DK_SIZE(keys) - 1;
    for (Py_ssize_t ix = 0; ix != n; ix++, ep++) {
        Py_hash_t hash = ep->me_hash;
        size_t i = hash & mask;
        for (size_t perturb = hash; dictkeys_get_index(keys, i) != DKIX_EMPTY;) {
            perturb >>= PERTURB_SHIFT;
            i = mask & (i*5 + perturb + 1);
        }
        dictkeys_set_index(keys, i, ix);
    }
}

int
insertlayer_keyhashvalue(Layer *layer, PyObject *key, Py_hash_t hash, PyObject *value)
{
    if (layer->used >= layer->n) {
        int n = layer->n + layer->n;
        printf("doubling size of layer to %d.\n", n);

        layer->keys = realloc(layer->keys, n * sizeof *(layer->keys));
        if (!layer->keys) {
            return -1;
        }

        for (int i = layer->n; i < n; i++) {
            layer->keys[i] = NULL;
        }
        layer->n = n;
    }

    if (layer->keys[layer->used]) {
        printf("layer->keys[layer->used] not null???\n");
        return -1;
    }

    layer->keys[layer->used] = malloc(sizeof *layer->keys[layer->used]);
    if (!layer->keys[layer->used]) {
        return -1;
    }

    layer->keys[layer->used]->me_hash = hash;
    layer->keys[layer->used]->me_key = key;
    layer->keys[layer->used]->me_value = value;
    layer->keys[layer->used]->i = -1;
    layer->used++;
    return 0;
}

void
insertslot(CustomPyDictObject *mp, Py_ssize_t hashpos, PyDictKeyEntry *ep)
{
    Py_ssize_t idx = mp->ma_indices_stack[mp->ma_indices_stack_idx];
    mp->ma_indices_stack_idx--;

    dictkeys_set_index(mp->ma_keys, hashpos, idx);
    mp->ma_indices_to_hashpos[idx] = hashpos;

    PyDictKeyEntry *entry = &DK_ENTRIES(mp->ma_keys)[idx];
    entry->me_key = ep->me_key;
    entry->me_hash = ep->me_hash;
    if (mp->ma_values) {
        assert (mp->ma_values[mp->ma_keys->dk_nentries] == NULL);
        mp->ma_values[mp->ma_keys->dk_nentries] = ep->me_value;
    }
    else {
        entry->me_value = ep->me_value;
    }
    entry->i = ep->i;

    mp->ma_used++;
    mp->ma_version_tag = DICT_NEXT_VERSION();
    mp->ma_keys->dk_usable--;
    mp->ma_keys->dk_nentries++;
    assert(mp->ma_keys->dk_usable >= 0);
    ASSERT_CONSISTENT(mp);
}

// #define EBUG_FILTER
int
filter(CustomPyDictObject *mp, Py_ssize_t hashpos0, int num_cmps)
{
    PyDictKeysObject *dk = mp->ma_keys;
    size_t mask = DK_MASK(dk);
    PyDictKeyEntry *ep0 = DK_ENTRIES(dk);
    Layer *layer = &(mp->ma_layers[hashpos0]);

    if (!layer->keys) {
        layer->keys = malloc(PyDict_MINSIZE * sizeof *(layer->keys));
        if (!layer->keys) {
            return -1;
        }

        for (int i = 0; i < PyDict_MINSIZE; i++) {
            layer->keys[i] = NULL;
        }

        layer->n = PyDict_MINSIZE;
        layer->used = 0;
    }

    // move data, except for whatever's at hashpos0
    int num_items_moved = 0;
    for (int j = 1; j < num_cmps; j++) {
        size_t hashpos = (hashpos0 + j) & mask;
        Py_ssize_t jx = dictkeys_get_index(dk, hashpos);
        PyDictKeyEntry *ep = &ep0[jx];

        if (ep->i != hashpos0) {
            continue;
        }

#ifdef EBUG_FILTER
        printf("\tfilter move (%s, %lld).\n", PyUnicode_AsUTF8(ep->me_key), ep->i);
        fflush(stdout);
#endif

        // No collisions? Simply insert!
        if (dictkeys_get_index(mp->ma_keys, hashpos0) == DKIX_EMPTY) {
            // insertslot will determine entry.i
            insertslot(mp, hashpos0, ep);

            ep->me_key = NULL;
            ep->me_value = NULL;
            ep->i = -1;
            // dict_traverse2(mp, 1);
            return 0;
        }

        if (insertlayer_keyhashvalue(layer, ep->me_key, ep->me_hash, ep->me_value)) {
            printf("\tfilter error inserting %s into layer.\n", PyUnicode_AsUTF8(ep->me_key));
            fflush(stdout);
            return -1;
        }

#ifdef EBUG_FILTER
        printf("\tfilter get_index %lld: %lld\n", hashpos, dictkeys_get_index(mp->ma_keys, hashpos));
        printf("\tfilter set_index %lld, -1\n", hashpos);
        fflush(stdout);
#endif
        dictkeys_set_index(mp->ma_keys, hashpos, DKIX_EMPTY);

        mp->ma_indices_stack_idx++;
        mp->ma_indices_stack[mp->ma_indices_stack_idx] = jx;

        ep->me_key = NULL;
        ep->me_value = NULL;
        ep->i = -1;

        mp->ma_used--;
        mp->ma_keys->dk_usable++;
        mp->ma_keys->dk_nentries--;

        num_items_moved++;
    }

#ifdef EBUG_FILTER
    printf("\tfilter dk_nentries: %lld.\n", mp->ma_keys->dk_nentries);
    fflush(stdout);

    /* for (int i = 0; i < mp->ma_num_items; i++) {
        if (mp->ma_indices_to_hashpos[i] <= 0) continue;

        printf("%d %lld\n", i, mp->ma_indices_to_hashpos[i]);
        fflush(stdout);
    } */
#endif

    return num_items_moved;
}

// #define EBUG_BUILD_INDICES

void
collect_entries(CustomPyDictObject *mp, PyDictKeysObject *oldkeys, PyDictKeyEntry *newentries,
        Py_ssize_t *numentries)
{
    Py_ssize_t i = 0;
    PyDictKeyEntry *ep = DK_ENTRIES(oldkeys);
    *numentries = 0;

    while (i < DK_SIZE(oldkeys)) {
        Py_ssize_t ix = dictkeys_get_index(oldkeys, i);

        if (ix >= 0 && ep[ix].me_value) {
            newentries[*numentries] = ep[ix];
            (*numentries)++;
        }

        if (mp->ma_layers[i].keys) {
            for (int j = 0; j < mp->ma_layers[i].used; j++) {
                PyDictKeyEntry *layer_ep = mp->ma_layers[i].keys[j];
                newentries[*numentries] = *layer_ep;
                (*numentries)++;
            }
        }

        i++;
    }
}

int
layers_reinit(CustomPyDictObject *mp, PyDictKeysObject *oldkeys)
{
    if (!mp->ma_layers) {
        printf("layers_reinit ma_layers NULL???\n");
        fflush(stdout);
        return -1;
    }

    Py_ssize_t i = 0;
    while (i < DK_SIZE(oldkeys)) {
        if (mp->ma_layers[i].keys) {
            for (int j = 0; j < mp->ma_layers[i].n; j++) {
                // free(mp->ma_layers[i].keys[j]);
                mp->ma_layers[i].keys[j] = NULL;
            }

            free(mp->ma_layers[i].keys);
            mp->ma_layers[i].keys = NULL;
        }
        i++;
    }

    mp->ma_layers = realloc(mp->ma_layers, DK_SIZE(mp->ma_keys) * sizeof *(mp->ma_layers));
    if (mp->ma_layers == NULL) {
        printf("customdictresize ma_indices_to_hashpos realloc fail.\n");
        fflush(stdout);
        return -1;
    }

    for (Py_ssize_t i = 0; i < DK_SIZE(mp->ma_keys); i++) {
        mp->ma_layers[i].keys = NULL;

        mp->ma_layers[i].used = 0;
        mp->ma_layers[i].n = 0;
    }
 
    return 0;
}

// #define EBUG_RESIZE
/*
Restructure the table by allocating a new table and reinserting all
items again.  When entries have been deleted, the new table may
actually be smaller than the old one.
If a table is split (its keys and hashes are shared, its values are not),
then the values are temporarily copied into the table, it is resized as
a combined table, then the me_value slots in the old table are NULLed out.
After resizing a table is always combined,
but can be resplit by make_keys_shared().
*/
static int
customdictresize(CustomPyDictObject *mp, uint8_t log2_newsize,
        Py_ssize_t (*lookup)(CustomPyDictObject *, PyObject *, Py_hash_t, PyObject **, int *),
        Py_ssize_t (*empty_slot)(PyDictKeysObject *, Py_hash_t, size_t *, int *),
        void (*build_idxs)(CustomPyDictObject *, PyDictKeyEntry *, Py_ssize_t))
{
    printf("customdictresize log2_newsize: %ld.\n", log2_newsize);
    fflush(stdout);
#ifdef EBUG
#endif

    Py_ssize_t numentries;
    PyDictKeysObject *oldkeys;
    PyObject **oldvalues;
    PyDictKeyEntry *oldentries, *newentries;

    if (log2_newsize >= SIZEOF_SIZE_T*8) {
        PyErr_NoMemory();
        return -1;
    }
    assert(log2_newsize >= PyDict_LOG_MINSIZE);

    oldkeys = mp->ma_keys;

    /* NOTE: Current odict checks mp->ma_keys to detect resize happen.
     * So we can't reuse oldkeys even if oldkeys->dk_size == newsize.
     * TODO: Try reusing oldkeys when reimplement odict.
     */

    /* Allocate a new table. */
    mp->ma_keys = new_keys_object(log2_newsize);
    if (mp->ma_keys == NULL) {
        mp->ma_keys = oldkeys;
        return -1;
    }

    mp->ma_indices_stack = realloc(mp->ma_indices_stack, DK_SIZE(mp->ma_keys) * sizeof *(mp->ma_indices_stack));
    if (mp->ma_indices_to_hashpos == NULL) {
        printf("customdictresize ma_indices_stack realloc fail.\n");
        fflush(stdout);
        return -1;
    }

    for (int i = 0; i < DK_SIZE(mp->ma_keys); i++)
        mp->ma_indices_stack[i] = DK_SIZE(mp->ma_keys) - 1 - i;
    mp->ma_indices_stack_idx = DK_SIZE(mp->ma_keys) - 1;

    mp->ma_indices_to_hashpos = realloc(mp->ma_indices_to_hashpos, DK_SIZE(mp->ma_keys) * sizeof *(mp->ma_indices_to_hashpos));
    if (mp->ma_indices_to_hashpos == NULL) {
        printf("customdictresize ma_indices_to_hashpos realloc fail.\n");
        fflush(stdout);
        return -1;
    }

    for (int i = 0; i < DK_SIZE(mp->ma_keys); i++) {
        mp->ma_indices_to_hashpos[i] = -1;
    }

    // New table must be large enough.
    assert(mp->ma_keys->dk_usable >= mp->ma_used);
    if (oldkeys->dk_kind == DICT_KEYS_GENERAL)
        mp->ma_keys->dk_kind = DICT_KEYS_GENERAL;

    numentries = mp->ma_used;
    oldentries = DK_ENTRIES(oldkeys);
    newentries = DK_ENTRIES(mp->ma_keys);
    oldvalues = mp->ma_values;
    if (oldvalues != NULL) {
        /* Convert split table into new combined table.
         * We must incref keys; we can transfer values.
         * Note that values of split table is always dense.
         */
        for (Py_ssize_t i = 0; i < numentries; i++) {
            assert(oldvalues[i] != NULL);
            PyDictKeyEntry *ep = &oldentries[i];
            PyObject *key = ep->me_key;
            Py_INCREF(key);
            newentries[i].me_key = key;
            newentries[i].me_hash = ep->me_hash;
            newentries[i].me_value = oldvalues[i];
        }

        dictkeys_decref(oldkeys);
        mp->ma_values = NULL;
        if (oldvalues != empty_values) {
            free_values(oldvalues);
        }
    }
    else {  // combined table.
        if (0 /* oldkeys->dk_nentries == numentries */) {
            memcpy(newentries, oldentries, numentries * sizeof(PyDictKeyEntry));
        }
        else {
            collect_entries(mp, oldkeys, newentries, &numentries);
        }

        if (layers_reinit(mp, oldkeys))
            return -1;

        assert(oldkeys->dk_kind != DICT_KEYS_SPLIT);
        assert(oldkeys->dk_refcnt == 1);
#ifdef Py_REF_DEBUG
        _Py_RefTotal--;
#endif
        struct _Py_dict_state *state = get_dict_state();
#ifdef Py_DEBUG
        // dictresize() must not be called after _PyDict_Fini()
        assert(state->keys_numfree != -1);
#endif
        if (DK_SIZE(oldkeys) == PyDict_MINSIZE &&
            state->keys_numfree < PyDict_MAXFREELIST)
        {
            state->keys_free_list[state->keys_numfree++] = oldkeys;
        }
        else {
            PyObject_Free(oldkeys);
        }
    }

    build_idxs(mp, newentries, numentries);
    // mp->ma_keys->dk_usable -= numentries;
    mp->ma_keys->dk_nentries = numentries;
    ASSERT_CONSISTENT(mp);
    return 0;
}

/*
Restructure the table by allocating a new table and reinserting all
items again.  When entries have been deleted, the new table may
actually be smaller than the old one.
If a table is split (its keys and hashes are shared, its values are not),
then the values are temporarily copied into the table, it is resized as
a combined table, then the me_value slots in the old table are NULLed out.
After resizing a table is always combined,
but can be resplit by make_keys_shared().
*/
static int
dictresize(CustomPyDictObject *mp, uint8_t log2_newsize)
{
    printf("dictresize log2_newsize: %ld.\n", log2_newsize);
    fflush(stdout);

    Py_ssize_t numentries;
    PyDictKeysObject *oldkeys;
    PyObject **oldvalues;
    PyDictKeyEntry *oldentries, *newentries;

    if (log2_newsize >= SIZEOF_SIZE_T*8) {
        PyErr_NoMemory();
        return -1;
    }
    assert(log2_newsize >= PyDict_LOG_MINSIZE);

    oldkeys = mp->ma_keys;

#ifdef EBUG
    printf("old capacity: %lld.\n", DK_SIZE(mp->ma_keys));
#endif

    /* NOTE: Current odict checks mp->ma_keys to detect resize happen.
     * So we can't reuse oldkeys even if oldkeys->dk_size == newsize.
     * TODO: Try reusing oldkeys when reimplement odict.
     */

    /* Allocate a new table. */
    mp->ma_keys = new_keys_object(log2_newsize);
    if (mp->ma_keys == NULL) {
        mp->ma_keys = oldkeys;
        return -1;
    }

#ifdef EBUG
    printf("capacity: %lld.\n", DK_SIZE(mp->ma_keys));
#endif

    // New table must be large enough.
    assert(mp->ma_keys->dk_usable >= mp->ma_used);
    if (oldkeys->dk_kind == DICT_KEYS_GENERAL)
        mp->ma_keys->dk_kind = DICT_KEYS_GENERAL;

    numentries = mp->ma_used;
    oldentries = DK_ENTRIES(oldkeys);
    newentries = DK_ENTRIES(mp->ma_keys);
    oldvalues = mp->ma_values;
    if (oldvalues != NULL) {
        /* Convert split table into new combined table.
         * We must incref keys; we can transfer values.
         * Note that values of split table is always dense.
         */
        for (Py_ssize_t i = 0; i < numentries; i++) {
            assert(oldvalues[i] != NULL);
            PyDictKeyEntry *ep = &oldentries[i];
            PyObject *key = ep->me_key;
            Py_INCREF(key);
            newentries[i].me_key = key;
            newentries[i].me_hash = ep->me_hash;
            newentries[i].me_value = oldvalues[i];
        }

        dictkeys_decref(oldkeys);
        mp->ma_values = NULL;
        if (oldvalues != empty_values) {
            free_values(oldvalues);
        }
    }
    else {  // combined table.
        if (oldkeys->dk_nentries == numentries) {
            memcpy(newentries, oldentries, numentries * sizeof(PyDictKeyEntry));
        }
        else {
            PyDictKeyEntry *ep = oldentries;
            for (Py_ssize_t i = 0; i < numentries; i++) {
                while (ep->me_value == NULL)
                    ep++;
                newentries[i] = *ep++;
            }
        }

        assert(oldkeys->dk_kind != DICT_KEYS_SPLIT);
        assert(oldkeys->dk_refcnt == 1);
#ifdef Py_REF_DEBUG
        _Py_RefTotal--;
#endif
        struct _Py_dict_state *state = get_dict_state();
#ifdef Py_DEBUG
        // dictresize() must not be called after _PyDict_Fini()
        assert(state->keys_numfree != -1);
#endif
        if (DK_SIZE(oldkeys) == PyDict_MINSIZE &&
            state->keys_numfree < PyDict_MAXFREELIST)
        {
            state->keys_free_list[state->keys_numfree++] = oldkeys;
        }
        else {
            PyObject_Free(oldkeys);
        }
    }

    build_idxs(mp, newentries, numentries);
    mp->ma_keys->dk_usable -= numentries;
    mp->ma_keys->dk_nentries = numentries;
    return 0;
}

/* Find the smallest dk_size >= minsize. */
static inline uint8_t
calculate_log2_keysize(Py_ssize_t minsize)
{
#if SIZEOF_LONG == SIZEOF_SIZE_T
    minsize = (minsize | PyDict_MINSIZE) - 1;
    return _Py_bit_length(minsize | (PyDict_MINSIZE-1));
#elif defined(_MSC_VER)
    // On 64bit Windows, sizeof(long) == 4.
    minsize = (minsize | PyDict_MINSIZE) - 1;
    unsigned long msb;
    _BitScanReverse64(&msb, (uint64_t)minsize);
    return (uint8_t)(msb + 1);
#else
    uint8_t log2_size;
    for (log2_size = PyDict_LOG_MINSIZE;
            (((Py_ssize_t)1) << log2_size) < minsize;
            log2_size++)
        ;
    return log2_size;
#endif
}

/* estimate_keysize is reverse function of USABLE_FRACTION.
 *
 * This can be used to reserve enough size to insert n entries without
 * resizing.
 */
static inline uint8_t
estimate_log2_keysize(Py_ssize_t n)
{
    return calculate_log2_keysize((n*3 + 1) / 2);
}

/* GROWTH_RATE. Growth rate upon hitting maximum load.
 * Currently set to used*3.
 * This means that dicts double in size when growing without deletions,
 * but have more head room when the number of deletions is on a par with the
 * number of insertions.  See also bpo-17563 and bpo-33205.
 *
 * GROWTH_RATE was set to used*4 up to version 3.2.
 * GROWTH_RATE was set to used*2 in version 3.3.0
 * GROWTH_RATE was set to used*2 + capacity/2 in 3.4.0-3.6.0.
 */
#define GROWTH_RATE(d) ((d)->ma_used*3)
#define CUSTOM_GROWTH_RATE(d) ((d)->ma_num_items*2)

static int
custom_insertion_resize(CustomPyDictObject *mp,
        Py_ssize_t (*lookup)(CustomPyDictObject *, PyObject *, Py_hash_t, PyObject **, int *),
        Py_ssize_t (*empty_slot)(PyDictKeysObject *, Py_hash_t, size_t *, int *),
        void (*build_idxs)(CustomPyDictObject *, PyDictKeyEntry *, Py_ssize_t))
{
    printf("custom_insertion_resize\n");
    fflush(stdout);
    return resize(mp, calculate_log2_keysize(CUSTOM_GROWTH_RATE(mp)));
}

static int
insertion_resize(CustomPyDictObject *mp)
{
    printf("insertion_resize\n");
    fflush(stdout);
    return dictresize(mp, calculate_log2_keysize(GROWTH_RATE(mp)));
}

Py_ssize_t _Py_HOT_FUNCTION
rprobe_Py_dict_lookup(CustomPyDictObject *mp, PyObject *key, Py_hash_t hash, PyObject **value_addr, int *num_cmps)
{
#ifdef EBUG
    printf("called _Py_dict_lookup\n");
#endif

    PyDictKeysObject *dk;
start:
    dk = mp->ma_keys;
    DictKeysKind kind = dk->dk_kind;
    PyDictKeyEntry *ep0 = DK_ENTRIES(dk);
    size_t mask = DK_MASK(dk);
    size_t perturb = hash;
    size_t i = (size_t)hash & mask;

    Py_ssize_t ix;
    *num_cmps = 0;
    if (PyUnicode_CheckExact(key) && kind != DICT_KEYS_GENERAL) {
        /* Strings only */
        for (;;) {
            ix = dictkeys_get_index(mp->ma_keys, i);

#ifdef EBUG
            printf("0(i, ix): (%lld, %lld)\n", i, ix);
            fflush(stdout);
#endif

            if (ix >= 0) {
                (*num_cmps)++;
                PyDictKeyEntry *ep = &ep0[ix];
                assert(ep->me_key != NULL);
                assert(PyUnicode_CheckExact(ep->me_key));
                if (ep->me_key == key ||
                        (ep->me_hash == hash && unicode_eq(ep->me_key, key))) {
                    goto found;
                }
            }
            else if (ix == DKIX_EMPTY) {
                *value_addr = NULL;
                return DKIX_EMPTY;
            }
            perturb >>= PERTURB_SHIFT;
            i = mask & (i*5 + perturb + 1);
            ix = dictkeys_get_index(mp->ma_keys, i);

#ifdef EBUG
            printf("1(i, ix): (%lld, %lld)\n", i, ix);
            fflush(stdout);
#endif

            if (ix >= 0) {
                (*num_cmps)++;
                PyDictKeyEntry *ep = &ep0[ix];
                assert(ep->me_key != NULL);
                assert(PyUnicode_CheckExact(ep->me_key));
                if (ep->me_key == key ||
                        (ep->me_hash == hash && unicode_eq(ep->me_key, key))) {
                    goto found;
                }
            }
            else if (ix == DKIX_EMPTY) {
                *value_addr = NULL;
                return DKIX_EMPTY;
            }
            perturb >>= PERTURB_SHIFT;
            i = mask & (i*5 + perturb + 1);
        }
        Py_UNREACHABLE();
    }
    for (;;) {
        ix = dictkeys_get_index(dk, i);
        if (ix == DKIX_EMPTY) {
            *value_addr = NULL;
            return ix;
        }
        if (ix >= 0) {
            PyDictKeyEntry *ep = &ep0[ix];
            assert(ep->me_key != NULL);
            if (ep->me_key == key) {
                goto found;
            }
            if (ep->me_hash == hash) {
                PyObject *startkey = ep->me_key;
                Py_INCREF(startkey);
                int cmp = PyObject_RichCompareBool(startkey, key, Py_EQ);
                Py_DECREF(startkey);
                if (cmp < 0) {
                    *value_addr = NULL;
                    return DKIX_ERROR;
                }
                if (dk == mp->ma_keys && ep->me_key == startkey) {
                    if (cmp > 0) {
                        goto found;
                    }
                }
                else {
                    /* The dict was mutated, restart */
                    goto start;
                }
            }
        }
        perturb >>= PERTURB_SHIFT;
        i = (i*5 + perturb + 1) & mask;
    }
    Py_UNREACHABLE();
found:
    if (dk->dk_kind == DICT_KEYS_SPLIT) {
        *value_addr = mp->ma_values[ix];
    }
    else {
        *value_addr = ep0[ix].me_value;
    }
    return ix;
}

typedef struct {
    Py_ssize_t ix;
    Py_ssize_t layer_i;
} Ix;

Ix _Py_HOT_FUNCTION
custom_dictkeys_stringlookup(PyDictKeysObject *dk, Layer *layers, PyObject *key, Py_hash_t hash,
        size_t *hashpos0, int *num_cmps)
{
#ifdef EBUG
    printf("custom_dictkeys_stringlookup %s i: %lld.\n", PyUnicode_AsUTF8(key), (size_t)hash & DK_MASK(mp->ma_keys));
    fflush(stdout);
#endif

    PyDictKeyEntry *ep0 = DK_ENTRIES(dk);
    size_t mask = DK_MASK(dk);
    size_t i = *hashpos0 = (size_t)hash & mask;
    Py_ssize_t ix;
    *num_cmps = 0;
    for (;;) {
        ix = dictkeys_get_index(dk, i);

#ifdef EBUG
        printf("custom_dictkeys_stringlookup 0(i, ix): (%lld, %lld)\n", i, ix);
        fflush(stdout);
#endif

        if (ix >= 0) {
            (*num_cmps)++;
            PyDictKeyEntry *ep = &ep0[ix];
            assert(ep->me_key != NULL);
            assert(PyUnicode_CheckExact(ep->me_key));
            if (ep->me_key == key ||
                    (ep->me_hash == hash && unicode_eq(ep->me_key, key))) {
                Ix rv = { ix, -1 };
                return rv;
            }
            else if (i == *hashpos0 && layers[i].keys) {
                for (int j = 0; j < layers[i].used; j++) {
                    (*num_cmps)++;
                    if (layers[i].keys[j]->me_key == key ||
                            (layers[i].keys[j]->me_hash == hash && unicode_eq(layers[i].keys[j]->me_key, key))) {
                        Ix rv = { ix, j };
                        return rv;
                    }
                }

                Ix rv = { DKIX_EMPTY, -1 };
                return rv;
            }
        }
        else if (ix == DKIX_EMPTY) {
            Ix rv = { DKIX_EMPTY, -1 };
            return rv;
        }
        i = mask & (i + 1);
        ix = dictkeys_get_index(dk, i);

#ifdef EBUG
        printf("custom_dictkeys_stringlookup 1(i, ix): (%lld, %lld)\n", i, ix);
        fflush(stdout);
#endif

        if (ix >= 0) {
            (*num_cmps)++;
            PyDictKeyEntry *ep = &ep0[ix];
            assert(ep->me_key != NULL);
            assert(PyUnicode_CheckExact(ep->me_key));
            if (ep->me_key == key ||
                    (ep->me_hash == hash && unicode_eq(ep->me_key, key))) {
                Ix rv = { ix, -1 };
                return rv;
            }
            else if (i == *hashpos0 && layers[i].keys) {
                for (int j = 0; j < layers[i].used; j++) {
                    (*num_cmps)++;
                    if (layers[i].keys[j]->me_key == key ||
                            (layers[i].keys[j]->me_hash == hash && unicode_eq(layers[i].keys[j]->me_key, key))) {
                        Ix rv = { ix, j };
                        return rv;
                    }
                }

                Ix rv = { DKIX_EMPTY, -1 };
                return rv;
            }
        }
        else if (ix == DKIX_EMPTY) {
            Ix rv = { DKIX_EMPTY, -1 };
            return rv;
        }
        i = mask & (i + 1);
    }
    Py_UNREACHABLE();
}

Py_ssize_t _Py_HOT_FUNCTION
custom_Py_dict_lookup(CustomPyDictObject *mp, PyObject *key, Py_hash_t hash, PyObject **value_addr,
        int *num_cmps)
{
    PyDictKeysObject *dk;
    dk = mp->ma_keys;
    DictKeysKind kind = dk->dk_kind;
    if (PyUnicode_CheckExact(key) && kind != DICT_KEYS_GENERAL) {
        Ix ix;
        {
            size_t hashpos0;
            ix = custom_dictkeys_stringlookup(dk, mp->ma_layers, key, hash, &hashpos0, num_cmps);
        }
        if (ix.ix == DKIX_EMPTY) {
            *value_addr = NULL;
        }
        else if (kind == DICT_KEYS_SPLIT) {
            *value_addr = mp->ma_values[ix.ix];
        }
        else if (ix.layer_i < 0) {
            *value_addr = DK_ENTRIES(dk)[ix.ix].me_value;
        }
        else {
            Py_ssize_t i = mp->ma_indices_to_hashpos[ix.ix];
            *value_addr = mp->ma_layers[i].keys[ix.layer_i]->me_value;
        }
        return ix.ix;
    }
    Py_UNREACHABLE();
}


/* Internal function to find slot for an item from its hash
   when it is known that the key is not present in the dict.

   The dict must be combined. */
static Py_ssize_t
custom_find_empty_slot(PyDictKeysObject *keys, Py_hash_t hash, size_t* i0, int *num_cmps)
{
    // If layer.keys, then look up in layer; DO NOT probe!
    assert(keys != NULL);

    const size_t mask = DK_MASK(keys);
    size_t i = *i0 = hash & mask;

    Py_ssize_t ix = dictkeys_get_index(keys, i);
    *num_cmps = 0;

    while (ix >= 0) {
        (*num_cmps)++;
        i = (i + 1) & mask;
        ix = dictkeys_get_index(keys, i);
    }
    return i;
}

static int
custominsertdict_impl(CustomPyDictObject *mp, PyObject *key, Py_hash_t hash, PyObject *value,
        Py_ssize_t (*empty_slot)(PyDictKeysObject *, Py_hash_t, size_t *, int *))
{
    Py_ssize_t hashpos0;
    int num_cmps;
    Py_ssize_t hashpos = empty_slot(mp->ma_keys, hash, &hashpos0, &num_cmps);

    Layer *layer = &(mp->ma_layers[hashpos0]);
    if (layer->keys) {
        if (dictkeys_get_index(mp->ma_keys, hashpos0) == DKIX_EMPTY) {
            PyDictKeyEntry entry = { hash, key, value, hashpos0 };
            insertslot(mp, hashpos0, &entry);
            return 0;
        }

        insertlayer_keyhashvalue(layer, key, hash, value);
        return 0;
    }

    if (num_cmps <= mp->ma_keys->dk_log2_size) {
        PyDictKeyEntry entry = { hash, key, value, hashpos0 };
        insertslot(mp, hashpos, &entry);
        return 0;
    }

    filter(mp, hashpos0, num_cmps);
    if (dictkeys_get_index(mp->ma_keys, hashpos0) == DKIX_EMPTY) {
        printf("INVARIANT BROKEN %s.\n", PyUnicode_AsUTF8(key));
        fflush(stdout);
    }

    if (insertlayer_keyhashvalue(layer, key, hash, value)) {
        printf("custominsertdict memory problem calling insertlayer_keyhashvalue.\n");
        fflush(stdout);
        return -1;
    }

    return 0;
}

static void
custom_build_indices(CustomPyDictObject *mp, PyDictKeyEntry *ep, Py_ssize_t n)
{
    mp->ma_used = 0;
    for (int i = 0; i < n; i++, ep++) {
        custominsertdict_impl(mp, ep->me_key, ep->me_hash, ep->me_value, custom_find_empty_slot);
    }
}

static Py_ssize_t
dictkeys_stringlookup(PyDictKeysObject* dk, PyObject *key, Py_hash_t hash)
{
    PyDictKeyEntry *ep0 = DK_ENTRIES(dk);
    size_t mask = DK_MASK(dk);
    size_t perturb = hash;
    size_t i = (size_t)hash & mask;
    Py_ssize_t ix;
    for (;;) {
        ix = dictkeys_get_index(dk, i);
        if (ix >= 0) {
            PyDictKeyEntry *ep = &ep0[ix];
            assert(ep->me_key != NULL);
            assert(PyUnicode_CheckExact(ep->me_key));
            if (ep->me_key == key ||
                    (ep->me_hash == hash && unicode_eq(ep->me_key, key))) {
                return ix;
            }
        }
        else if (ix == DKIX_EMPTY) {
            return DKIX_EMPTY;
        }
        perturb >>= PERTURB_SHIFT;
        i = mask & (i*5 + perturb + 1);
        ix = dictkeys_get_index(dk, i);
        if (ix >= 0) {
            PyDictKeyEntry *ep = &ep0[ix];
            assert(ep->me_key != NULL);
            assert(PyUnicode_CheckExact(ep->me_key));
            if (ep->me_key == key ||
                    (ep->me_hash == hash && unicode_eq(ep->me_key, key))) {
                return ix;
            }
        }
        else if (ix == DKIX_EMPTY) {
            return DKIX_EMPTY;
        }
        perturb >>= PERTURB_SHIFT;
        i = mask & (i*5 + perturb + 1);
    }
    Py_UNREACHABLE();
}

Py_ssize_t _Py_HOT_FUNCTION
_Py_dict_lookup(PyDictObject *mp, PyObject *key, Py_hash_t hash, PyObject **value_addr)
{
    PyDictKeysObject *dk;
start:
    dk = mp->ma_keys;
    DictKeysKind kind = dk->dk_kind;
    if (PyUnicode_CheckExact(key) && kind != DICT_KEYS_GENERAL) {
        Py_ssize_t ix = dictkeys_stringlookup(dk, key, hash);
        if (ix == DKIX_EMPTY) {
            *value_addr = NULL;
        }
        else if (kind == DICT_KEYS_SPLIT) {
            *value_addr = mp->ma_values[ix];
        }
        else {
            *value_addr = DK_ENTRIES(dk)[ix].me_value;
        }
        return ix;
    }
    PyDictKeyEntry *ep0 = DK_ENTRIES(dk);
    size_t mask = DK_MASK(dk);
    size_t perturb = hash;
    size_t i = (size_t)hash & mask;
    Py_ssize_t ix;
    for (;;) {
        ix = dictkeys_get_index(dk, i);
        if (ix == DKIX_EMPTY) {
            *value_addr = NULL;
            return ix;
        }
        if (ix >= 0) {
            PyDictKeyEntry *ep = &ep0[ix];
            assert(ep->me_key != NULL);
            if (ep->me_key == key) {
                goto found;
            }
            if (ep->me_hash == hash) {
                PyObject *startkey = ep->me_key;
                Py_INCREF(startkey);
                int cmp = PyObject_RichCompareBool(startkey, key, Py_EQ);
                Py_DECREF(startkey);
                if (cmp < 0) {
                    *value_addr = NULL;
                    return DKIX_ERROR;
                }
                if (dk == mp->ma_keys && ep->me_key == startkey) {
                    if (cmp > 0) {
                        goto found;
                    }
                }
                else {
                    /* The dict was mutated, restart */
                    goto start;
                }
            }
        }
        perturb >>= PERTURB_SHIFT;
        i = (i*5 + perturb + 1) & mask;
    }
    Py_UNREACHABLE();
found:
    if (dk->dk_kind == DICT_KEYS_SPLIT) {
        *value_addr = mp->ma_values[ix];
    }
    else {
        *value_addr = ep0[ix].me_value;
    }
    return ix;
}

#define MAINTAIN_TRACKING(mp, key, value) \
    do { \
        if (!_PyObject_GC_IS_TRACKED(mp)) { \
            if (_PyObject_GC_MAY_BE_TRACKED(key) || \
                _PyObject_GC_MAY_BE_TRACKED(value)) { \
                _PyObject_GC_TRACK(mp); \
            } \
        } \
    } while(0)

/* True if the object may be tracked by the GC in the future, or already is.
   This can be useful to implement some optimizations. */
#define _PyObject_GC_MAY_BE_TRACKED(obj) \
    (PyObject_IS_GC(obj) && \
        (!PyTuple_CheckExact(obj) || _PyObject_GC_IS_TRACKED(obj)))

/* Internal function to find slot for an item from its hash
   when it is known that the key is not present in the dict.

   The dict must be combined. */
static Py_ssize_t
find_empty_slot(PyDictKeysObject *keys, Py_hash_t hash, size_t* i0, int *num_cmps)
{
    assert(keys != NULL);

    const size_t mask = DK_MASK(keys);
    size_t i = *i0 = hash & mask;
    Py_ssize_t ix = dictkeys_get_index(keys, i);
    *num_cmps = 0;
    for (size_t perturb = hash; ix >= 0;) {
        (*num_cmps)++;
        perturb >>= PERTURB_SHIFT;
        i = (i*5 + perturb + 1) & mask;
        ix = dictkeys_get_index(keys, i);
    }
    return i;
}

int
seen(PyDictKeyEntry entry, PyDictKeyEntry *A, int n)
{
    for (int i = 0; i < n; i++) {
        if (A[i].me_key == entry.me_key ||
                (A[i].me_hash == entry.me_hash && unicode_eq(A[i].me_key, entry.me_key)))
            return 1;
    }

    return 0;
}

int
dict_traverse2(CustomPyDictObject *dict, int print)
{
    PyDictKeysObject *keys = dict->ma_keys;
    PyDictKeyEntry *ep = DK_ENTRIES(keys);
    int num_items = 0;

    PyDictKeyEntry *seen_entries = malloc((dict->ma_num_items * 2) * sizeof *seen_entries);
    if (!seen_entries) {
        printf("dict_traverse2 malloc fail.\n");
        fflush(stdout);
        return -1;
    }

    int seen_entries_idx = 0;
    int error = 0;

    for (int i = 0; i < DK_SIZE(keys); i++) {
        Py_ssize_t ix = dictkeys_get_index(keys, i);

        if (ix < 0 && print) {
            printf("%d -> -1\n", i);
            fflush(stdout);
        }

        if (ix >= 0 && print) {
            printf("%d -> %lld: ", i, ix);
            fflush(stdout);
        }

        if (ix >= 0 && !ep[ix].me_key && !dict->ma_layers[i].keys) {
            if (print) {
                printf("\n");
                fflush(stdout);
            }

            continue;
        }

        if (ix >= 0 && ep[ix].me_key) {
            if (seen(ep[ix], seen_entries, seen_entries_idx)) {
                printf("primary already have %s in dict.\n", PyUnicode_AsUTF8(ep[ix].me_key));
                fflush(stdout);
                error = 1;
                goto error_occurred;
            }

            num_items++;
            seen_entries[seen_entries_idx] = ep[ix];
            seen_entries_idx++;

            if (print) {
                printf("%s.\n", PyUnicode_AsUTF8(ep[ix].me_key));
                fflush(stdout);
            }
        }

        if (dict->ma_layers && dict->ma_layers[i].keys) {
            if (print) {
                if (ix < 0) {
                    printf("%d -> %lld: ", i, ix);
                    fflush(stdout);
                }

                printf("\t");
                fflush(stdout);
            }

            Layer *layer = &dict->ma_layers[i];
            for (int j = 0; j < layer->used; j++) {
                if (seen(*layer->keys[j], seen_entries, seen_entries_idx)) {
                    printf("already have %p %s in dict;", layer->keys[j]->me_key, PyUnicode_AsUTF8(layer->keys[j]->me_key));
                    fflush(stdout);
                    error = 1;
                }

                num_items++;
                seen_entries[seen_entries_idx] = *layer->keys[j];
                seen_entries_idx++;

                if (print) {
                    printf("%s", PyUnicode_AsUTF8(layer->keys[j]->me_key));
                    fflush(stdout);
                }

                if (print && j < (layer->used - 1)) {
                    printf(", ");
                    fflush(stdout);
                }
            }

            if (print) {
                printf(".\n");
                fflush(stdout);
            }

            if (error)
                goto error_occurred;
        }
    }
    printf("traversed.\n");
    fflush(stdout);
    if (print) {
        printf("size of primary layer: %lld.\n", DK_SIZE(keys));
        printf("num_items: %d.\n", num_items);
        printf("ma_num_items: %lld.\n", dict->ma_num_items);
        fflush(stdout);
    }

    /* for (int i = 0; i < seen_keys_idx; i++) {
        printf("%d %s\n", i, seen_keys[i]);
        fflush(stdout);
    } */

    printf("ma_num_items: %lld.\n", dict->ma_num_items);
    fflush(stdout);
    for (int i = 0; i < dict->keys ? dict->ma_num_items : 0; i++) {
        int found = 0;
        for (int j = 0; j < seen_entries_idx; j++) {
            /* printf("strcmp %s %s.\n", dict->ma_string_keys[i], seen_keys[j]);
            fflush(stdout); */

            if (dict->keys[i].me_key == seen_entries[j].me_key ||
                    (dict->keys[i].me_hash == seen_entries[j].me_hash && unicode_eq(dict->keys[i].me_key, seen_entries[j].me_key))) {
                found = 1;
                break;
            }
        }

        if (found)
            ; // printf("found %s.\n", dict->ma_string_keys[i]);
        else {
            printf("%s missing!!!\n", PyUnicode_AsUTF8(dict->keys[i].me_key));
            error = 1;
        }

        fflush(stdout);
    }

error_occurred:
    free(seen_entries);

    if (error)
        return -1;

    return num_items;
}

// #define EBUG_INSERT
/*
Internal routine to insert a new item into the table.
Used both by the internal resize routine and by the public insert routine.
Returns -1 if an error occurred, or 0 on success.
*/
static int
custominsertdict(CustomPyDictObject *mp, PyObject *key, Py_hash_t hash, PyObject *value)
{
    printf("custominsertdict\n");
    fflush(stdout);
    PyObject *old_value;

    if (mp->ma_values != NULL && !PyUnicode_CheckExact(key)) {
        if (custom_insertion_resize(mp, lookup, empty_slot, build_idxs) < 0)
            goto Fail;
    }

    Py_ssize_t ix;
    {
        // Insert doesn't use lookup's extra return values
        int num_cmps;
        ix = lookup(mp, key, hash, &old_value, &num_cmps);
    }
    if (ix == DKIX_ERROR)
        goto Fail;

    MAINTAIN_TRACKING(mp, key, value);

    if (ix == DKIX_EMPTY) {
        /* Insert into new slot. */
        mp->ma_keys->dk_version = 0;
        assert(old_value == NULL);
        if (mp->ma_keys->dk_usable <= 0) {
            printf("custominsertdict resize (num_items, used): (%lld, %lld).\n", mp->ma_num_items, mp->ma_used);
            fflush(stdout);

            /* Need to resize. */
            if (custom_insertion_resize(mp, lookup, empty_slot, build_idxs) < 0)
                goto Fail;
        }
        if (!PyUnicode_CheckExact(key) && mp->ma_keys->dk_kind != DICT_KEYS_GENERAL) {
            mp->ma_keys->dk_kind = DICT_KEYS_GENERAL;
        }

        int rv = custominsertdict_impl(mp, key, hash, value, empty_slot);
        if (!rv) {
            PyDictKeyEntry entry = { hash, key, value };
            mp->keys[mp->ma_num_items] = entry;
            mp->ma_num_items++;
        }

        return rv;
    }

    if (old_value != value) {
        if (_PyDict_HasSplitTable(mp)) {
            mp->ma_values[ix] = value;
            if (old_value == NULL) {
                /* pending state */
                assert(ix == mp->ma_used);
                mp->ma_used++;
            }
        }
        else {
            assert(old_value != NULL);

            printf("updating me_value.\n");
            fflush(stdout);

            DK_ENTRIES(mp->ma_keys)[ix].me_value = value;
        }
        mp->ma_version_tag = DICT_NEXT_VERSION();
    }
    Py_XDECREF(old_value); /* which **CAN** re-enter (see issue #22653) */
    ASSERT_CONSISTENT(mp);
    Py_DECREF(key);
    return 0;

Fail:
    Py_DECREF(value);
    Py_DECREF(key);
    return -1;
}

/*
Internal routine to insert a new item into the table.
Used both by the internal resize routine and by the public insert routine.
Returns -1 if an error occurred, or 0 on success.
*/
static int
insertdict(CustomPyDictObject *mp, PyObject *key, Py_hash_t hash, PyObject *value)
{
    PyObject *old_value;
    PyDictKeyEntry *ep;

    Py_INCREF(key);
    Py_INCREF(value);
    if (mp->ma_values != NULL && !PyUnicode_CheckExact(key)) {
        if (insertion_resize(mp) < 0)
            goto Fail;
    }

    int num_cmps;   /* currently not measuring the efficiency of insert */
    Py_ssize_t ix = lookup(mp, key, hash, &old_value, &num_cmps);

    if (ix == DKIX_ERROR)
        goto Fail;

    MAINTAIN_TRACKING(mp, key, value);

    /* When insertion order is different from shared key, we can't share
     * the key anymore.  Convert this instance to combine table.
     */
    if (_PyDict_HasSplitTable(mp) &&
        ((ix >= 0 && old_value == NULL && mp->ma_used != ix) ||
         (ix == DKIX_EMPTY && mp->ma_used != mp->ma_keys->dk_nentries))) {
        if (insertion_resize(mp) < 0)
            goto Fail;
        ix = DKIX_EMPTY;
    }

    if (ix == DKIX_EMPTY) {
        /* Insert into new slot. */
        mp->ma_keys->dk_version = 0;
        assert(old_value == NULL);
        if (mp->ma_keys->dk_usable <= 0) {
            /* Need to resize. */
            if (insertion_resize(mp) < 0)
                goto Fail;
        }
        if (!PyUnicode_CheckExact(key) && mp->ma_keys->dk_kind != DICT_KEYS_GENERAL) {
            mp->ma_keys->dk_kind = DICT_KEYS_GENERAL;
        }

        // ix = lookup(mp, key, hash, &old_value, &num_cmps);
        size_t i;
        int num_cmps2;
        Py_ssize_t hashpos = empty_slot(mp->ma_keys, hash, &i, &num_cmps2);
        /* if (num_cmps2 != num_cmps) {
            printf("num_cmps2 != num_cmps: %d != %d.\n", num_cmps2, num_cmps);
            fflush(stdout);
        } */

        ep = &DK_ENTRIES(mp->ma_keys)[mp->ma_keys->dk_nentries];
        dictkeys_set_index(mp->ma_keys, hashpos, mp->ma_keys->dk_nentries);
        ep->me_key = key;
        ep->me_hash = hash;
        if (mp->ma_values) {
            assert (mp->ma_values[mp->ma_keys->dk_nentries] == NULL);
            mp->ma_values[mp->ma_keys->dk_nentries] = value;
        }
        else {
            ep->me_value = value;
        }
        //ep->me_layer = NULL;
        mp->ma_used++;
        mp->ma_version_tag = DICT_NEXT_VERSION();
        mp->ma_keys->dk_usable--;
        mp->ma_keys->dk_nentries++;
        assert(mp->ma_keys->dk_usable >= 0);
        ASSERT_CONSISTENT(mp);
        return 0;
    }

    if (old_value != value) {
        if (_PyDict_HasSplitTable(mp)) {
            mp->ma_values[ix] = value;
            if (old_value == NULL) {
                /* pending state */
                assert(ix == mp->ma_used);
                mp->ma_used++;
            }
        }
        else {
            assert(old_value != NULL);

            printf("updating me_value.\n");

            DK_ENTRIES(mp->ma_keys)[ix].me_value = value;
        }
        mp->ma_version_tag = DICT_NEXT_VERSION();
    }
    Py_XDECREF(old_value); /* which **CAN** re-enter (see issue #22653) */
    ASSERT_CONSISTENT(mp);
    Py_DECREF(key);
    return 0;

Fail:
    Py_DECREF(value);
    Py_DECREF(key);
    return -1;
}

/* Internal version of PyDict_Contains used when the hash value is already known */
int
custom_PyDict_Contains_KnownHash2(PyObject *op, PyObject *key, Py_hash_t hash)
{
    CustomPyDictObject *mp = (CustomPyDictObject *)op;
    PyObject *value = op;
    Py_ssize_t ix = 0;

    // ix = _Py_dict_lookup(mp, key, hash, &value, &num_cmps);
    if (ix == DKIX_ERROR)
        return -1;
    return (ix != DKIX_EMPTY && value != NULL);
}

/* Internal version of PyDict_Contains used when the hash value is already known */
int
custom_PyDict_Contains_KnownHash(PyObject *op, PyObject *key, Py_hash_t hash)
{
    PyDictObject *mp = (PyDictObject *)op;
    PyObject *value = op;
    Py_ssize_t ix = 0;

    // ix = _Py_dict_lookup(mp, key, hash, &value, &num_cmps);
    if (ix == DKIX_ERROR)
        return -1;
    return (ix != DKIX_EMPTY && value != NULL);
}

// Same to insertdict but specialized for ma_keys = Py_EMPTY_KEYS.
static int
custom_insert_to_emptydict(CustomPyDictObject *mp, PyObject *key, Py_hash_t hash,
                    PyObject *value)
{
    printf("called custom_insert_to_emptydict; hash: %lld\n", hash);
    fflush(stdout);
#ifdef EBUG
#endif

    assert(mp->ma_keys == Py_EMPTY_KEYS);

    PyDictKeysObject *newkeys = new_keys_object(PyDict_LOG_MINSIZE);

    if (newkeys == NULL) {
        return -1;
    }
    if (!PyUnicode_CheckExact(key)) {
        newkeys->dk_kind = DICT_KEYS_GENERAL;
    }
    dictkeys_decref(Py_EMPTY_KEYS);
    mp->ma_keys = newkeys;
    mp->ma_values = NULL;

    mp->ma_indices_stack = malloc(DK_SIZE(mp->ma_keys) * sizeof *(mp->ma_indices_stack));
    if (mp->ma_indices_stack == NULL) {
        printf("custom_insert_to_emptydict ma_indices_stack malloc fail.\n");
        fflush(stdout);
        return -1;
    }

    for (int i = 0; i < DK_SIZE(mp->ma_keys); i++)
        mp->ma_indices_stack[i] = DK_SIZE(mp->ma_keys) - 1 - i;
    mp->ma_indices_stack_idx = DK_SIZE(mp->ma_keys) - 1;

    mp->ma_indices_to_hashpos = malloc(DK_SIZE(newkeys) * sizeof *(mp->ma_indices_to_hashpos));
    if (mp->ma_indices_to_hashpos == NULL) {
        printf("custom_insert_to_emptydict ma_indices_to_hashpos malloc fail.\n");
        fflush(stdout);
        return -1;
    }

    for (int i = 0; i < DK_SIZE(mp->ma_keys); i++)
        mp->ma_indices_to_hashpos[i] = -1;

    printf("custom_insert_to_emptydict mallocing %lld.\n", DK_SIZE(newkeys));
    fflush(stdout);
    mp->ma_layers = malloc(DK_SIZE(newkeys) * sizeof *(mp->ma_layers));
    if (mp->ma_layers == NULL) {
        printf("custom_insert_to_emptydict ma_layers malloc fail.\n");
        fflush(stdout);
        return -1;
    }

    for (int64_t i = 0; i < DK_SIZE(newkeys); i++) {
        mp->ma_layers[i].keys = NULL;
        mp->ma_layers[i].used = 0;
        mp->ma_layers[i].n = 0;
    }

    int MAX = 50000;
    mp->keys = malloc(MAX * sizeof *(mp->keys));
    if (!mp->keys) {
        printf("custom_insert_to_emptydict keys malloc fail.\n");
        fflush(stdout);
        return -1;
    }

    Py_INCREF(key);
    Py_INCREF(value);
    MAINTAIN_TRACKING(mp, key, value);

    size_t hashpos = (size_t)hash & (PyDict_MINSIZE-1);

    PyDictKeyEntry *ep = DK_ENTRIES(mp->ma_keys);

    Py_ssize_t idx = mp->ma_indices_stack[mp->ma_indices_stack_idx];
    mp->ma_indices_stack_idx--;

    dictkeys_set_index(mp->ma_keys, hashpos, idx);
    mp->ma_indices_to_hashpos[idx] = hashpos;

    ep->me_key = key;
    ep->me_hash = hash;
    ep->me_value = value;
    ep->i = hashpos;
    printf("empty dict (key, hashpos): (%s, %lld).\n", PyUnicode_AsUTF8(key), hashpos);
    fflush(stdout);

    mp->keys[0]  = *ep;
    mp->ma_used++;
    mp->ma_num_items++;
    mp->ma_version_tag = DICT_NEXT_VERSION();
    mp->ma_keys->dk_usable--;
    mp->ma_keys->dk_nentries++;
    return 0;
}

// Same to insertdict but specialized for ma_keys = Py_EMPTY_KEYS.
static int
insert_to_emptydict(CustomPyDictObject *mp, PyObject *key, Py_hash_t hash,
                    PyObject *value)
{
#ifdef EBUG
    printf("called insert_to_emptydict; hash: %lld\n", hash);
    fflush(stdout);
#endif

    assert(mp->ma_keys == Py_EMPTY_KEYS);

    PyDictKeysObject *newkeys = new_keys_object(PyDict_LOG_MINSIZE);
    if (newkeys == NULL) {
        return -1;
    }
    if (!PyUnicode_CheckExact(key)) {
        newkeys->dk_kind = DICT_KEYS_GENERAL;
    }
    dictkeys_decref(Py_EMPTY_KEYS);
    mp->ma_keys = newkeys;
    mp->ma_values = NULL;

    Py_INCREF(key);
    Py_INCREF(value);
    MAINTAIN_TRACKING(mp, key, value);

    size_t hashpos = (size_t)hash & (PyDict_MINSIZE-1);

    PyDictKeyEntry *ep = DK_ENTRIES(mp->ma_keys);
    dictkeys_set_index(mp->ma_keys, hashpos, 0);
    ep->me_key = key;
    ep->me_hash = hash;
    ep->me_value = value;
    mp->ma_used++;
    mp->ma_version_tag = DICT_NEXT_VERSION();
    mp->ma_keys->dk_usable--;
    mp->ma_keys->dk_nentries++;
    mp->keys = NULL;
    return 0;
}

/* Optimized memcpy() for Windows */
#ifdef _MSC_VER
#  if SIZEOF_PY_UHASH_T == 4
#    define PY_UHASH_CPY(dst, src) do {                                    \
       dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3]; \
       } while(0)
#  elif SIZEOF_PY_UHASH_T == 8
#    define PY_UHASH_CPY(dst, src) do {                                    \
       dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3]; \
       dst[4] = src[4]; dst[5] = src[5]; dst[6] = src[6]; dst[7] = src[7]; \
       } while(0)
#  else
#    error SIZEOF_PY_UHASH_T must be 4 or 8
#  endif /* SIZEOF_PY_UHASH_T */
#else /* not Windows */
#  define PY_UHASH_CPY(dst, src) memcpy(dst, src, SIZEOF_PY_UHASH_T)
#endif /* _MSC_VER */

/* **************************************************************************
 * Modified Fowler-Noll-Vo (FNV) hash function
 */
static Py_hash_t
fnv(const void *src, Py_ssize_t len, Py_ssize_t prefix, Py_ssize_t suffix)
{
    const unsigned char *p = src;
    Py_uhash_t x;
    Py_ssize_t remainder, blocks;
    union {
        Py_uhash_t value;
        unsigned char bytes[SIZEOF_PY_UHASH_T];
    } block;

#ifdef Py_DEBUG
    assert(_Py_HashSecret_Initialized);
#endif
    remainder = len % SIZEOF_PY_UHASH_T;
    if (remainder == 0) {
        /* Process at least one block byte by byte to reduce hash collisions
         * for strings with common prefixes. */
        remainder = SIZEOF_PY_UHASH_T;
    }
    blocks = (len - remainder) / SIZEOF_PY_UHASH_T;

    // printf("_Py_HashSecret.fnv.prefix: %lld.\n", _Py_HashSecret.fnv.prefix);

    // x = (Py_uhash_t) _Py_HashSecret.fnv.prefix;
    x = prefix;
    x ^= (Py_uhash_t) *p << 7;
    while (blocks--) {
        PY_UHASH_CPY(block.bytes, p);
        x = (_PyHASH_MULTIPLIER * x) ^ block.value;
        p += SIZEOF_PY_UHASH_T;
    }
    /* add remainder */
    for (; remainder > 0; remainder--)
        x = (_PyHASH_MULTIPLIER * x) ^ (Py_uhash_t) *p++;
    x ^= (Py_uhash_t) len;
    // x ^= (Py_uhash_t) _Py_HashSecret.fnv.suffix;
    x ^= suffix;
    if (x == (Py_uhash_t) -1) {
        x = (Py_uhash_t) -2;
    }
    return x;
}

Py_hash_t
custom_Py_HashBytes(const void *src, Py_ssize_t len, Py_ssize_t prefix, Py_ssize_t suffix)
{
    Py_hash_t x;
    /*
      We make the hash of the empty string be 0, rather than using
      (prefix ^ suffix), since this slightly obfuscates the hash secret
    */
    if (len == 0) {
        return 0;
    }

#ifdef Py_HASH_STATS
    hashstats[(len <= Py_HASH_STATS_MAX) ? len : 0]++;
#endif

#if Py_HASH_CUTOFF > 0
    if (len < Py_HASH_CUTOFF) {
        printf("custom_Py_HashBytes %d < %d\n.", len, Py_HASH_CUTOFF);

        /* Optimize hashing of very small strings with inline DJBX33A. */
        Py_uhash_t hash;
        const unsigned char *p = src;
        hash = 5381; /* DJBX33A starts with 5381 */

        switch(len) {
            /* ((hash << 5) + hash) + *p == hash * 33 + *p */
            case 7: hash = ((hash << 5) + hash) + *p++; /* fallthrough */
            case 6: hash = ((hash << 5) + hash) + *p++; /* fallthrough */
            case 5: hash = ((hash << 5) + hash) + *p++; /* fallthrough */
            case 4: hash = ((hash << 5) + hash) + *p++; /* fallthrough */
            case 3: hash = ((hash << 5) + hash) + *p++; /* fallthrough */
            case 2: hash = ((hash << 5) + hash) + *p++; /* fallthrough */
            case 1: hash = ((hash << 5) + hash) + *p++; break;
            default:
                Py_UNREACHABLE();
        }
        hash ^= len;
        hash ^= (Py_uhash_t) _Py_HashSecret.djbx33a.suffix;
        x = (Py_hash_t)hash;
    }
    else
#endif /* Py_HASH_CUTOFF */
        x = fnv(src, len, prefix, suffix);

    if (x == -1)
        return -2;
    return x;
}

/* Believe it or not, this produces the same value for ASCII strings
   as bytes_hash(). */
static Py_hash_t
unicode_hash(PyObject *self, Py_ssize_t prefix, Py_ssize_t suffix)
{
    Py_uhash_t x;  /* Unsigned for defined overflow behavior. */

#ifdef Py_DEBUG
    assert(_Py_HashSecret_Initialized);
#endif
    /* if (_PyUnicode_HASH(self) != -1) {
        printf("_PyUnicode_HASH(self) != -1.\n");

        return _PyUnicode_HASH(self);
    }
    if (PyUnicode_READY(self) == -1)
        return -1; */

#ifdef EBUG
    printf("unicode_hash calling custom_Py_HashBytes.\n");
#endif

    x = custom_Py_HashBytes(PyUnicode_DATA(self),
                      PyUnicode_GET_LENGTH(self) * PyUnicode_KIND(self),
                      prefix,
                      suffix);
    _PyUnicode_HASH(self) = x;
    return x;
}

Py_hash_t
custom_PyObject_Hash(PyObject *v)
{
    PyTypeObject *tp = Py_TYPE(v);

    if (tp->tp_hash != NULL) {
        // return (*tp->tp_hash)(v);
        return unicode_hash(v, 0, 0);
    }
    /* To keep to the general practice that inheriting
     * solely from object in C code should work without
     * an explicit call to PyType_Ready, we implicitly call
     * PyType_Ready here and then check the tp_hash slot again
     */
    if (tp->tp_dict == NULL) {
        if (PyType_Ready(tp) < 0)
            return -1;
        if (tp->tp_hash != NULL)
            return (*tp->tp_hash)(v);
    }
    /* Otherwise, the object can't be hashed */
    return PyObject_HashNotImplemented(v);
}

/* CAUTION: PyDict_SetItem() must guarantee that it won't resize the
 * dictionary if it's merely replacing the value for an existing key.
 * This means that it's safe to loop over a dictionary with PyDict_Next()
 * and occasionally replace a value -- but you can't insert new keys or
 * remove them.
 */
int
custom_PyDict_SetItem2(PyObject *op, PyObject *key, PyObject *value)
{
#ifdef EBUG
    printf("called custom_PyDict_SetItem2\n");
    fflush(stdout);
#endif

    CustomPyDictObject *mp;
    Py_hash_t hash;
    if (!PyDict_Check(op)) {
        PyErr_BadInternalCall();
        return -1;
    }
    assert(key);
    assert(value);
    mp = (CustomPyDictObject *)op;

    hash = custom_PyObject_Hash(key);

#ifdef EBUG
    printf("custom_PyDict_SetItem2 hash: %lld\n", hash);
    fflush(stdout);
#endif

    if (!PyUnicode_CheckExact(key) ||
        (hash = ((PyASCIIObject *) key)->hash) == -1)
    {
        if (hash == -1)
            return -1;
    }

    if (mp->ma_keys == Py_EMPTY_KEYS) {
        return emptydictinsertion(mp, key, hash, value);
    }

    /* insert() handles any resizing that might be necessary */
    return insert(mp, key, hash, value);
}

/* CAUTION: PyDict_SetItem() must guarantee that it won't resize the
 * dictionary if it's merely replacing the value for an existing key.
 * This means that it's safe to loop over a dictionary with PyDict_Next()
 * and occasionally replace a value -- but you can't insert new keys or
 * remove them.
 */
int
custom_PyDict_SetItem(PyObject *op, PyObject *key, PyObject *value,
        Py_ssize_t (*lookup)(PyDictObject *, PyObject *, Py_hash_t, PyObject **, int *),
        Py_ssize_t (*empty_slot)(PyDictKeysObject *keys, Py_hash_t hash, size_t *, int *),
        void (*build_idxs)(PyDictKeysObject *, PyDictKeyEntry *, Py_ssize_t))
{
#ifdef EBUG
    printf("called custom_PyDict_SetItem\n");
#endif

    CustomPyDictObject *mp;
    Py_hash_t hash;
    if (!PyDict_Check(op)) {
        PyErr_BadInternalCall();
        return -1;
    }
    assert(key);
    assert(value);
    mp = (CustomPyDictObject *)op;

    hash = custom_PyObject_Hash(key);

#ifdef EBUG
    printf("custom_PyDict_SetItem hash: %lld\n", hash);
#endif

    if (!PyUnicode_CheckExact(key) ||
        (hash = ((PyASCIIObject *) key)->hash) == -1)
    {
        if (hash == -1)
            return -1;
    }

    if (mp->ma_keys == Py_EMPTY_KEYS) {
        return insert_to_emptydict(mp, key, hash, value);
    }
    /* insertdict() handles any resizing that might be necessary */
    return insertdict(mp, key, hash, value);
}

static int
custom_dict_merge(PyObject *a, PyObject *b, int override)
{
#ifdef EBUG
    printf("\ncustom_dict_merge override: %d\n", override);
    fflush(stdout);
#endif

    CustomPyDictObject *mp, *other;
    Py_ssize_t i, n;
    PyDictKeyEntry *entry, *ep0;

    assert(0 <= override && override <= 2);

    /* We accept for the argument either a concrete dictionary object,
     * or an abstract "mapping" object.  For the former, we can do
     * things quite efficiently.  For the latter, we only require that
     * PyMapping_Keys() and PyObject_GetItem() be supported.
     */
    if (a == NULL || !PyDict_Check(a) || b == NULL) {
        PyErr_BadInternalCall();
        return -1;
    }
    mp = (CustomPyDictObject*)a;
    if (PyDict_Check(b) && (Py_TYPE(b)->tp_iter == (getiterfunc)dict_iter)) {
        other = (CustomPyDictObject*)b;
        if (other == mp || other->ma_used == 0)
            /* a.update(a) or a.update({}); nothing to do */
            return 0;
        if (mp->ma_used == 0) {
            /* Since the target dict is empty, PyDict_GetItem()
             * always returns NULL.  Setting override to 1
             * skips the unnecessary test.
             */
            override = 1;
            PyDictKeysObject *okeys = other->ma_keys;

            // If other is clean, combined, and just allocated, just clone it.
            if (other->ma_values == NULL &&
                    other->ma_used == okeys->dk_nentries &&
                    (DK_SIZE(okeys) == PyDict_MINSIZE ||
                     USABLE_FRACTION(DK_SIZE(okeys)/2) < other->ma_used)) {
                PyDictKeysObject *keys = custom_clone_combined_dict_keys(other);
                if (keys == NULL) {
                    return -1;
                }

                dictkeys_decref(mp->ma_keys);
                mp->ma_keys = keys;
                if (mp->ma_values != NULL) {
                    if (mp->ma_values != empty_values) {
                        free_values(mp->ma_values);
                    }
                    mp->ma_values = NULL;
                }

                mp->ma_used = other->ma_used;
                mp->ma_version_tag = DICT_NEXT_VERSION();
                ASSERT_CONSISTENT(mp);

                if (_PyObject_GC_IS_TRACKED(other) && !_PyObject_GC_IS_TRACKED(mp)) {
                    /* Maintain tracking. */
                    _PyObject_GC_TRACK(mp);
                }

                return 0;
            }
        }
        /* Do one big resize at the start, rather than
         * incrementally resizing as we insert new items.  Expect
         * that there will be no (or few) overlapping keys.
         */
        if (USABLE_FRACTION(DK_SIZE(mp->ma_keys)) < other->ma_used) {
            if (customdictresize(mp, estimate_log2_keysize(mp->ma_used + other->ma_used), lookup, empty_slot, build_idxs)) {
               return -1;
            }
        }
        ep0 = DK_ENTRIES(other->ma_keys);
        for (i = 0, n = other->ma_keys->dk_nentries; i < n; i++) {
            PyObject *key, *value;
            Py_hash_t hash;
            entry = &ep0[i];
            key = entry->me_key;
            hash = entry->me_hash;
            if (other->ma_values)
                value = other->ma_values[i];
            else
                value = entry->me_value;

            if (value != NULL) {
                int err = 0;
                Py_INCREF(key);
                Py_INCREF(value);
                if (override == 1)
                    err = insert(mp, key, hash, value);
                else {
                    err = custom_PyDict_Contains_KnownHash(a, key, hash);
                    if (err == 0) {
                        err = insert(mp, key, hash, value);
                    }
                    else if (err > 0) {
                        if (override != 0) {
                            _PyErr_SetKeyError(key);
                            Py_DECREF(value);
                            Py_DECREF(key);
                            return -1;
                        }
                        err = 0;
                    }
                }
                Py_DECREF(value);
                Py_DECREF(key);
                if (err != 0)
                    return -1;

                if (n != other->ma_keys->dk_nentries) {
                    PyErr_SetString(PyExc_RuntimeError,
                                    "dict mutated during update");
                    return -1;
                }
            }
        }
    }
    else {
        /* printf("custom_dict_merge else.\n");
        fflush(stdout); */

        /* Do it the generic, slower way */
        PyObject *keys = PyMapping_Keys(b);
        PyObject *iter;
        PyObject *key, *value;
        int status;

        if (keys == NULL)
            /* Docstring says this is equivalent to E.keys() so
             * if E doesn't have a .keys() method we want
             * AttributeError to percolate up.  Might as well
             * do the same for any other error.
             */
            return -1;

        iter = PyObject_GetIter(keys);
        Py_DECREF(keys);
        if (iter == NULL)
            return -1;

        for (key = PyIter_Next(iter); key; key = PyIter_Next(iter)) {
#ifdef EBUG
            printf("dict_merge for loop iteration.\n");
#endif

            if (override != 1) {
                status = PyDict_Contains(a, key);

                if (status != 0) {
                    if (status > 0) {
                        if (override == 0) {
                            Py_DECREF(key);
                            continue;
                        }
                        _PyErr_SetKeyError(key);
                    }
                    Py_DECREF(key);
                    Py_DECREF(iter);
                    return -1;
                }
            }
            value = PyObject_GetItem(b, key);
            if (value == NULL) {
                Py_DECREF(iter);
                Py_DECREF(key);
                return -1;
            }

            status = custom_PyDict_SetItem2(a, key, value);

#ifdef EBUG
            printf("status: %d\n", status);
            fflush(stdout);
#endif

            Py_DECREF(key);
            Py_DECREF(value);
            if (status < 0) {
                Py_DECREF(iter);
                return -1;
            }
        }
        Py_DECREF(iter);
        if (PyErr_Occurred())
            /* Iterator completed, via error */
            return -1;

#ifdef EBUG
        printf("custom_dict_merge post-for loop.\n");
        fflush(stdout);
#endif
    }
    ASSERT_CONSISTENT(a);
    return 0;
}

static int
dict_merge(PyObject *a, PyObject *b, int override,
        Py_ssize_t (*lookup)(PyDictObject *, PyObject *, Py_hash_t, PyObject **, int *),
        Py_ssize_t (*empty_slot)(PyDictKeysObject *keys, Py_hash_t hash, size_t *, int *),
        void (*build_idxs)(PyDictKeysObject *, PyDictKeyEntry *, Py_ssize_t))
{
#ifdef EBUG
    printf("\ndict_merge override: %d\n", override);
    fflush(stdout);
#endif

    CustomPyDictObject *mp, *other;
    Py_ssize_t i, n;
    PyDictKeyEntry *entry, *ep0;

    assert(0 <= override && override <= 2);

    /* We accept for the argument either a concrete dictionary object,
     * or an abstract "mapping" object.  For the former, we can do
     * things quite efficiently.  For the latter, we only require that
     * PyMapping_Keys() and PyObject_GetItem() be supported.
     */
    if (a == NULL || !PyDict_Check(a) || b == NULL) {
        PyErr_BadInternalCall();
        return -1;
    }
    mp = (CustomPyDictObject*)a;
    if (PyDict_Check(b) && (Py_TYPE(b)->tp_iter == (getiterfunc)dict_iter)) {
        other = (CustomPyDictObject*)b;
        if (other == mp || other->ma_used == 0)
            /* a.update(a) or a.update({}); nothing to do */
            return 0;
        if (mp->ma_used == 0) {
            /* Since the target dict is empty, PyDict_GetItem()
             * always returns NULL.  Setting override to 1
             * skips the unnecessary test.
             */
            override = 1;
            PyDictKeysObject *okeys = other->ma_keys;

            // If other is clean, combined, and just allocated, just clone it.
            if (other->ma_values == NULL &&
                    other->ma_used == okeys->dk_nentries &&
                    (DK_SIZE(okeys) == PyDict_MINSIZE ||
                     USABLE_FRACTION(DK_SIZE(okeys)/2) < other->ma_used)) {
                PyDictKeysObject *keys = clone_combined_dict_keys(other);
                if (keys == NULL) {
                    return -1;
                }

                dictkeys_decref(mp->ma_keys);
                mp->ma_keys = keys;
                if (mp->ma_values != NULL) {
                    if (mp->ma_values != empty_values) {
                        free_values(mp->ma_values);
                    }
                    mp->ma_values = NULL;
                }

                mp->ma_used = other->ma_used;
                mp->ma_version_tag = DICT_NEXT_VERSION();
                ASSERT_CONSISTENT(mp);

                if (_PyObject_GC_IS_TRACKED(other) && !_PyObject_GC_IS_TRACKED(mp)) {
                    /* Maintain tracking. */
                    _PyObject_GC_TRACK(mp);
                }

                return 0;
            }
        }
        /* Do one big resize at the start, rather than
         * incrementally resizing as we insert new items.  Expect
         * that there will be no (or few) overlapping keys.
         */
        if (USABLE_FRACTION(DK_SIZE(mp->ma_keys)) < other->ma_used) {
            if (dictresize(mp, estimate_log2_keysize(mp->ma_used + other->ma_used))) {
               return -1;
            }
        }
        ep0 = DK_ENTRIES(other->ma_keys);
        for (i = 0, n = other->ma_keys->dk_nentries; i < n; i++) {
            PyObject *key, *value;
            Py_hash_t hash;
            entry = &ep0[i];
            key = entry->me_key;
            hash = entry->me_hash;
            if (other->ma_values)
                value = other->ma_values[i];
            else
                value = entry->me_value;

            if (value != NULL) {
                int err = 0;
                Py_INCREF(key);
                Py_INCREF(value);
                if (override == 1)
                    err = insertdict(mp, key, hash, value);
                else {
                    err = custom_PyDict_Contains_KnownHash(a, key, hash);
                    if (err == 0) {
                        err = insertdict(mp, key, hash, value);
                    }
                    else if (err > 0) {
                        if (override != 0) {
                            _PyErr_SetKeyError(key);
                            Py_DECREF(value);
                            Py_DECREF(key);
                            return -1;
                        }
                        err = 0;
                    }
                }
                Py_DECREF(value);
                Py_DECREF(key);
                if (err != 0)
                    return -1;

                if (n != other->ma_keys->dk_nentries) {
                    PyErr_SetString(PyExc_RuntimeError,
                                    "dict mutated during update");
                    return -1;
                }
            }
        }
    }
    else {
        /* Do it the generic, slower way */
        PyObject *keys = PyMapping_Keys(b);
        PyObject *iter;
        PyObject *key, *value;
        int status;

        if (keys == NULL)
            /* Docstring says this is equivalent to E.keys() so
             * if E doesn't have a .keys() method we want
             * AttributeError to percolate up.  Might as well
             * do the same for any other error.
             */
            return -1;

        iter = PyObject_GetIter(keys);
        Py_DECREF(keys);
        if (iter == NULL)
            return -1;

        for (key = PyIter_Next(iter); key; key = PyIter_Next(iter)) {
#ifdef EBUG
            printf("dict_merge for loop iteration.\n");
#endif

            if (override != 1) {
                status = PyDict_Contains(a, key);

                if (status != 0) {
                    if (status > 0) {
                        if (override == 0) {
                            Py_DECREF(key);
                            continue;
                        }
                        _PyErr_SetKeyError(key);
                    }
                    Py_DECREF(key);
                    Py_DECREF(iter);
                    return -1;
                }
            }
            value = PyObject_GetItem(b, key);
            if (value == NULL) {
                Py_DECREF(iter);
                Py_DECREF(key);
                return -1;
            }
            status = custom_PyDict_SetItem(a, key, value, lookup, empty_slot, build_idxs);

#ifdef EBUG
            printf("status: %d\n", status);
#endif

            Py_DECREF(key);
            Py_DECREF(value);
            if (status < 0) {
                Py_DECREF(iter);
                return -1;
            }
        }
        Py_DECREF(iter);
        if (PyErr_Occurred())
            /* Iterator completed, via error */
            return -1;
    }
    ASSERT_CONSISTENT(a);
    return 0;
}

int
custom_PyDict_Merge2(PyObject *a, PyObject *b, int override)
{
#ifdef EBUG
    printf("called custom_PyDict_Merge2\n");
    fflush(stdout);
#endif

    /* XXX Deprecate override not in (0, 1). */
    return custom_dict_merge(a, b, override != 0);
}

int
custom_PyDict_Merge(PyObject *a, PyObject *b, int override,
        Py_ssize_t (*lookup)(PyDictObject *, PyObject *, Py_hash_t, PyObject **, int *),
        Py_ssize_t (*empty_slot)(PyDictKeysObject *keys, Py_hash_t hash, size_t *, int *),
        void (*build_idxs)(PyDictKeysObject *, PyDictKeyEntry *, Py_ssize_t))
{
#ifdef EBUG
    printf("called custom_PyDict_Merge\n");
#endif

    /* XXX Deprecate override not in (0, 1). */
    return dict_merge(a, b, override != 0, lookup, empty_slot, build_idxs);
}

/* Single-arg dict update; used by dict_update_common and operators. */
static int
custom_dict_update_arg(PyObject *self, PyObject *arg)
{
#ifdef EBUG
    printf("called custom_dict_update_arg\n");
    fflush(stdout);
#endif

    if (PyDict_CheckExact(arg)) {
        return custom_PyDict_Merge2(self, arg, 1);
    }
    _Py_IDENTIFIER(keys);
    PyObject *func;
    if (_PyObject_LookupAttrId(arg, &PyId_keys, &func) < 0) {
        return -1;
    }
    if (func != NULL) {
        Py_DECREF(func);
        return custom_PyDict_Merge2(self, arg, 1);
    }
    return PyDict_MergeFromSeq2(self, arg, 1);
}

/* Single-arg dict update; used by dict_update_common and operators. */
static int
dict_update_arg(PyObject *self, PyObject *arg,
        Py_ssize_t (*lookup)(PyDictObject *, PyObject *, Py_hash_t, PyObject **, int *),
        Py_ssize_t (*empty_slot)(PyDictKeysObject *keys, Py_hash_t hash, size_t *, int *),
        void (*build_idxs)(PyDictKeysObject *, PyDictKeyEntry *, Py_ssize_t))
{
#ifdef EBUG
    printf("called dict_update_arg\n");
#endif

    if (PyDict_CheckExact(arg)) {
        return custom_PyDict_Merge(self, arg, 1, lookup, empty_slot, build_idxs);
    }
    _Py_IDENTIFIER(keys);
    PyObject *func;
    if (_PyObject_LookupAttrId(arg, &PyId_keys, &func) < 0) {
        return -1;
    }
    if (func != NULL) {
        Py_DECREF(func);
        return custom_PyDict_Merge(self, arg, 1, lookup, empty_slot, build_idxs);
    }
    return PyDict_MergeFromSeq2(self, arg, 1);
}

static PyObject *
dict_or(PyObject *self, PyObject *other)
{
    if (!PyDict_Check(self) || !PyDict_Check(other)) {
        Py_RETURN_NOTIMPLEMENTED;
    }
    PyObject *new = PyDict_Copy(self);
    if (new == NULL) {
        return NULL;
    }
    if (/* dict_update_arg(new, other) */ 0) {
        Py_DECREF(new);
        return NULL;
    }
    return new;
}

static PyObject *
dict_ior(PyObject *self, PyObject *other)
{
    if (/* dict_update_arg(self, other) */ 0) {
        return NULL;
    }
    Py_INCREF(self);
    return self;
}

static PyNumberMethods dict_as_number = {
    .nb_or = dict_or,
    .nb_inplace_or = dict_ior,
};

/* Hack to implement "key in dict" */
static PySequenceMethods dict_as_sequence = {
    0,                          /* sq_length */
    0,                          /* sq_concat */
    0,                          /* sq_repeat */
    0,                          /* sq_item */
    0,                          /* sq_slice */
    0,                          /* sq_ass_item */
    0,                          /* sq_ass_slice */
    PyDict_Contains,            /* sq_contains */
    0,                          /* sq_inplace_concat */
    0,                          /* sq_inplace_repeat */
};

static Py_ssize_t
custom_dict_length(CustomPyDictObject *mp)
{
    return mp->ma_used;
}

static Py_ssize_t
dict_length(PyDictObject *mp)
{
    return mp->ma_used;
}

static PyObject *
custom_dict_subscript(CustomPyDictObject *mp, PyObject *key)
{
    Py_ssize_t ix = 0;
    Py_hash_t hash;
    PyObject *value = key;

    if (!PyUnicode_CheckExact(key) ||
        (hash = ((PyASCIIObject *) key)->hash) == -1) {
        hash = PyObject_Hash(key);
        if (hash == -1)
            return NULL;
    }
    // ix = _Py_dict_lookup(mp, key, hash, &value, &num_cmps);
    if (ix == DKIX_ERROR)
        return NULL;
    if (ix == DKIX_EMPTY || value == NULL) {
        if (!PyDict_CheckExact(mp)) {
            /* Look up __missing__ method if we're a subclass. */
            PyObject *missing, *res;
            _Py_IDENTIFIER(__missing__);
            missing = _PyObject_LookupSpecial((PyObject *)mp, &PyId___missing__);
            if (missing != NULL) {
                res = PyObject_CallOneArg(missing, key);
                Py_DECREF(missing);
                return res;
            }
            else if (PyErr_Occurred())
                return NULL;
        }
        _PyErr_SetKeyError(key);
        return NULL;
    }
    Py_INCREF(value);
    return value;
}

static PyObject *
dict_subscript(PyDictObject *mp, PyObject *key)
{
    Py_ssize_t ix = 0;
    Py_hash_t hash;
    PyObject *value = key;

    if (!PyUnicode_CheckExact(key) ||
        (hash = ((PyASCIIObject *) key)->hash) == -1) {
        hash = PyObject_Hash(key);
        if (hash == -1)
            return NULL;
    }
    // ix = _Py_dict_lookup(mp, key, hash, &value, &num_cmps);
    if (ix == DKIX_ERROR)
        return NULL;
    if (ix == DKIX_EMPTY || value == NULL) {
        if (!PyDict_CheckExact(mp)) {
            /* Look up __missing__ method if we're a subclass. */
            PyObject *missing, *res;
            _Py_IDENTIFIER(__missing__);
            missing = _PyObject_LookupSpecial((PyObject *)mp, &PyId___missing__);
            if (missing != NULL) {
                res = PyObject_CallOneArg(missing, key);
                Py_DECREF(missing);
                return res;
            }
            else if (PyErr_Occurred())
                return NULL;
        }
        _PyErr_SetKeyError(key);
        return NULL;
    }
    Py_INCREF(value);
    return value;
}

static int
custom_dict_ass_sub(CustomPyDictObject *mp, PyObject *v, PyObject *w)
{
    if (w == NULL)
        return PyDict_DelItem((PyObject *)mp, v);
    else
        return DKIX_ERROR; // custom_PyDict_SetItem((PyObject *)mp, v, w);
}

static int
dict_ass_sub(PyDictObject *mp, PyObject *v, PyObject *w)
{
    if (w == NULL)
        return PyDict_DelItem((PyObject *)mp, v);
    else
        return DKIX_ERROR; // custom_PyDict_SetItem((PyObject *)mp, v, w);
}

static PyMappingMethods dict_as_mapping = {
    (lenfunc)dict_length, /*mp_length*/
    (binaryfunc)dict_subscript, /*mp_subscript*/
    (objobjargproc)dict_ass_sub, /*mp_ass_subscript*/
};

PyDoc_STRVAR(dictionary_doc,
"dict() -> new empty dictionary\n"
"dict(mapping) -> new dictionary initialized from a mapping object's\n"
"    (key, value) pairs\n"
"dict(iterable) -> new dictionary initialized as if via:\n"
"    d = {}\n"
"    for k, v in iterable:\n"
"        d[k] = v\n"
"dict(**kwargs) -> new dictionary initialized with the name=value pairs\n"
"    in the keyword argument list.  For example:  dict(one=1, two=2)");