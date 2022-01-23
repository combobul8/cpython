#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <pyport.h>
#include <structmember.h>
#include "Custom.h"

#define IS_POWER_OF_2(x) (((x) & (x-1)) == 0)
#define USABLE_FRACTION(n) (((n) << 1)/3)

// Fast inlined version of PyType_HasFeature()
static inline int
_PyType_HasFeature(PyTypeObject *type, unsigned long feature) {
    return ((type->tp_flags & feature) != 0);
}

// Fast inlined version of PyType_IS_GC()
#define _PyType_IS_GC(t) _PyType_HasFeature((t), Py_TPFLAGS_HAVE_GC)

// Fast inlined version of PyObject_IS_GC()
static inline int
_PyObject_IS_GC(PyObject *obj)
{
    return (PyType_IS_GC(Py_TYPE(obj))
            && (Py_TYPE(obj)->tp_is_gc == NULL
                || Py_TYPE(obj)->tp_is_gc(obj)));
}

#define _Py_AS_GC(o) ((PyGC_Head *)(o)-1)

PyAPI_DATA(_PyRuntimeState) _PyRuntime;

/* The (N-2) most significant bits contain the real address. */
#define _PyGC_PREV_SHIFT           (2)
#define _PyGC_PREV_MASK            (((uintptr_t) -1) << _PyGC_PREV_SHIFT)
#define _PyGCHead_SET_NEXT(g, p) ((g)->_gc_next = (uintptr_t)(p))
#define _PyGCHead_SET_PREV(g, p) do { \
    assert(((uintptr_t)p & ~_PyGC_PREV_MASK) == 0); \
    (g)->_gc_prev = ((g)->_gc_prev & ~_PyGC_PREV_MASK) \
        | ((uintptr_t)(p)); \
    } while (0)

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

static PyModuleDef custommodule2 = {
    PyModuleDef_HEAD_INIT,
    .m_name = "custom2",
    .m_doc = "Example module that creates an extension type.",
    .m_size = -1,
};

static int
custom_dict_update_common(PyObject *self, PyObject *args, PyObject *kwds, const char *methname,
        Py_ssize_t (*lookup)(CustomPyDictObject *, PyObject *, Py_hash_t, PyObject **, int *),
        Py_ssize_t (*empty_slot)(PyDictKeysObject *, Py_hash_t, size_t *, int *),
        void (*build_idxs)(CustomPyDictObject *, PyDictKeyEntry *, Py_ssize_t))
{
    printf("called custom_dict_update_common\n");
    fflush(stdout);
#ifdef EBUG
#endif

    PyObject *arg = NULL;
    int result = 0;

    if (!PyArg_UnpackTuple(args, methname, 0, 1, &arg)) {
        result = -1;
    }
    else if (arg != NULL) {
        result = custom_dict_update_arg(self, arg, lookup, empty_slot, build_idxs);
    }

    if (result == 0 && kwds != NULL) {
        if (PyArg_ValidateKeywordArguments(kwds)) {
            result = PyDict_Merge(self, kwds, 1);
        }
        else
            result = -1;
    }
    return result;
}

static int
dict_update_common(PyObject *self, PyObject *args, PyObject *kwds,
                   const char *methname,
                   Py_ssize_t (*lookup)(PyDictObject *, PyObject *, Py_hash_t, PyObject **, int *),
                   Py_ssize_t (*empty_slot)(PyDictKeysObject *keys, Py_hash_t hash, size_t *, int *),
                   void (*build_idxs)(PyDictKeysObject *, PyDictKeyEntry *, Py_ssize_t))
{
#ifdef EBUG
    printf("called dict_update_common\n");
#endif

    PyObject *arg = NULL;
    int result = 0;

    if (!PyArg_UnpackTuple(args, methname, 0, 1, &arg)) {
        result = -1;
    }
    else if (arg != NULL) {
        result = dict_update_arg(self, arg, lookup, empty_slot, build_idxs);
    }

    if (result == 0 && kwds != NULL) {
        if (PyArg_ValidateKeywordArguments(kwds)) {
            result = PyDict_Merge(self, kwds, 1);
        }
        else
            result = -1;
    }
    return result;
}

#define ORIG_LOOKUP

static int
custom_dict_init(PyObject *self, PyObject *args, PyObject *kwds)
{
#ifdef EBUG
    printf("called custom_dict_init\n");
    fflush(stdout);
#endif

#ifdef ORIG_LOOKUP
    lookup = rprobe_Py_dict_lookup;
    empty_slot = find_empty_slot;
    build_idxs = build_indices;
    resize = dictresize;
#else
    lookup = custom_Py_dict_lookup;
    empty_slot = custom_find_empty_slot;
    build_idxs = custom_build_indices;
    resize = customdictresize;
#endif

    return custom_dict_update_common(self, args, kwds, "dict", lookup, empty_slot, build_idxs);
}

