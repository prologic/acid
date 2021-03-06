/*
 * Copyright 2013, David Wilson.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy
 * of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#define _BSD_SOURCE // timegm()
#define _POSIX_C_SOURCE 200809L

#include "acid.h"
#include "datetime.h"

#include <arpa/inet.h>
#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <string.h>
#include <structmember.h>
#include <sys/types.h>
#include <time.h>

// Reference to key.c::KeyType.
static PyTypeObject *KeyType;
// Reference to uuid.UUID().
static PyTypeObject *UUID_Type;
// Reference to uuid.UUID.get_bytes().
static PyObject *uuid_get_bytes;
// Reference to datetime.datetime.utcoffset().
static PyObject *datetime_utcoffset;


/**
 * Read a byte into `*ch` from `rdr`, returning 1 on success or 0 if no bytes
 * remain.
 */
static int reader_getc(struct reader *rdr, uint8_t *ch)
{
    int ret = 0;
    if(rdr->p < rdr->e) {
        *ch = *(rdr->p++);
        ret = 1;
    }
    return ret;
}

/**
 * Return 1 if `rdr` has at least `n` bytes left, otherwise set an exception
 * and return 0.
 */
static int reader_ensure(struct reader *rdr, Py_ssize_t n)
{
    int ret = (rdr->e - rdr->p) >= n;
    if(! ret) {
        PyErr_Format(PyExc_ValueError,
            "expected %lld bytes but only %lld remain.",
            (long long) n, (long long) (rdr->e - rdr->p));
    }
    return ret;
}

/**
 * Unconditionally return a byte from `rdr` and advance its read position.
 */
static uint8_t reader_getchar(struct reader *rdr)
{
    return *(rdr->p++);
}

/**
 * Initialize a struct writer, used to ease direct construction of a PyString.
 */
int writer_init(struct writer *wtr, Py_ssize_t initial)
{
    wtr->pos = 0;
    wtr->s = PyString_FromStringAndSize(NULL, initial);
    return wtr->s != NULL;
}

/**
 * Ensure `wtr` contains at least `size` unused bytes. Return 1 on success or
 * set an exception and return 0.
 */
static int writer_need(struct writer *wtr, Py_ssize_t size)
{
    assert(wtr->s != 0);
    Py_ssize_t cursize = PyString_GET_SIZE(wtr->s);
    Py_ssize_t remain = cursize - wtr->pos;
    if(remain < size) {
        cursize += 5 + (size - remain);
        if(-1 == _PyString_Resize(&(wtr->s), cursize)) {
            PyErr_NoMemory();
            return 0;
        }
    }
    return 1;
}

/**
 * Return a pointer to the current write position. The pointer is valid as long
 * as writer_need() is never indirectly called.
 */
uint8_t *writer_ptr(struct writer *wtr)
{
    return (uint8_t *) &(PyString_AS_STRING(wtr->s)[wtr->pos]);
}

/**
 * Unconditionally write a byte to `wtr` and increment its write position.
 */
static void writer_putchar(struct writer *wtr, uint8_t ch)
{
    PyString_AS_STRING(wtr->s)[wtr->pos++] = ch;
}

/*
 * Append a byte `o` to `wtr`, growing it as necessary. Return 1 on success or
 * set an exception and return 0 on error.
 */
static int writer_putc(struct writer *wtr, uint8_t o)
{
    int ret = writer_need(wtr, 1);
    if(ret) {
        writer_putchar(wtr, o);
    }
    return ret;
}

/**
 * Append a bytestring `s` to the buffer, growing it as necessary. Return 1 on
 * success or set an exception and return 0.
 */
static int writer_puts(struct writer *wtr, const char *s, Py_ssize_t size)
{
    int ret = writer_need(wtr, size);
    if(ret) {
        memcpy(PyString_AS_STRING(wtr->s) + wtr->pos, s, size);
        wtr->pos += size;
    }
    return ret;
}

/**
 * Resize the string to its final size. `wtr` should be discarded after calling
 * finalize(). Return a new reference to the string on success or sets an
 * exception and returns NULL on failure.
 */
