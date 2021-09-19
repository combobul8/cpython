#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <structmember.h>
#include "stringlib/eq.h"    // unicode_eq()
#include "Custom.h"

#define IS_POWER_OF_2(x) (((x) & (x-1)) == 0)
#define PERTURB_SHIFT 5
#define USABLE_FRACTION(n) (((n) << 1)/3)

static PyModuleDef custommodule = {
    PyModuleDef_HEAD_INIT,
    .m_name = "custom",
    .m_doc = "Example module that creates an extension type.",
    .m_size = -1,
};

static void
Custom_dealloc(CustomObject *self)
{
    Py_XDECREF(self->first);
    Py_XDECREF(self->last);
    Py_TYPE(self)->tp_free((PyObject *) self);
}

static PyObject *
Custom_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    CustomObject *self = (CustomObject *) type->tp_alloc(type, 0);
    if (self == NULL) {
        return NULL;
    }
    self->first = PyUnicode_FromString("");
    if (self->first == NULL) {
        Py_DECREF(self);
        return NULL;
    }
    self->last = PyUnicode_FromString("");
    if (self->last == NULL) {
        Py_DECREF(self);
        return NULL;
    }
    self->number = 0;
    return (PyObject *) self;
}

static int
Custom_init(CustomObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"first", "last", "number", NULL};
    PyObject *first = NULL, *last = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|OOi", kwlist,
                                     &first, &last,
                                     &self->number))
        return -1;

    if (first) {
        PyObject *tmp = self->first;
        Py_INCREF(first);
        self->first = first;
        Py_XDECREF(tmp);
    }
    if (last) {
        PyObject *tmp = self->last;
        Py_INCREF(last);
        self->last = last;
        Py_XDECREF(tmp);
    }
    return 0;
}

static PyMemberDef Custom_members[] = {
    {"first", T_OBJECT_EX, offsetof(CustomObject, first), 0,
     "first name"},
    {"last", T_OBJECT_EX, offsetof(CustomObject, last), 0,
     "last name"},
    {"number", T_INT, offsetof(CustomObject, number), 0,
     "custom number"},
    {NULL}  /* Sentinel */
};

static PyObject *
Custom_name(CustomObject *self, PyObject *Py_UNUSED(ignored))
{
    if (self->first == NULL) {
        PyErr_SetString(PyExc_AttributeError, "first");
        return NULL;
    }
    if (self->last == NULL) {
        PyErr_SetString(PyExc_AttributeError, "last");
        return NULL;
    }
    return PyUnicode_FromFormat("%S %S", self->first, self->last);
}

static PyMethodDef Custom_methods[] = {
    {"name", (PyCFunction) Custom_name, METH_NOARGS,
     "Return the name, combining the first and last name"
    },
    {NULL}  /* Sentinel */
};

static PyTypeObject CustomType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "custom.Custom",
    .tp_doc = "Custom objects",
    .tp_basicsize = sizeof(CustomObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = Custom_new,
    .tp_init = (initproc) Custom_init,
    .tp_dealloc = (destructor) Custom_dealloc,
    .tp_members = Custom_members,
    .tp_methods = Custom_methods,
};

PyMODINIT_FUNC
PyInit_custom(void)
{
    PyObject *m;
    if (PyType_Ready(&CustomType) < 0)
        return NULL;

    m = PyModule_Create(&custommodule);
    if (m == NULL)
        return NULL;

    Py_INCREF(&CustomType);
    if (PyModule_AddObject(m, "Custom", (PyObject *) &CustomType) < 0) {
        Py_DECREF(&CustomType);
        Py_DECREF(m);
        return NULL;
    }

    return m;
}

/* free_keys_object */
/*
static inline void
dictkeys_incref(PyDictKeysObject *dk)
{
#ifdef Py_REF_DEBUG
    _Py_RefTotal++;
#endif
    dk->dk_refcnt++;
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
*/

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

Py_ssize_t _Py_HOT_FUNCTION
_Py_dict_lookup(PyDictObject *mp, PyObject *key, Py_hash_t hash, PyObject **value_addr)
{
    PyDictKeysObject *dk;
start:
    dk = mp->ma_keys;
    DictKeysKind kind = dk->dk_kind;
    PyDictKeyEntry *ep0 = DK_ENTRIES(dk);
    size_t mask = DK_MASK(dk);
    size_t perturb = hash;
    size_t i = (size_t)hash & mask;
    Py_ssize_t ix;
    if (PyUnicode_CheckExact(key) && kind != DICT_KEYS_GENERAL) {
        /* Strings only */
        for (;;) {
            ix = dictkeys_get_index(mp->ma_keys, i);
            if (ix >= 0) {
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
            if (ix >= 0) {
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

static inline void //PyThreadState*
_PyRuntimeState_GetThreadState(_PyRuntimeState *runtime)
{
#ifdef EXPERIMENTAL_ISOLATED_SUBINTERPRETERS
    return _PyThreadState_GetTSS();
#else
    return (PyThreadState*)_Py_atomic_load_relaxed(&runtime->gilstate.tstate_current);
#endif
}

static inline PyThreadState*
_PyThreadState_GET(void)
{
#ifdef EXPERIMENTAL_ISOLATED_SUBINTERPRETERS
    return _PyThreadState_GetTSS();
#else
    return _PyRuntimeState_GetThreadState(&_PyRuntime);
#endif
}

static inline PyInterpreterState* _PyInterpreterState_GET(void) {
    PyThreadState *tstate = _PyThreadState_GET();
#ifdef Py_DEBUG
    _Py_EnsureTstateNotNULL(tstate);
#endif
    return tstate->interp;
}

static struct _Py_dict_state *
get_dict_state(void)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    return &interp->dict_state;
}

static PyDictKeysObject*
new_keys_object(uint8_t log2_size)
{
    PyDictKeysObject *dk;
    Py_ssize_t es, usable;

    assert(log2_size >= PyDict_LOG_MINSIZE);

    usable = USABLE_FRACTION(1<<log2_size);
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

    struct _Py_dict_state *state = get_dict_state();
#ifdef Py_DEBUG
    // new_keys_object() must not be called after _PyDict_Fini()
    assert(state->keys_numfree != -1);
#endif
    if (log2_size == PyDict_LOG_MINSIZE && state->keys_numfree > 0) {
        dk = state->keys_free_list[--state->keys_numfree];
    }
    else
    {
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

static int
dictresize(PyDictObject *mp, uint8_t log2_newsize)
{
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

    build_indices(mp->ma_keys, newentries, numentries);
    mp->ma_keys->dk_usable -= numentries;
    mp->ma_keys->dk_nentries = numentries;
    return 0;
}

static int
insertion_resize(PyDictObject *mp)
{
    return dictresize(mp, calculate_log2_keysize(GROWTH_RATE(mp)));
}

static int
insertdict(PyDictObject *mp, PyObject *key, Py_hash_t hash, PyObject *value)
{
    PyObject *old_value;
    PyDictKeyEntry *ep;

    Py_INCREF(key);
    Py_INCREF(value);
    if (mp->ma_values != NULL && !PyUnicode_CheckExact(key)) {
        if (insertion_resize(mp) < 0)
            goto Fail;
    }

    Py_ssize_t ix = _Py_dict_lookup(mp, key, hash, &old_value);
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
        Py_ssize_t hashpos = find_empty_slot(mp->ma_keys, hash);
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