/*[clinic input]

@coexist
dict.__contains__

  key: object
  /

True if the dictionary has the specified key, else False.
[clinic start generated code]*/

static PyObject *
custom_dict___contains__(CustomPyDictObject *self, PyObject *key)
/*[clinic end generated code: output=a3d03db709ed6e6b input=fe1cb42ad831e820]*/
{
    printf("called dict__contains__.\n");

    register CustomPyDictObject *mp = self;
    Py_hash_t hash;
    Py_ssize_t ix = 0;
    PyObject *value = key;
    // int num_cmps;   // currently unused

    if (!PyUnicode_CheckExact(key) ||
        (hash = ((PyASCIIObject *) key)->hash) == -1) {
        hash = PyObject_Hash(key);
        if (hash == -1)
            return NULL;
    }
    // ix = _Py_dict_lookup(mp, key, hash, &value, &num_cmps);
    if (ix == DKIX_ERROR)
        return NULL;
    if (ix == DKIX_EMPTY || value == NULL)
        Py_RETURN_FALSE;
    Py_RETURN_TRUE;
}

/*[clinic input]

@coexist
dict.__contains__

  key: object
  /

True if the dictionary has the specified key, else False.
[clinic start generated code]*/

static PyObject *
dict___contains__(PyDictObject *self, PyObject *key)
/*[clinic end generated code: output=a3d03db709ed6e6b input=fe1cb42ad831e820]*/
{
    printf("called dict__contains__.\n");

    register PyDictObject *mp = self;
    Py_hash_t hash;
    Py_ssize_t ix = 0;
    PyObject *value = key;
    // int num_cmps;   // currently unused

    if (!PyUnicode_CheckExact(key) ||
        (hash = ((PyASCIIObject *) key)->hash) == -1) {
        hash = PyObject_Hash(key);
        if (hash == -1)
            return NULL;
    }
    // ix = _Py_dict_lookup(mp, key, hash, &value, &num_cmps);
    if (ix == DKIX_ERROR)
        return NULL;
    if (ix == DKIX_EMPTY || value == NULL)
        Py_RETURN_FALSE;
    Py_RETURN_TRUE;
}

Py_ssize_t
_Custom_PyDict_SizeOf(CustomPyDictObject *mp)
{
    Py_ssize_t size, usable, res;

    size = DK_SIZE(mp->ma_keys);
    usable = USABLE_FRACTION(size);

    res = _PyObject_SIZE(Py_TYPE(mp));
    if (mp->ma_values)
        res += usable * sizeof(PyObject*);
    /* If the dictionary is split, the keys portion is accounted-for
       in the type object. */
    if (mp->ma_keys->dk_refcnt == 1)
        res += (sizeof(PyDictKeysObject)
                + DK_IXSIZE(mp->ma_keys) * size
                + sizeof(PyDictKeyEntry) * usable);
    return res;
}

static PyObject *
custom_dict_sizeof(CustomPyDictObject *mp, PyObject *Py_UNUSED(ignored))
{
    return PyLong_FromSsize_t(_Custom_PyDict_SizeOf(mp));
}

static PyObject *
dict_sizeof(PyDictObject *mp, PyObject *Py_UNUSED(ignored))
{
    return PyLong_FromSsize_t(_PyDict_SizeOf(mp));
}

/*[clinic input]
dict.get

    key: object
    default: object = None
    /

Return the value for key if key is in the dictionary, else default.
[clinic start generated code]*/

static PyObject *
custom_dict_get_impl(CustomPyDictObject *self, PyObject *key, PyObject *default_value,
        Py_ssize_t (*lookup)(CustomPyDictObject *, PyObject *, Py_hash_t, PyObject **, int *),
        int *success)
/*[clinic end generated code: output=bba707729dee05bf input=279ddb5790b6b107]*/
{
    PyObject *val = NULL;
    Py_hash_t hash;
    Py_ssize_t ix;

    hash = custom_PyObject_Hash(key);

    if (!PyUnicode_CheckExact(key) ||
        (hash = ((PyASCIIObject *) key)->hash) == -1) {
        hash = PyObject_Hash(key);
        if (hash == -1)
            return NULL;
    }

    int num_cmps;
    ix = lookup(self, key, hash, &val, &num_cmps);

    assert(PyInt_Check(val) == 1);

#ifdef EBUG
    printf("get_impl %s ix: %lld.\n", PyUnicode_AsUTF8(key), ix);
    fflush(stdout);
#endif

    if (ix == DKIX_ERROR)
        return NULL;
    if (ix == DKIX_EMPTY || val == NULL)
        val = default_value;
    Py_INCREF(val);

    *success = ix != DKIX_EMPTY;
    return val;
}