static PyObject *writer_fini(struct writer *wtr)
{
    PyObject *o = wtr->s;
    if(o) {
        wtr->s = NULL;
        _PyString_Resize(&o, wtr->pos);
    }
    return o;
}

/**
 * Discard the partially built string and clear the writer.
 */
void writer_abort(struct writer *wtr)
{
    Py_CLEAR(wtr->s);
    wtr->pos = 0;
}

/**
 * Encode the unsigned 64-bit integer `v` into `wtr`, optionally prefixing the
 * output with `kind` if nonzero, and XORing all output bytes with `xor` (for
 * negative integers).
 */
static int write_int(struct writer *wtr, uint64_t v, enum ElementKind kind,
                     uint8_t xor)
{
    if(kind && !writer_putc(wtr, kind)) {
        return 0;
    }

    int ok = 1;
    if(v <= 240ULL) {
        ok = writer_putc(wtr, xor ^ v);
    } else if(v <= 2287ULL) {
        if((ok = writer_need(wtr, 2))) {
            v -= 240ULL;
            writer_putchar(wtr, xor ^ (241 + (uint8_t) (v >> 8)));
            writer_putchar(wtr, xor ^ ((uint8_t) (v & 0xff)));
        }
    } else if(v <= 67823) {
        if((ok = writer_need(wtr, 3))) {
            v -= 2288ULL;
            writer_putchar(wtr, xor ^ 0xf9);
            writer_putchar(wtr, xor ^ ((uint8_t) (v >> 8)));
            writer_putchar(wtr, xor ^ ((uint8_t) (v & 0xff)));
        }
    } else if((ok = writer_need(wtr, 9))) {
        // Progressively increment type byte from 24bit case.
        uint8_t *typeptr = writer_ptr(wtr);
        uint8_t type = 0xfa;
        writer_putchar(wtr, 0);

        if(v > 0xffffffffffffffULL) {
            writer_putchar(wtr, xor ^ ((uint8_t) (v >> 56)));
            type++;
        }
        if(v > 0xffffffffffffULL) {
            writer_putchar(wtr, xor ^ ((uint8_t) (v >> 48)));
            type++;
        }
        if(v > 0xffffffffffULL) {
            writer_putchar(wtr, xor ^ ((uint8_t) (v >> 40)));
            type++;
        }
        if(v > 0xffffffffULL) {
            writer_putchar(wtr, xor ^ ((uint8_t) (v >> 32)));
            type++;
        }
        if(v > 0xffffffULL) {
            writer_putchar(wtr, xor ^ ((uint8_t) (v >> 24)));
            type++;
        }
        writer_putchar(wtr, xor ^ ((uint8_t) (v >> 16)));
        writer_putchar(wtr, xor ^ ((uint8_t) (v >> 8)));
        writer_putchar(wtr, xor ^ ((uint8_t) (v)));
        *typeptr = xor ^ type;
    }
    return ok;
}

/**
 * Python-level interface to write_int(). Return the integer encoded to a
 * string on success, or sets an exception and return NULL and set an exception
 * on failure.
 */
static PyObject *py_pack_int(PyObject *self, PyObject *args)
{
    char *prefix;
    Py_ssize_t prefix_len;
    uint64_t v;

    if(! PyArg_ParseTuple(args, "s#K", (char **) &prefix, &prefix_len, &v)) {
        return NULL;
    }

    struct writer wtr;
    if(writer_init(&wtr, 9)) {
        if(writer_puts(&wtr, prefix, prefix_len)) {
            if(write_int(&wtr, v, 0, 0)) {
                return writer_fini(&wtr);
            }
        }
    }
    return NULL;
}

/**
 * Write `p[0..length]` to `wtr`, optionally prefixed by `kind` if it is
 * nonzero. Return 1 on success, or set an exception and return 0 on failure.
 */
static int write_str(struct writer *wtr, uint8_t *restrict p, Py_ssize_t length,
                     enum ElementKind kind)
{
    Py_ssize_t need = length + (kind > 0);
    need = (Py_ssize_t) ceil(need * 1.1428571428571428);
    if(! writer_need(wtr, need)) {
        return 0;
    }

    if(kind) {
        writer_putchar(wtr, kind);
    }

    int shift = 1;
    uint8_t trailer = 0;

