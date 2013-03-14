
import cStringIO
import operator
import os
import pdb
import shutil
import unittest

from pprint import pprint
from unittest import TestCase

import centidb
import centidb.centidb
import centidb.support
import _centidb


#
# Module reloads are necessary because KEY_ENCODER & co bind whatever
# encode_keys() & so happens to exist before we get a chance to interfere. It
# also improves the chance of noticing any not-planned-for speedups related
# side effects, rather than relying on explicit test coverage.
# 
# There are nicer approaches to this (e.g. make_key_encoder()), but these would
# optimize for the uncommon case of running tests.
#

class PythonMixin:
    """Reload modules with speedups disabled."""
    @classmethod
    def setUpClass(cls):
        global centidb
        os.environ['NO_SPEEDUPS'] = '1'
        centidb.centidb = reload(centidb.centidb)
        centidb = reload(centidb)
        getattr(cls, '_setUpClass', lambda: None)()

class NativeMixin:
    """Reload modules with speedups enabled."""
    @classmethod
    def setUpClass(cls):
        global centidb
        os.environ.pop('NO_SPEEDUPS', None)
        centidb.centidb = reload(centidb.centidb)
        centidb = reload(centidb)
        getattr(cls, '_setUpClass', lambda: None)()

def register(python=True, native=True):
    def fn(klass):
        if python:
            name = 'Python' + klass.__name__
            globals()[name] = type(name, (klass, PythonMixin, TestCase), {})
        if native:
            name = 'Native' + klass.__name__
            globals()[name] = type(name, (klass, NativeMixin, TestCase), {})
        return klass
    return fn


def ddb():
    pprint(list(db))

def copy(it, dst):
    for tup in it:
        dst.put(*tup)


def make_asserter(op, ops):
    def ass(x, y, msg='', *a):
        if msg:
            if a:
                msg %= a
            msg = ' (%s)' % msg

        f = '%r %s %r%s'
        assert op(x, y), f % (x, ops, y, msg)
    return ass

lt = make_asserter(operator.lt, '<')
eq = make_asserter(operator.eq, '==')
le = make_asserter(operator.le, '<=')


@register()
class IterTest:
    KEYS = ('aa', 'cc', 'd', 'dd', 'de')
    ITEMS = [(k, '') for k in KEYS]
    REVERSE = ITEMS[::-1]

    def setUp(self):
        self.e = centidb.support.ListEngine()
        for key in self.KEYS:
            self.e.put(key, '')

    def iter(self, *args, **kwargs):
        return list(centidb.centidb._iter(self.e, self.e, *args, **kwargs))

    def testForward(self):
        eq(self.ITEMS, self.iter())

    def testForwardSeekFirst(self):
        eq(self.ITEMS, self.iter('aa'))

    def testForwardSeekNoExist(self):
        eq(self.ITEMS[1:], self.iter('b'))

    def testForwardSeekExist(self):
        eq(self.ITEMS[1:], self.iter('cc'))

    def testForwardSkipMostExist(self):
        eq([('de', '')], self.iter('de'))

    def testForwardSeekBeyondNoExist(self):
        eq([], self.iter('df'))

    def riter(self, *args, **kwargs):
        return self.iter(reverse=True, *args, **kwargs)

    def testReverse(self):
        eq(self.REVERSE, self.riter())

    def testReverseSeekLast(self):
        eq(self.REVERSE, self.riter('de'))

    def testReverseSeekNoExist(self):
        eq(self.REVERSE[1:], self.riter('ddd'))

    def testReverseSeekExist(self):
        eq(self.REVERSE[1:], self.riter('dd'))

    def testReverseSkipMostExist(self):
        eq([('aa', '')], self.riter('ab'))

    def testReverseSeekBeyondFirst(self):
        eq([], self.riter('a'))


@register(native=False)
class SkiplistTest:
    def testFindLess(self):
        sl = centidb.support.SkipList()
        update = sl._update[:]
        assert sl._findLess(update, 'missing') is sl.head

        sl.insert('dave', 'dave')
        assert sl._findLess(update, 'dave') is sl.head

        sl.insert('dave2', 'dave')
        assert sl._findLess(update, 'dave2')[0] == 'dave'

        assert sl._findLess(update, 'dave3')[0] == 'dave2'
        #print sl.reprNode(sl._findLess(update, 'dave3')[3])