/*[clinic input]
dict.get

    key: object
    default: object = None
    /

Return the value for key if key is in the dictionary, else default.
[clinic start generated code]*/

static PyObject *
dict_get_impl(PyDictObject *self, PyObject *key, PyObject *default_value,
        Py_ssize_t (*lookup)(PyDictObject *, PyObject *, Py_hash_t, PyObject **, int *))
/*[clinic end generated code: output=bba707729dee05bf input=279ddb5790b6b107]*/
{
    PyObject *val = NULL;
    Py_hash_t hash;
    Py_ssize_t ix;

    hash = custom_PyObject_Hash(key);

    if (!PyUnicode_CheckExact(key) ||
        (hash = ((PyASCIIObject *) key)->hash) == -1) {
        hash = PyObject_Hash(key);
        if (hash == -1)
            return NULL;
    }

    int num_cmps;
    ix = lookup(self, key, hash, &val, &num_cmps);

    printf("num_cmps: %d.\n", num_cmps);
    fflush(stdout);
#ifdef EBUG
#endif

    if (ix == DKIX_ERROR)
        return NULL;
    if (ix == DKIX_EMPTY || val == NULL)
        val = default_value;
    Py_INCREF(val);

    return val;
}

static PyObject *
custom_dict_setdefault_impl(CustomPyDictObject *self, PyObject *key,
                     PyObject *default_value)
/*[clinic end generated code: output=f8c1101ebf69e220 input=0f063756e815fd9d]*/
{
    PyObject *val;

    val = PyDict_SetDefault((PyObject *)self, key, default_value);
    Py_XINCREF(val);
    return val;
}

static PyObject *
dict_setdefault_impl(PyDictObject *self, PyObject *key,
                     PyObject *default_value)
/*[clinic end generated code: output=f8c1101ebf69e220 input=0f063756e815fd9d]*/
{
    PyObject *val;

    val = PyDict_SetDefault((PyObject *)self, key, default_value);
    Py_XINCREF(val);
    return val;
}

/*[clinic input]
dict.setdefault

    key: object
    default: object = None
    /

Insert key with a value of default if key is not in the dictionary.

Return the value for key if key is in the dictionary, else default.
[clinic start generated code]*/

static PyObject *
custom_dict_setdefault(CustomPyDictObject *self, PyObject *const *args, Py_ssize_t nargs)
{
    PyObject *return_value = NULL;
    PyObject *key;
    PyObject *default_value = Py_None;

    if (!_PyArg_CheckPositional("setdefault", nargs, 1, 2)) {
        goto exit;
    }
    key = args[0];
    if (nargs < 2) {
        goto skip_optional;
    }
    default_value = args[1];
skip_optional:
    return_value = custom_dict_setdefault_impl(self, key, default_value);

exit:
    return return_value;
}

/*[clinic input]
dict.setdefault

    key: object
    default: object = None
    /

Insert key with a value of default if key is not in the dictionary.

Return the value for key if key is in the dictionary, else default.
[clinic start generated code]*/

static PyObject *
dict_setdefault(PyDictObject *self, PyObject *const *args, Py_ssize_t nargs)
{
    PyObject *return_value = NULL;
    PyObject *key;
    PyObject *default_value = Py_None;

    if (!_PyArg_CheckPositional("setdefault", nargs, 1, 2)) {
        goto exit;
    }
    key = args[0];
    if (nargs < 2) {
        goto skip_optional;
    }
    default_value = args[1];
skip_optional:
    return_value = dict_setdefault_impl(self, key, default_value);

exit:
    return return_value;
}

/*[clinic input]
dict.pop

    key: object
    default: object = NULL
    /

D.pop(k[,d]) -> v, remove specified key and return the corresponding value.

If the key is not found, return the default if given; otherwise,
raise a KeyError.
[clinic start generated code]*/

static PyObject *
custom_dict_pop_impl(CustomPyDictObject *self, PyObject *key, PyObject *default_value)
/*[clinic end generated code: output=3abb47b89f24c21c input=e221baa01044c44c]*/
{
    return _PyDict_Pop((PyObject*)self, key, default_value);
}

/*[clinic input]
dict.pop

    key: object
    default: object = NULL
    /

D.pop(k[,d]) -> v, remove specified key and return the corresponding value.

If the key is not found, return the default if given; otherwise,
raise a KeyError.
[clinic start generated code]*/

static PyObject *
dict_pop_impl(PyDictObject *self, PyObject *key, PyObject *default_value)
/*[clinic end generated code: output=3abb47b89f24c21c input=e221baa01044c44c]*/
{
    return _PyDict_Pop((PyObject*)self, key, default_value);
}