    while(length--) {
        uint8_t o = *(p++);
        writer_putchar(wtr, 0x80 | trailer | (o >> shift));
        if(shift < 7) {
            trailer = (o << (7 - shift));
            shift++;
        } else {
            writer_putchar(wtr, 0x80 | o);
            shift = 1;
            trailer = 0;
        }
    }

    if(shift > 1) {
        writer_putchar(wtr, 0x80 | trailer);
    }
    return 1;
}

/**
 * Given a datetime.datetime instance `dt`, try to figure out its UTC offset in
 * seconds. If the datetime is timezone-naive, then assume it is in the
 * system's timezone. Return 0 on success, or set an exception and return -1 on
 * failure.
 */
static int get_utcoffset_secs(PyObject *dt, int64_t ts)
{
    PyObject *td = PyObject_CallFunctionObjArgs(datetime_utcoffset, dt, NULL);
    if(! td) {
        return -1;
    } else if(td == Py_None) {
        Py_DECREF(td);
        time_t now = (time_t) ts;
        struct tm tm;
        localtime_r(&now, &tm);
        time_t local = mktime(&tm);
        time_t utc = timegm(&tm);
        return (int) (utc - local);
    } else if(! PyDelta_CheckExact(td)) {
        return -1;
    } else {
        PyDateTime_Delta *delta = (void *)td;
        int offset = delta->days * (60 * 60 * 24);
        offset += delta->seconds;
        Py_DECREF(td);
        return offset;
    }
}

/**
 * Encode the datetime.datetime instance `dt` into `wtr`. Return 0 on success
 * or set an exception and return -1 on failure.
 */
static int write_time(struct writer *wtr, PyObject *dt)
{
    struct tm tm = {
        .tm_sec = PyDateTime_DATE_GET_SECOND(dt),
        .tm_min = PyDateTime_DATE_GET_MINUTE(dt),
        .tm_hour = PyDateTime_DATE_GET_HOUR(dt),
        .tm_mday = PyDateTime_GET_DAY(dt),
        .tm_mon = PyDateTime_GET_MONTH(dt) - 1,
        .tm_year = PyDateTime_GET_YEAR(dt) - 1900
    };

    int64_t ts = (int64_t) timegm(&tm);

    int offset_secs = get_utcoffset_secs(dt, ts);
    if(offset_secs == -1) {
        return -1;
    }

    ts -= offset_secs;
    int offset_bits = UTCOFFSET_SHIFT + (offset_secs / UTCOFFSET_DIV);
    assert(offset_bits <= 0x7f && offset_bits >= 0);
    ts *= 1000;
    ts += PyDateTime_DATE_GET_MICROSECOND(dt) / 1000;
    ts <<= 7;
    ts |= offset_bits;

    if(ts < 0) {
        return write_int(wtr, (uint64_t) -ts, KIND_NEG_TIME, 0xff);
    } else {
        return write_int(wtr, (uint64_t) ts, KIND_TIME, 0);
    }
}

/**
 * Given some arbitrary Python object `arg`, figure out what it is, and encode
 * it to `wtr`. Return 1 on success, or set an exception and return 0 on
 * failure.
 */