class EngineTestBase:
    def testGetPutOverwrite(self):
        assert self.e.get('dave') is None
        self.e.put('dave', '')
        self.assertEqual(self.e.get('dave'), '')
        self.e.put('dave', '2')
        self.assertEqual(self.e.get('dave'), '2')

    def testDelete(self):
        self.e.delete('dave')
        assert self.e.get('dave') is None
        self.e.put('dave', '')
        self.assertEqual(self.e.get('dave'), '')
        self.e.delete('dave')
        self.assertEqual(self.e.get('dave'), None)

    def testIterForwardEmpty(self):
        assert list(self.e.iter('')) == []
        assert list(self.e.iter('x')) == []

    def testIterForwardFilled(self):
        self.e.put('dave', '')
        eq(list(self.e.iter('dave')), [('dave', '')])
        eq(list(self.e.iter('davee')), [])

    def testIterReverseEmpty(self):
        eq(list(self.e.iter('', reverse=True)), [])
        eq(list(self.e.iter('x', reverse=True)), [])

    def testIterReverseAtEnd(self):
        self.e.put('a', '')
        self.e.put('b', '')
        eq(list(self.e.iter('b', reverse=True)), [('b', ''), ('a', '')])

    def testIterReversePastEnd(self):
        self.e.put('a', '')
        self.e.put('b', '')
        eq(list(self.e.iter('c', reverse=True)), [('b', ''), ('a', '')])

    def testIterReverseFilled(self):
        self.e.put('dave', '')
        eq(list(self.e.iter('davee', reverse=True)), [('dave', '')])

    def testIterForwardMiddle(self):
        self.e.put('a', '')
        self.e.put('c', '')
        self.e.put('d', '')
        assert list(self.e.iter('b')) == [('c', ''), ('d', '')]
        assert list(self.e.iter('c')) == [('c', ''), ('d', '')]

    def testIterReverseMiddle(self):
        self.e.put('a', '')
        self.e.put('b', '')
        self.e.put('d', '')
        self.e.put('e', '')
        eq(list(self.e.iter('c', reverse=True)),
           [('b', ''), ('a', '')])


@register()
class ListEngineTest(EngineTestBase):
    def setUp(self):
        self.e = centidb.support.ListEngine()


@register()
class PlyvelEngineTest(EngineTestBase):
    @classmethod
    def _setUpClass(cls):
        if os.path.exists('test.ldb'):
            shutil.rmtree('test.ldb')
        cls.e = centidb.support.PlyvelEngine(
            name='test.ldb', create_if_missing=True)

    def setUp(self):
        for key, value in self.e.iter(''):
            self.e.delete(key)

    @classmethod
    def tearDownClass(cls):
        cls.e = None
        shutil.rmtree('test.ldb')



@register()
class KeysTest:
    SINGLE_VALS = [
        None,
        1,
        'x',
        u'hehe',
        True,
        False,
        -1
    ]

    def _enc(self, *args, **kwargs):
        return centidb.encode_keys(*args, **kwargs)

    def _dec(self, *args, **kwargs):
        return centidb.decode_keys(*args, **kwargs)

    def test_counter(self):
        s = self._enc(('dave', 1))
        eq([('dave', 1)], self._dec(s))

    def test_single(self):
        for val in self.SINGLE_VALS:
            encoded = centidb.encode_keys((val,))
            decoded = centidb.decode_keys(encoded)
            eq([(val,)], decoded, 'input was %r' % (val,))

    def test_single_sort_lower(self):
        for val in self.SINGLE_VALS:
            e1 = centidb.encode_keys((val,))
            e2 = centidb.encode_keys([(val, val),])
            lt(e1, e2, 'eek %r' % (val,))


@register()
class StringEncodingTest:
    def do_test(self, k):
        eq(k, centidb.centidb.decode_key(centidb.encode_keys(k)))

    def test_simple(self):
        self.do_test(('dave',))

    def test_escapes(self):
        self.do_test(('dave\x00\x00',))
        self.do_test(('dave\x00\x01',))
        self.do_test(('dave\x01\x01',))
        self.do_test(('dave\x01\x02',))
        self.do_test(('dave\x01',))


@register()
class TuplizeTest:
    def test_already_tuple(self):
        eq((), centidb.centidb.tuplize(()))

    def test_not_already_tuple(self):
        eq(("",), centidb.centidb.tuplize(""))