static PyObject *
custom_dict_pop(CustomPyDictObject *self, PyObject *const *args, Py_ssize_t nargs)
{
    PyObject *return_value = NULL;
    PyObject *key;
    PyObject *default_value = NULL;

    if (!_PyArg_CheckPositional("pop", nargs, 1, 2)) {
        goto exit;
    }
    key = args[0];
    if (nargs < 2) {
        goto skip_optional;
    }
    default_value = args[1];
skip_optional:
    return_value = custom_dict_pop_impl(self, key, default_value);

exit:
    return return_value;
}

static PyObject *
dict_pop(PyDictObject *self, PyObject *const *args, Py_ssize_t nargs)
{
    PyObject *return_value = NULL;
    PyObject *key;
    PyObject *default_value = NULL;

    if (!_PyArg_CheckPositional("pop", nargs, 1, 2)) {
        goto exit;
    }
    key = args[0];
    if (nargs < 2) {
        goto skip_optional;
    }
    default_value = args[1];
skip_optional:
    return_value = dict_pop_impl(self, key, default_value);

exit:
    return return_value;
}

static PyObject *
custom_dict_get(CustomPyDictObject *self, PyObject *const *args, Py_ssize_t nargs)
{
#ifdef EBUG
    printf("\ncalled custom_dict_get\n");
    fflush(stdout);
#endif

    PyObject *return_value = NULL;
    PyObject *key;
    PyObject *default_value = Py_None;

    if (!_PyArg_CheckPositional("get", nargs, 1, 2)) {
        printf("dict_get first if\n");
        goto exit;
    }
    key = args[0];
    if (nargs < 2) {
        goto skip_optional;
    }
    default_value = args[1];
skip_optional:
    int success;
    return_value = custom_dict_get_impl(self, key, default_value, lookup, &success);
    /* if (success) {
        printf("custom_dict_get returning %ld.\n", PyLong_AsLong(return_value));
        fflush(stdout);
    } */

exit:
    return return_value;
}

/* Search index of hash table from offset of entry table */
static Py_ssize_t
lookdict_index(PyDictKeysObject *k, Py_hash_t hash, Py_ssize_t index)
{
    size_t mask = DK_MASK(k);
    size_t perturb = (size_t)hash;
    size_t i = (size_t)hash & mask;

    for (;;) {
        Py_ssize_t ix = dictkeys_get_index(k, i);
        if (ix == index) {
            return i;
        }
        if (ix == DKIX_EMPTY) {
            return DKIX_EMPTY;
        }
        perturb >>= PERTURB_SHIFT;
        i = mask & (i*5 + perturb + 1);
    }
    Py_UNREACHABLE();
}

/*[clinic input]
dict.popitem

Remove and return a (key, value) pair as a 2-tuple.

Pairs are returned in LIFO (last-in, first-out) order.
Raises KeyError if the dict is empty.
[clinic start generated code]*/

static PyObject *
custom_dict_popitem_impl(CustomPyDictObject *self)
/*[clinic end generated code: output=e65fcb04420d230d input=1c38a49f21f64941]*/
{
    Py_ssize_t i, j;
    PyDictKeyEntry *ep0, *ep;
    PyObject *res;

    /* Allocate the result tuple before checking the size.  Believe it
     * or not, this allocation could trigger a garbage collection which
     * could empty the dict, so if we checked the size first and that
     * happened, the result would be an infinite loop (searching for an
     * entry that no longer exists).  Note that the usual popitem()
     * idiom is "while d: k, v = d.popitem()". so needing to throw the
     * tuple away if the dict *is* empty isn't a significant
     * inefficiency -- possible, but unlikely in practice.
     */
    res = PyTuple_New(2);
    if (res == NULL)
        return NULL;
    if (self->ma_used == 0) {
        Py_DECREF(res);
        PyErr_SetString(PyExc_KeyError, "popitem(): dictionary is empty");
        return NULL;
    }
    /* Convert split table to combined table */
    if (self->ma_keys->dk_kind == DICT_KEYS_SPLIT) {
        // DictHelpersImpl helpers = { custom_Py_dict_lookup, custom_find_empty_slot, custom_build_indices };
        if (customdictresize(self, DK_LOG_SIZE(self->ma_keys), custom_Py_dict_lookup, custom_find_empty_slot, custom_build_indices)) {
            Py_DECREF(res);
            return NULL;
        }
    }
    self->ma_keys->dk_version = 0;

    /* Pop last item */
    ep0 = DK_ENTRIES(self->ma_keys);
    i = self->ma_keys->dk_nentries - 1;
    while (i >= 0 && ep0[i].me_value == NULL) {
        i--;
    }
    assert(i >= 0);

    ep = &ep0[i];
    j = lookdict_index(self->ma_keys, ep->me_hash, i);
    assert(j >= 0);
    assert(dictkeys_get_index(self->ma_keys, j) == i);
    dictkeys_set_index(self->ma_keys, j, DKIX_DUMMY);

    PyTuple_SET_ITEM(res, 0, ep->me_key);
    PyTuple_SET_ITEM(res, 1, ep->me_value);
    ep->me_key = NULL;
    ep->me_value = NULL;
    /* We can't dk_usable++ since there is DKIX_DUMMY in indices */
    self->ma_keys->dk_nentries = i;
    self->ma_used--;
    self->ma_version_tag = DICT_NEXT_VERSION();
    ASSERT_CONSISTENT(self);
    return res;
}