int write_element(struct writer *wtr, PyObject *arg)
{
    int ret = 0;
    PyTypeObject *type = Py_TYPE(arg);

    if(arg == Py_None) {
        ret = writer_putc(wtr, KIND_NULL);
    } else if(type == &PyInt_Type) {
        long v = PyInt_AS_LONG(arg);
        if(v < 0) {
            ret = write_int(wtr, -v, KIND_NEG_INTEGER, 0xff);
        } else {
            ret = write_int(wtr, v, KIND_INTEGER, 0);
        }
    } else if(type == &PyString_Type) {
        ret = write_str(wtr, (uint8_t *)PyString_AS_STRING(arg),
                             PyString_GET_SIZE(arg), KIND_BLOB);
    } else if(type == &PyUnicode_Type) {
        PyObject *utf8 = PyUnicode_EncodeUTF8(PyUnicode_AS_UNICODE(arg),
            PyUnicode_GET_SIZE(arg), "strict");
        if(utf8) {
            ret = write_str(wtr, (uint8_t *)PyString_AS_STRING(utf8),
                                 PyString_GET_SIZE(utf8), KIND_TEXT);
            Py_DECREF(utf8);
        }
    } else if(type == &PyBool_Type) {
        ret = writer_putc(wtr, KIND_BOOL);
        ret = writer_putc(wtr, (uint8_t) (arg == Py_True));
    } else if(type == &PyLong_Type) {
        int64_t i64 = PyLong_AsLongLong(arg);
        if(! PyErr_Occurred()) {
            if(i64 < 0) {
                ret = write_int(wtr, -i64, KIND_NEG_INTEGER, 0xff);
            } else {
                ret = write_int(wtr, i64, KIND_INTEGER, 0);
            }
        }
    } else if(PyDateTime_CheckExact(arg)) {
        ret = write_time(wtr, arg);
    } else if(type == UUID_Type) {
        PyObject *ss = PyObject_CallFunctionObjArgs(uuid_get_bytes, arg, NULL);
        if(! ss) {
            return -1;
        }
        assert(Py_TYPE(ss) == &PyString_Type);
        ret = writer_putc(wtr, KIND_UUID);
        if(ret) {
            ret = writer_puts(wtr, PyString_AS_STRING(ss),
                                   PyString_GET_SIZE(ss));
        }
        Py_DECREF(ss);
    } else {
        const char *name = arg->ob_type->tp_name;
        PyErr_Format(PyExc_TypeError, "got unsupported type %.200s", name);
    }

    return ret;
}

/**
 * Encode all the elements from the Python tuple `tup` into `wtr`, returning 1
 * on success or set an exception and return 0 on failure.
 */
static int write_tuple(struct writer *wtr, PyObject *tup)
{
    int ret = 1;
    for(Py_ssize_t i = 0; ret && i < PyTuple_GET_SIZE(tup); i++) {
        ret = write_element(wtr, PyTuple_GET_ITEM(tup, i));
    }
    return ret;
}

/**
 * Python-level packs() implementation. Accepts 2 parameters, string prefix and
 * list/tuple/element to encode.
 */
static PyObject *py_packs(PyObject *self, PyObject *args)
{
    uint8_t *prefix = NULL;
    Py_ssize_t prefix_size;

    Py_ssize_t arg_count = PyTuple_GET_SIZE(args);
    if(arg_count != 2) {
        PyErr_SetString(PyExc_TypeError,
            "packs() takes exactly 2 arguments.");
        return NULL;
    }

    PyObject *py_prefix = PyTuple_GET_ITEM(args, 0);
    if(Py_TYPE(py_prefix) != &PyString_Type) {
        PyErr_SetString(PyExc_TypeError, "packs() prefix must be str.");
        return NULL;
    }
    prefix = (uint8_t *) PyString_AS_STRING(py_prefix);
    prefix_size = PyString_GET_SIZE(py_prefix);

    struct writer wtr;
    if(! writer_init(&wtr, 20)) {
        return NULL;
    }

    if(prefix) {
        if(! writer_puts(&wtr, (char *)prefix, prefix_size)) {
            return NULL;
        }
    }

    PyObject *tups = PyTuple_GET_ITEM(args, 1);
    PyTypeObject *type = Py_TYPE(tups);

    int ret = 1;
    if(type != &PyList_Type) {
        if(type == &PyTuple_Type) {
            ret = write_tuple(&wtr, tups);
        } else if(type == KeyType) {
            ret = writer_puts(&wtr, (void *) ((Key *)tups)->p, Py_SIZE(tups));
        } else {
            ret = write_element(&wtr, tups);
        }
    } else {
        for(int i = 0; ret && i < PyList_GET_SIZE(tups); i++) {
            if(i) {
                ret = writer_putc(&wtr, KIND_SEP);
            }
            PyObject *elem = PyList_GET_ITEM(tups, i);
            type = Py_TYPE(elem);
            if(type == &PyTuple_Type) {
                ret = write_tuple(&wtr, elem);
            } else if(type == KeyType) {
                ret = writer_puts(&wtr, (void *) ((Key *)elem)->p,
                                  Py_SIZE(elem));
            } else {
                ret = write_element(&wtr, elem);
            }
        }
    }

    PyObject *packed = writer_fini(&wtr);
    if(! ret) {
        Py_CLEAR(packed);
    }
    return packed;
}