class EncodeIntTestBase:
    INTS = [0, 1, 240, 241, 2286, 2287, 2288,
            67823, 67824, 16777215, 16777216,
            4294967295, 4294967296,
            1099511627775, 1099511627776,
            281474976710655, 281474976710656,
            72057594037927935, 72057594037927936]

    def testInts(self):
        for i in self.INTS:
            io = cStringIO.StringIO(self.encode_int(i))
            j = self.decode_int(lambda: io.read(1), io.read)
            assert j == i, (i, j, io.getvalue())

class PythonEncodeIntTestCase(EncodeIntTestBase, TestCase):
    encode_int = staticmethod(centidb.encode_int)
    decode_int = staticmethod(centidb.decode_int)

class NativeEncodeIntTestCase(EncodeIntTestBase, TestCase):
    encode_int = staticmethod(_centidb.encode_int)
    decode_int = staticmethod(centidb.decode_int)



@register()
class TupleTest:
    def assertOrder(self, tups):
        tups = [centidb.centidb.tuplize(x) for x in tups]
        encs = map(centidb.encode_keys, tups)
        encs.sort()
        eq(tups, [centidb.centidb.decode_key(x) for x in encs])

    def testStringSorting(self):
        strs = [(x,) for x in ('dave', 'dave\x00', 'dave\x01', 'davee\x01')]
        encs = map(centidb.encode_keys, strs)
        encs.sort()
        eq(strs, [centidb.centidb.decode_key(x) for x in encs])

    def testTupleNonTuple(self):
        pass


@register()
class CollBasicTest:
    def setUp(self):
        self.e = centidb.support.ListEngine()
        self.store = centidb.Store(self.e)
        self.coll = centidb.Collection(self.store, 'coll1')

    def _record(self, *args):
        return centidb.Record(self.coll, *args)

    def testGetNoExist(self):
        eq(None, self.coll.get('missing'))

    def testGetNoExistRec(self):
        eq(None, self.coll.get('missing', rec=True))

    def testGetNoExistDefault(self):
        eq('dave', self.coll.get('missing', default='dave'))

    def testGetNoExistDefaultRec(self):
        eq(self._record("dave"),
           self.coll.get('missing', default='dave', rec=True))

    def testGetExist(self):
        rec = self.coll.put('')
        eq(rec.key, (1,))
        eq('', self.coll.get(1))
        rec = self.coll.put('x')
        eq(rec.key, (2,))
        eq('x', self.coll.get(2))

    def testIterItemsExist(self):
        rec = self.coll.put('')
        eq([((1,), '')], list(self.coll.iteritems()))

    def testIterKeysExist(self):
        rec = self.coll.put('')
        rec2 = self.coll.put('')
        eq([rec.key, rec2.key], list(self.coll.iterkeys()))

    def testIterValuesExist(self):
        rec = self.coll.put('')
        eq([''], list(self.coll.itervalues()))


class Bag(object):
    def __init__(self, **kwargs):
        vars(self).update(kwargs)


@register(python=False)
class IndexKeyBuilderTest:
    def _keys(self, func):
        idx = Bag(prefix='\x10', func=func)
        ikb = _centidb.IndexKeyBuilder([idx])
        return ikb.build((1,), {})

    def testSingleValue(self):
        eq(['\x10\x15\x01\x66\x15\x01'], self._keys(lambda obj: 1))

    def testListSingleValue(self):
        eq(self._keys(lambda obj: ['foo']), ['\x10(foof\x15\x01'])

    def testListTuple(self):
        eq(self._keys(lambda obj: ['foo', 'bar']),
                      ['\x10(foof\x15\x01', '\x10(barf\x15\x01'])


@register()
class RecordTest:
    def test_basic(self):
        self.assertRaises(TypeError, centidb.Record)
        centidb.Record('ok', 'ok')


def x():
    db = plyvel.DB('test.ldb', create_if_missing=True)
    store = storelib.Store(db)

    feeds = storelib.Collection(store, 'feeds',
        key_func=lambda _, feed: feed.url,
        encoder=ThriftEncoder(iotypes.Feed))
    feeds.add_index('id', lambda _, feed: [feed.id] if feed.id else [])

    feed = iotypes.Feed(url='http://dave', title='mytitle', id=69)
    feeds.put(feed)


if __name__ == '__main__':
    unittest.main()
