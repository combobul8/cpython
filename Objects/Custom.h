#include <windows.h>

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

PyDoc_STRVAR(getitem__doc__, "x.__getitem__(y) <==> x[y]");

PyDoc_STRVAR(sizeof__doc__,
"D.__sizeof__() -> size of D in memory, in bytes");

#define DICT_GET_METHODDEF    \
    {"get", (PyCFunction)(void(*)(void))dict_get, METH_FASTCALL, dict_get__doc__},

PyDoc_STRVAR(dict_get__doc__,
"get($self, key, default=None, /)\n"
"--\n"
"\n"
"Return the value for key if key is in the dictionary, else default.");

#define DICT_SETDEFAULT_METHODDEF    \
    {"setdefault", (PyCFunction)(void(*)(void))dict_setdefault, METH_FASTCALL, dict_setdefault__doc__},

PyDoc_STRVAR(dict_setdefault__doc__,
"setdefault($self, key, default=None, /)\n"
"--\n"
"\n"
"Insert key with a value of default if key is not in the dictionary.\n"
"\n"
"Return the value for key if key is in the dictionary, else default.");

#define DICT_POP_METHODDEF    \
    {"pop", (PyCFunction)(void(*)(void))dict_pop, METH_FASTCALL, dict_pop__doc__},

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

typedef struct {
    PyObject_HEAD
    PyDictObject *di_dict; /* Set to NULL when iterator is exhausted */
    Py_ssize_t di_used;
    Py_ssize_t di_pos;
    PyObject* di_result; /* reusable result tuple for iteritems */
    Py_ssize_t len;
} dictiterobject;

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

struct _Py_dict_state {
    /* Dictionary reuse scheme to save calls to malloc and free */
    PyDictObject *free_list[PyDict_MAXFREELIST];
    int numfree;
    PyDictKeysObject *keys_free_list[PyDict_MAXFREELIST];
    int keys_numfree;
};

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
#define DKIX_EMPTY (-1)
#define DKIX_ERROR (-3)