/**
 * Decode the varint pointed to by `rdr` into `u64`, XORing read bytes with
 * `xor`. Return 1 on success or set an exception and return 0 on failure.
 */
static int read_plain_int(struct reader *rdr, uint64_t *u64, uint8_t xor)
{
    uint8_t ch = 0;
    if(! reader_getc(rdr, &ch)) {
        return 0;
    }

    uint64_t v = 0;
    int ok = 1;

    ch ^= xor;
    if(ch <= 240) {
        v = ch;
    } else if(ch <= 248) {
        if((ok = reader_ensure(rdr, 1))) {
            v  = 240;
            v += 256 * (ch - 241);
            v += xor ^ reader_getchar(rdr);
        }
    } else if(ch == 249) {
        if((ok = reader_ensure(rdr, 2))) {
            v  = 2288;
            v += 256 * (xor ^ reader_getchar(rdr));
            v += xor ^ reader_getchar(rdr);
        }
    } else if((ok = reader_ensure(rdr, 8 - (255-ch)))) {
        v = 0;
        if(ch >= 255) {
            v |= ((uint64_t) (xor ^ reader_getchar(rdr))) << 56;
        }
        if(ch >= 254) {
            v |= ((uint64_t) (xor ^ reader_getchar(rdr))) << 48;
        }
        if(ch >= 253) {
            v |= ((uint64_t) (xor ^ reader_getchar(rdr))) << 40;
        }
        if(ch >= 252) {
            v |= ((uint64_t) (xor ^ reader_getchar(rdr))) << 32;
        }
        if(ch >= 251) {
            v |= ((uint64_t) (xor ^ reader_getchar(rdr))) << 24;
        }
        v |= ((uint64_t) (xor ^ reader_getchar(rdr))) << 16;
        v |= ((uint64_t) (xor ^ reader_getchar(rdr))) << 8;
        v |= ((uint64_t) (xor ^ reader_getchar(rdr)));
    }
    *u64 = v;
    return ok;
}

/**
 * Decode the integer pointed to by `rdr` and return it. If `negate` is 0, the
 * integer is presumed to be positive, otherwise it will be negated before
 * being returned. `xor` is the mask to apply to read bytes. For negative
 * integers it should be 0xff, otherise 0. Return a new reference to the
 * integer on success, or set an exception and return NULL on failure.
 */
static PyObject *read_int(struct reader *rdr, int negate, uint8_t xor)
{
    uint64_t u64;
    if(! read_plain_int(rdr, &u64, xor)) {
        return NULL;
    }
    PyObject *v = PyLong_FromUnsignedLongLong(u64);
    if(v && negate) {
        PyObject *v2 = PyNumber_Negative(v);
        Py_DECREF(v);
        v = v2;
    }
    return v;
}

/**
 * Decode the string pointed to by `rdr` and return it, or NULL on error and
 * set an exception.
 */
static PyObject *read_str(struct reader *rdr)
{
    struct writer wtr;

    if(! writer_init(&wtr, 20)) {
        return NULL;
    }
    // 0-byte string at end of key.
    if(! (rdr->p - rdr->e)) {
        return writer_fini(&wtr);
    }

    uint8_t lb = 0;
    uint8_t cb;

    int shift = 1;

    int ret = reader_getc(rdr, &lb);
    if(! ret) {
        return 0;
    }
    if(lb < 0x80) {
        rdr->p--;
        return writer_fini(&wtr);
    }

    while(ret && reader_getc(rdr, &cb)) {
        if(cb < 0x80) {
            rdr->p--;
            break;
        }
        uint8_t ch = lb << shift;
        ch |= (cb & 0x7f) >> (7 - shift);
        ret = writer_putc(&wtr, ch);
        if(shift < 7) {
            shift++;
            lb = cb;
        } else {
            shift = 1;
            ret = reader_getc(rdr, &lb);
            if(ret && lb < 0x80) {
                rdr->p--;
                break;
            }
        }
    }
    return writer_fini(&wtr);
}