/*[clinic input]
dict.popitem

Remove and return a (key, value) pair as a 2-tuple.

Pairs are returned in LIFO (last-in, first-out) order.
Raises KeyError if the dict is empty.
[clinic start generated code]*/

static PyObject *
dict_popitem_impl(CustomPyDictObject *self)
/*[clinic end generated code: output=e65fcb04420d230d input=1c38a49f21f64941]*/
{
    Py_ssize_t i, j;
    PyDictKeyEntry *ep0, *ep;
    PyObject *res;

    /* Allocate the result tuple before checking the size.  Believe it
     * or not, this allocation could trigger a garbage collection which
     * could empty the dict, so if we checked the size first and that
     * happened, the result would be an infinite loop (searching for an
     * entry that no longer exists).  Note that the usual popitem()
     * idiom is "while d: k, v = d.popitem()". so needing to throw the
     * tuple away if the dict *is* empty isn't a significant
     * inefficiency -- possible, but unlikely in practice.
     */
    res = PyTuple_New(2);
    if (res == NULL)
        return NULL;
    if (self->ma_used == 0) {
        Py_DECREF(res);
        PyErr_SetString(PyExc_KeyError, "popitem(): dictionary is empty");
        return NULL;
    }
    /* Convert split table to combined table */
    if (self->ma_keys->dk_kind == DICT_KEYS_SPLIT) {
        if (dictresize(self, DK_LOG_SIZE(self->ma_keys))) {
            Py_DECREF(res);
            return NULL;
        }
    }
    self->ma_keys->dk_version = 0;

    /* Pop last item */
    ep0 = DK_ENTRIES(self->ma_keys);
    i = self->ma_keys->dk_nentries - 1;
    while (i >= 0 && ep0[i].me_value == NULL) {
        i--;
    }
    assert(i >= 0);

    ep = &ep0[i];
    j = lookdict_index(self->ma_keys, ep->me_hash, i);
    assert(j >= 0);
    assert(dictkeys_get_index(self->ma_keys, j) == i);
    dictkeys_set_index(self->ma_keys, j, DKIX_DUMMY);

    PyTuple_SET_ITEM(res, 0, ep->me_key);
    PyTuple_SET_ITEM(res, 1, ep->me_value);
    ep->me_key = NULL;
    ep->me_value = NULL;
    /* We can't dk_usable++ since there is DKIX_DUMMY in indices */
    self->ma_keys->dk_nentries = i;
    self->ma_used--;
    self->ma_version_tag = DICT_NEXT_VERSION();
    ASSERT_CONSISTENT(self);
    return res;
}

static PyObject *
custom_dict_popitem(CustomPyDictObject *self, PyObject *Py_UNUSED(ignored))
{
    return custom_dict_popitem_impl(self);
}

static PyObject *
dict_popitem(CustomPyDictObject *self, PyObject *Py_UNUSED(ignored))
{
    return dict_popitem_impl(self);
}

static PyObject *
dictkeys_new(PyObject *dict, PyObject *Py_UNUSED(ignored))
{
    return _PyDictView_New(dict, &PyDictKeys_Type);
}

static PyObject *
dictitems_new(PyObject *dict, PyObject *Py_UNUSED(ignored))
{
    return _PyDictView_New(dict, &PyDictItems_Type);
}

static PyObject *
dictvalues_new(PyObject *dict, PyObject *Py_UNUSED(ignored))
{
    return _PyDictView_New(dict, &PyDictValues_Type);
}

/* Note: dict.update() uses the METH_VARARGS|METH_KEYWORDS calling convention.
   Using METH_FASTCALL|METH_KEYWORDS would make dict.update(**dict2) calls
   slower, see the issue #29312. */
static PyObject *
custom_dict_update(PyObject *self, PyObject *args, PyObject *kwds)
{
#ifdef EBUG
    printf("\ncalled custom_dict_update\n");
    fflush(stdout);
#endif

    int dict_update_common_rv;
    if ((dict_update_common_rv = custom_dict_update_common(self, args, kwds, "update", lookup, empty_slot, build_idxs)) != -1) {
#ifdef EBUG
        printf("dict_update_common_rv if: %d\n", dict_update_common_rv);
#endif

        Py_RETURN_NONE;
    }

#ifdef EBUG
    printf("dict_update_common_rv: %d\n", dict_update_common_rv);
#endif

    return NULL;
}