/**
 * Decode a datetime pointed to by `rdr`. Return a new reference to the
 * datetime.datetime instance on success, or set an exception and return NULL
 * on failure.
 */
static PyObject *read_time(struct reader *rdr, enum ElementKind kind)
{
    uint64_t v;
    uint8_t xor = (kind == KIND_NEG_TIME) ? 0xff : 0;
    if(! read_plain_int(rdr, &v, xor)) {
        return NULL;
    }

    int offset_secs = (((int) v & 0x7f) - UTCOFFSET_SHIFT) * UTCOFFSET_DIV;
    PyObject *fixed_offset = get_fixed_offset(offset_secs);
    if(! fixed_offset) {
        return NULL;
    }

    v >>= 7;
    double secs = (double) (v / 1000) + (((double) (v % 1000)) / 1000.0);
    if(kind == KIND_NEG_TIME) {
        secs = -secs;
    }

    PyObject *py_secs = PyFloat_FromDouble(secs);
    if(! py_secs) {
        Py_DECREF(fixed_offset);
        return NULL;
    }

    PyObject *args = PyTuple_New(2);
    PyTuple_SET_ITEM(args, 0, py_secs);
    PyTuple_SET_ITEM(args, 1, fixed_offset);
    PyObject *dt = PyDateTime_FromTimestamp(args);
    Py_DECREF(args);
    return dt;
}

/**
 * Decode a UUID pointed to by `rdr`. Return a new reference to the UUID
 * instance on success, or set an exception and return NULL on failure.
 */
static PyObject *read_uuid(struct reader *rdr)
{
    if(! reader_ensure(rdr, 16)) {
        return NULL;
    }
    PyObject *s = PyString_FromStringAndSize((const char *)rdr->p, 16);
    if(! s) {
        return NULL;
    }

    PyObject *arg = PyObject_CallFunctionObjArgs(
        (PyObject *)UUID_Type, Py_None, s, NULL);
    Py_DECREF(s);
    rdr->p += 16;
    return arg;
}

/**
 * Decode the next tuple element pointed to by `rdr`, returning NULL and
 * setting an exception on failure.
 */
PyObject *
read_element(struct reader *rdr)
{
    PyObject *tmp;
    PyObject *arg = NULL;
    uint64_t u64;

    uint8_t ch = reader_getchar(rdr);
    switch(ch) {
    case KIND_NULL:
        arg = Py_None;
        Py_INCREF(arg);
        break;

    case KIND_INTEGER:
        arg = read_int(rdr, 0, 0);
        break;
    case KIND_NEG_INTEGER:
        arg = read_int(rdr, 1, 0xff);
        break;
    case KIND_BOOL:
        if(read_plain_int(rdr, &u64, 0)) {
            arg = u64 ? Py_True : Py_False;
            Py_INCREF(arg);
        }
        break;
    case KIND_BLOB:
        arg = read_str(rdr);
        break;
    case KIND_TEXT:
        tmp = read_str(rdr);
        if(tmp) {
            arg = PyUnicode_DecodeUTF8(PyString_AS_STRING(tmp),
                                       PyString_GET_SIZE(tmp), "strict");
            Py_DECREF(tmp);
        }
        break;
    case KIND_NEG_TIME:
    case KIND_TIME:
        arg = read_time(rdr, (enum ElementKind) ch);
        break;
    case KIND_UUID:
        arg = read_uuid(rdr);
        break;
    default:
        PyErr_Format(PyExc_ValueError, "bad kind %d; key corrupt?", ch);
        break;
    }
    return arg;
}

/**
 * Construct and return a tuple of encoded elements pointed to by `rdr`, until
 * KIND_SEP or the empty string is reached.
 */
static PyObject *unpack(struct reader *rdr)
{
    PyObject *tup = PyTuple_New(TUPLE_START_SIZE);
    if(! tup) {
        return NULL;
    }

    Py_ssize_t tpos = 0;
    while(rdr->p < rdr->e) {
        if(*rdr->p == KIND_SEP) {
            rdr->p++;
            break;
        }
        PyObject *arg = read_element(rdr);
        if(! arg) {
            Py_DECREF(tup);
            return NULL;
        }
        if(tpos == PyTuple_GET_SIZE(tup)) {
            if(-1 == _PyTuple_Resize(&tup, PyTuple_GET_SIZE(tup) + 2)) {
                Py_DECREF(arg);
                return NULL;
            }
        }
        PyTuple_SET_ITEM(tup, tpos++, arg);
    }
    PyTuple_GET_SIZE(tup) = tpos;
    return tup;
}

/**
 * Given a reader initialized to point at the start of a tuple, seek to the
 * idx'th element in the tuple. Return 0 on success or -1 and set an exception
 * on error.
 */
int
skip_element(struct reader *rdr, int *eof)
{
    uint8_t xor = 0;
    int ret = 0;

    uint8_t ch = *rdr->p++;
    switch(ch) {
    case KIND_BOOL:
    case KIND_NULL:
        break;
    case KIND_NEG_TIME:
    case KIND_NEG_INTEGER:
        xor = 0xff;
    case KIND_TIME:
    case KIND_INTEGER:
        ch = xor ^ *rdr->p++;
        if(ch <= 248 && ch > 240) {
            rdr->p++;
        } else if(ch >= 249) {
            rdr->p += 8 - (255-ch);
        }
        break;
    case KIND_TEXT:
    case KIND_BLOB:
        for(; (rdr->p < rdr->e) && (0x80 & *rdr->p); rdr->p++);
        break;
    case KIND_UUID:
        rdr->p += 16;
        break;
    case KIND_SEP:
        *eof = 1;
        break;
    default:
        PyErr_Format(PyExc_ValueError, "bad kind %d; key corrupt?", ch);
        ret = -1;
    }
    if(rdr->p == rdr->e && !*eof) {
        *eof = 1;
    }
    return ret;
}

/**
 * Python-level interface to unpack a tuple. Expects 2 arguments: string prefix
 * to ignore and encoded tuple. Return the unpacked tuple on success, or set an
 * exception and return NULL on failure.
 */
static PyObject *py_unpack(PyObject *self, PyObject *args)
{
    uint8_t *prefix;
    uint8_t *s;
    Py_ssize_t s_len;
    Py_ssize_t prefix_len;

    if(! PyArg_ParseTuple(args, "s#s#", (char **) &prefix, &prefix_len,
                                        (char **) &s, &s_len)) {
        return NULL;
    }

    if(s_len < prefix_len || memcmp(prefix, s, prefix_len)) {
        Py_RETURN_NONE;
    }

    struct reader rdr = {s+prefix_len, s+s_len};
    return unpack(&rdr);
}

/**
 * Python-level interface to unpack a list of tuples. Expects 2 arguments:
 * string prefix to ignore and encoded tuple. Return the unpacked list on
 * success, or set an exception and return NULL on failure.
 */
static PyObject *py_unpacks(PyObject *self, PyObject *args)
{
    uint8_t *prefix;
    uint8_t *s;
    Py_ssize_t prefix_len;
    Py_ssize_t s_len;

    if(! PyArg_ParseTuple(args, "s#s#", (char **) &prefix, &prefix_len,
                                        (char **) &s, &s_len)) {
        return NULL;
    }

    if(s_len < prefix_len || memcmp(prefix, s, prefix_len)) {
        Py_RETURN_NONE;
    }

    struct reader rdr = {s+prefix_len, s+s_len};
    PyObject *tups = PyList_New(LIST_START_SIZE);
    if(! tups) {
        return NULL;
    }

    Py_ssize_t lpos = 0;
    while(rdr.p < rdr.e) {
        PyObject *tup = unpack(&rdr);
        if(! tup) {
            Py_DECREF(tups);
            return NULL;
        }

        if(lpos < LIST_START_SIZE) {
            PyList_SET_ITEM(tups, lpos++, tup);
        } else {
            if(-1 == PyList_Append(tups, tup)) {
                Py_DECREF(tups);
                Py_DECREF(tup);
                return NULL;
            }
            Py_DECREF(tup);
            lpos++;
        }
    }
    PyTuple_GET_SIZE(tups) = lpos;
    return tups;
}

/**
 * Python-level function to decode an array of varints prefixed with a varint
 * indicating the array's length. Used to encode the size of each individual
 * value as part of a batch key.
 */