/* Internal version of dict.from_keys().  It is subclass-friendly. */
PyObject *
_Custom_PyDict_FromKeys(PyObject *cls, PyObject *iterable, PyObject *value)
{
    PyObject *it;       /* iter(iterable) */
    PyObject *key;
    PyObject *d;
    int status = 0;

    d = _PyObject_CallNoArg(cls);
    if (d == NULL)
        return NULL;

    // DictHelpersImpl helpers = { custom_Py_dict_lookup, custom_find_empty_slot, custom_build_indices };

    if (PyDict_CheckExact(d) && ((CustomPyDictObject *)d)->ma_used == 0) {
        if (PyDict_CheckExact(iterable)) {
            CustomPyDictObject *mp = (CustomPyDictObject *)d;
            PyObject *oldvalue;
            Py_ssize_t pos = 0;
            PyObject *key;
            Py_hash_t hash;

            if (customdictresize(mp, estimate_log2_keysize(PyDict_GET_SIZE(iterable)), custom_Py_dict_lookup, custom_find_empty_slot, custom_build_indices)) {
                Py_DECREF(d);
                return NULL;
            }

            while (_PyDict_Next(iterable, &pos, &key, &oldvalue, &hash)) {
                if (/*insertdict(mp, key, hash, value)*/ 1) {
                    Py_DECREF(d);
                    return NULL;
                }
            }
            return d;
        }
        if (PyAnySet_CheckExact(iterable)) {
            CustomPyDictObject *mp = (CustomPyDictObject *)d;
            Py_ssize_t pos = 0;
            PyObject *key;
            Py_hash_t hash;

            if (customdictresize(mp, estimate_log2_keysize(PySet_GET_SIZE(iterable)), custom_Py_dict_lookup, custom_find_empty_slot, custom_build_indices)) {
                Py_DECREF(d);
                return NULL;
            }

            while (_PySet_NextEntry(iterable, &pos, &key, &hash)) {
                if (/*insertdict(mp, key, hash, value)*/ 1) {
                    Py_DECREF(d);
                    return NULL;
                }
            }
            return d;
        }
    }

    it = PyObject_GetIter(iterable);
    if (it == NULL){
        Py_DECREF(d);
        return NULL;
    }

    if (PyDict_CheckExact(d)) {
        while ((key = PyIter_Next(it)) != NULL) {
            // status = custom_PyDict_SetItem(d, key, value);
            Py_DECREF(key);
            if (status < 0)
                goto Fail;
        }
    } else {
        while ((key = PyIter_Next(it)) != NULL) {
            status = PyObject_SetItem(d, key, value);
            Py_DECREF(key);
            if (status < 0)
                goto Fail;
        }
    }

    if (PyErr_Occurred())
        goto Fail;
    Py_DECREF(it);
    return d;

Fail:
    Py_DECREF(it);
    Py_DECREF(d);
    return NULL;
}

/* Internal version of dict.from_keys().  It is subclass-friendly. */
PyObject *
_PyDict_FromKeys(PyObject *cls, PyObject *iterable, PyObject *value)
{
    PyObject *it;       /* iter(iterable) */
    PyObject *key;
    PyObject *d;
    int status = 0;

    d = _PyObject_CallNoArg(cls);
    if (d == NULL)
        return NULL;

    if (PyDict_CheckExact(d) && ((PyDictObject *)d)->ma_used == 0) {
        if (PyDict_CheckExact(iterable)) {
            CustomPyDictObject *mp = (CustomPyDictObject *)d;
            PyObject *oldvalue;
            Py_ssize_t pos = 0;
            PyObject *key;
            Py_hash_t hash;

            if (dictresize(mp, estimate_log2_keysize(PyDict_GET_SIZE(iterable)))) {
                Py_DECREF(d);
                return NULL;
            }

            while (_PyDict_Next(iterable, &pos, &key, &oldvalue, &hash)) {
                if (/*insertdict(mp, key, hash, value)*/ 1) {
                    Py_DECREF(d);
                    return NULL;
                }
            }
            return d;
        }
        if (PyAnySet_CheckExact(iterable)) {
            CustomPyDictObject *mp = (CustomPyDictObject *)d;
            Py_ssize_t pos = 0;
            PyObject *key;
            Py_hash_t hash;

            if (dictresize(mp, estimate_log2_keysize(PySet_GET_SIZE(iterable)))) {
                Py_DECREF(d);
                return NULL;
            }

            while (_PySet_NextEntry(iterable, &pos, &key, &hash)) {
                if (/*insertdict(mp, key, hash, value)*/ 1) {
                    Py_DECREF(d);
                    return NULL;
                }
            }
            return d;
        }
    }

    it = PyObject_GetIter(iterable);
    if (it == NULL){
        Py_DECREF(d);
        return NULL;
    }

    if (PyDict_CheckExact(d)) {
        while ((key = PyIter_Next(it)) != NULL) {
            // status = custom_PyDict_SetItem(d, key, value);
            Py_DECREF(key);
            if (status < 0)
                goto Fail;
        }
    } else {
        while ((key = PyIter_Next(it)) != NULL) {
            status = PyObject_SetItem(d, key, value);
            Py_DECREF(key);
            if (status < 0)
                goto Fail;
        }
    }

    if (PyErr_Occurred())
        goto Fail;
    Py_DECREF(it);
    return d;

Fail:
    Py_DECREF(it);
    Py_DECREF(d);
    return NULL;
}