static PyObject *py_decode_offsets(PyObject *self, PyObject *args)
{
    uint8_t *s;
    Py_ssize_t s_len;

    if(! PyArg_ParseTuple(args, "s#", (char **) &s, &s_len)) {
        return NULL;
    }

    struct reader rdr = {s, s+s_len};

    uint64_t count;
    if(! read_plain_int(&rdr, &count, 0)) {
        return NULL;
    }

    uint64_t pos = 0;
    PyObject *out = PyList_New(1 + (int) count);
    PyObject *tmp = PyInt_FromLong(0);
    if(! (out && tmp)) {
        Py_CLEAR(out);
        Py_CLEAR(tmp);
        return NULL;
    }
    PyList_SET_ITEM(out, 0, tmp);

    for(uint64_t i = 0; i < count; i++) {
        uint64_t offset;
        if(! read_plain_int(&rdr, &offset, 0)) {
            return NULL;
        }
        pos += offset;
        tmp = PyInt_FromLong((long) pos);
        if(! tmp) {
            Py_CLEAR(out);
            return NULL;
        }
        PyList_SET_ITEM(out, 1 + i, tmp);
    }

    PyObject *tmpi = PyInt_FromLong(rdr.p - s);
    tmp = PyTuple_New(2);
    if(! (tmp && tmpi)) {
        Py_CLEAR(tmp);
        Py_CLEAR(tmpi);
        return NULL;
    }
    PyTuple_SET_ITEM(tmp, 0, out);
    PyTuple_SET_ITEM(tmp, 1, tmpi);
    return tmp;
}

/**
 * Import `module`, then iteratively walk its attributes looking for a specific
 * object. Given import_object("sys", "stdout", "write", NULL), would return a
 * new reference to a bound instancemethod for "sys.stdout.write". Return a new
 * reference on success, or set an exception and return NULL on failure.
 */
static PyObject *
import_object(const char *module, ...)
{
    va_list ap;
    va_start(ap, module);

    PyObject *obj = PyImport_ImportModule(module);
    if(! obj) {
        return NULL;
    }

    va_start(ap, module);
    const char *name;
    while((name = va_arg(ap, const char *)) != NULL) {
        PyObject *obj2 = PyObject_GetAttrString(obj, name);
        Py_DECREF(obj);
        if(! obj2) {
            va_end(ap);
            return NULL;
        }
        obj = obj2;
    }
    return obj;
}

/**
 * Table of functions exported in the acid._keylib module.
 */
static PyMethodDef KeylibMethods[] = {
    {"unpack", py_unpack, METH_VARARGS, "unpack"},
    {"unpacks", py_unpacks, METH_VARARGS, "unpacks"},
    {"pack", py_packs, METH_VARARGS, "pack"},
    {"packs", py_packs, METH_VARARGS, "packs"},
    {"pack_int", py_pack_int, METH_VARARGS, "pack_int"},
    {"decode_offsets", py_decode_offsets, METH_VARARGS, "decode_offsets"},
    {NULL, NULL, 0, NULL}
};

/**
 * Do all required to initialize the module.
 */
PyMODINIT_FUNC
init_keylib(void)
{
    PyDateTime_IMPORT;

    UUID_Type = (PyTypeObject *) import_object("uuid", "UUID", NULL);
    assert(PyType_CheckExact((PyObject *) UUID_Type));

    datetime_utcoffset = import_object("datetime",
        "datetime", "utcoffset", NULL);
    uuid_get_bytes = import_object("uuid", "UUID", "get_bytes", NULL);
    assert(datetime_utcoffset && uuid_get_bytes);

    PyObject *mod = Py_InitModule("acid._keylib", KeylibMethods);
    if(! mod) {
        return;
    }

    PyObject *dct = PyModule_GetDict(mod);
    if(! dct) {
        return;
    }

    PyTypeObject *fixed_offset = init_fixed_offset_type();
    if(fixed_offset) {
        PyDict_SetItemString(dct, "FixedOffset", (PyObject *) fixed_offset);
    }

    KeyType = init_key_type();
    if(KeyType) {
        PyDict_SetItemString(dct, "Key", (PyObject *) KeyType);
    }
}