/*[clinic input]
@classmethod
dict.fromkeys
    iterable: object
    value: object=None
    /

Create a new dictionary with keys from iterable and values set to value.
[clinic start generated code]*/

static PyObject *
dict_fromkeys_impl(PyTypeObject *type, PyObject *iterable, PyObject *value)
/*[clinic end generated code: output=8fb98e4b10384999 input=382ba4855d0f74c3]*/
{
    return _PyDict_FromKeys((PyObject *)type, iterable, value);
}

static PyObject *
dict_clear(PyDictObject *mp, PyObject *Py_UNUSED(ignored))
{
    PyDict_Clear((PyObject *)mp);
    Py_RETURN_NONE;
}

static PyObject *
dict_copy(PyDictObject *mp, PyObject *Py_UNUSED(ignored))
{
    return PyDict_Copy((PyObject*)mp);
}

/*[clinic input]
dict.__reversed__

Return a reverse iterator over the dict keys.
[clinic start generated code]*/

static PyObject *
dict___reversed___impl(PyDictObject *self)
/*[clinic end generated code: output=e674483336d1ed51 input=23210ef3477d8c4d]*/
{
    assert (PyDict_Check(self));
    return dictiter_new(self, &PyDictRevIterKey_Type);
}

static PyObject *
dict___reversed__(PyDictObject *self, PyObject *Py_UNUSED(ignored))
{
    return dict___reversed___impl(self);
}

static PyObject *
dict_fromkeys(PyTypeObject *type, PyObject *const *args, Py_ssize_t nargs)
{
    PyObject *return_value = NULL;
    PyObject *iterable;
    PyObject *value = Py_None;

    if (!_PyArg_CheckPositional("fromkeys", nargs, 1, 2)) {
        goto exit;
    }
    iterable = args[0];
    if (nargs < 2) {
        goto skip_optional;
    }
    value = args[1];
skip_optional:
    return_value = dict_fromkeys_impl(type, iterable, value);

exit:
    return return_value;
}

int
ix_to_i(int ix0, PyDictKeysObject *keys)
{
    int i = 0;
    while (i < DK_SIZE(keys)) {
        Py_ssize_t ix = dictkeys_get_index(keys, i);
        if (ix == ix0)
            return i;
        i++;
    }

    return i;
}

PyObject *
dict_print(PyObject *mp, PyObject *Py_UNUSED(ignored))
{
    dict_traverse2((CustomPyDictObject *) mp, 1);
    return mp;
}

PyObject *
dict_num_items(PyObject *mp, PyObject *Py_UNUSED(ignored))
{
    int num_items = dict_traverse2((CustomPyDictObject *) mp, 0);
    printf("num_items: %d.\n", num_items);
    fflush(stdout);

    return mp;
}

static PyMethodDef custom_mapp_methods[] = {
    DICT___CONTAINS___METHODDEF
    {"__getitem__", (PyCFunction)(void(*)(void))dict_subscript,        METH_O | METH_COEXIST,
     getitem__doc__},
    {"__sizeof__",      (PyCFunction)(void(*)(void))dict_sizeof,       METH_NOARGS,
     sizeof__doc__},
    CUSTOM_DICT_GET_METHODDEF
    DICT_SETDEFAULT_METHODDEF
    DICT_POP_METHODDEF
    DICT_POPITEM_METHODDEF
    {"keys",            dictkeys_new,                   METH_NOARGS,
    keys__doc__},
    {"items",           dictitems_new,                  METH_NOARGS,
    items__doc__},
    {"values",          dictvalues_new,                 METH_NOARGS,
    values__doc__},
    {"update",          (PyCFunction)(void(*)(void))custom_dict_update, METH_VARARGS | METH_KEYWORDS,
     update__doc__},
    DICT_FROMKEYS_METHODDEF
    {"clear",           (PyCFunction)dict_clear,        METH_NOARGS,
     clear__doc__},
    {"copy",            (PyCFunction)dict_copy,         METH_NOARGS,
     copy__doc__},
    DICT___REVERSED___METHODDEF
    {"__class_getitem__", (PyCFunction)Py_GenericAlias, METH_O|METH_CLASS, PyDoc_STR("See PEP 585")},
    {"print",           dict_print,                     METH_NOARGS,
    keys__doc__},
    {"num_items",       dict_num_items,                 METH_NOARGS,
    keys__doc__},
    {NULL,              NULL}   /* sentinel */
};

static inline void
dictkeys_incref(PyDictKeysObject *dk)
{
#ifdef Py_REF_DEBUG
    _Py_RefTotal++;
#endif
    dk->dk_refcnt++;
}

static PyObject *
custom_dict_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    assert(type != NULL);
    assert(type->tp_alloc != NULL);
    // dict subclasses must implement the GC protocol
    assert(_PyType_IS_GC(type));

    PyObject *self = type->tp_alloc(type, 0);
    if (self == NULL) {
        return NULL;
    }
    CustomPyDictObject *d = (CustomPyDictObject *)self;

    d->ma_used = 0;
    d->ma_version_tag = DICT_NEXT_VERSION();
    dictkeys_incref(Py_EMPTY_KEYS);
    d->ma_keys = Py_EMPTY_KEYS;
    d->ma_values = empty_values;
    ASSERT_CONSISTENT(d);

    if (type != &PyDict_Type) {
        // Don't track if a subclass tp_alloc is PyType_GenericAlloc()
        if (!_PyObject_GC_IS_TRACKED(d)) {
            _PyObject_GC_TRACK(d);
        }
    }
    else {
        // _PyType_AllocNoTrack() does not track the created object
        assert(!_PyObject_GC_IS_TRACKED(d));
    }
    return self;
}

static void
dict_dealloc(PyDictObject *mp)
{
    PyObject **values = mp->ma_values;
    PyDictKeysObject *keys = mp->ma_keys;
    Py_ssize_t i, n;

    /* bpo-31095: UnTrack is needed before calling any callbacks */
    PyObject_GC_UnTrack(mp);
    Py_TRASHCAN_BEGIN(mp, dict_dealloc)
    if (values != NULL) {
        if (values != empty_values) {
            for (i = 0, n = mp->ma_keys->dk_nentries; i < n; i++) {
                Py_XDECREF(values[i]);
            }
            free_values(values);
        }
        dictkeys_decref(keys);
    }
    else if (keys != NULL) {
        assert(keys->dk_refcnt == 1);
        dictkeys_decref(keys);
    }
    struct _Py_dict_state *state = get_dict_state();
#ifdef Py_DEBUG
    // new_dict() must not be called after _PyDict_Fini()
    assert(state->numfree != -1);
#endif
    if (state->numfree < PyDict_MAXFREELIST && Py_IS_TYPE(mp, &PyDict_Type)) {
        state->free_list[state->numfree++] = mp;
    }
    else {
        Py_TYPE(mp)->tp_free((PyObject *)mp);
    }
    Py_TRASHCAN_END
}

static int
dict_traverse(PyObject *op, visitproc visit, void *arg)
{
    PyDictObject *mp = (PyDictObject *)op;
    PyDictKeysObject *keys = mp->ma_keys;
    PyDictKeyEntry *entries = DK_ENTRIES(keys);
    Py_ssize_t i, n = keys->dk_nentries;

    if (keys->dk_kind == DICT_KEYS_GENERAL) {
        for (i = 0; i < n; i++) {
            if (entries[i].me_value != NULL) {
                Py_VISIT(entries[i].me_value);
                Py_VISIT(entries[i].me_key);
            }
        }
    }
    else {
        if (mp->ma_values != NULL) {
            for (i = 0; i < n; i++) {
                Py_VISIT(mp->ma_values[i]);
            }
        }
        else {
            for (i = 0; i < n; i++) {
                Py_VISIT(entries[i].me_value);
            }
        }
    }
    return 0;
}

PyTypeObject MyPyDict_Type2 = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "dict",
    .tp_doc = dictionary_doc,
    .tp_basicsize = sizeof(CustomPyDictObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
        Py_TPFLAGS_BASETYPE | Py_TPFLAGS_DICT_SUBCLASS |
        _Py_TPFLAGS_MATCH_SELF | Py_TPFLAGS_MAPPING,
    .tp_new = custom_dict_new,
    .tp_init = custom_dict_init,
    .tp_dealloc = (destructor)dict_dealloc,
    .tp_methods = custom_mapp_methods,
    .tp_traverse = dict_traverse
};

PyMODINIT_FUNC
PyInit_custom2(void)
{
    PyObject *m;
    if (PyType_Ready(&MyPyDict_Type2) < 0)
        return NULL;

    m = PyModule_Create(&custommodule2);
    if (m == NULL)
        return NULL;

    Py_INCREF(&MyPyDict_Type2);
    if (PyModule_AddObject(m, "Custom", (PyObject *) &MyPyDict_Type2) < 0) {
        Py_DECREF(&MyPyDict_Type2);
        Py_DECREF(m);
        return NULL;
    }

    return m;
}