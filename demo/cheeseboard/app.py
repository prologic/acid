
import sys
import time
import urllib

import acid
import acid.meta
import bottle
import wheezy.template.engine
import wheezy.template.ext.core
import wheezy.template.loader

import models

templates = wheezy.template.engine.Engine(
    loader=wheezy.template.loader.FileLoader(['templates']),
    extensions=[wheezy.template.ext.core.CoreExtension()])

store = models.init_store()


def getint(name, default=None):
    try:
        return int(bottle.request.params.get(name))
    except (ValueError, TypeError):
        return default


@bottle.route('/')
def index():
    t0 = time.time()
    hi = getint('hi')
    with store.begin():
        posts = list(models.Post.iter(hi=hi, reverse=True, max=5))
        highest_id = models.Post.find(reverse=True)
        print list(models.Post.iter())
    t1 = time.time()
    print posts
    older = None
    newer = None
    if posts:
        oldest = posts[-1][0][0] - 1
        if oldest > 0:
            older = '?hi=' + str(oldest)
        if posts[0][0] < highest_id:
            newer = '?hi=' + str(posts[0][0][0] + 5)

    return templates.get_template('index.html').render({
        'error': bottle.request.query.get('error'),
        'posts': posts,
        'older': older,
        'newer': newer,
        'msec': int((t1 - t0) * 1000)
    })


@bottle.route('/static/<filename>')
def static(filename):
    return bottle.static_file(filename, root='static')


@bottle.post('/newpost')
def newpost():
    post = models.Post(name=bottle.request.POST.get('name'),
                       text=bottle.request.POST.get('text'))
    try:
        with store.begin(write=True):
            post.save()
    except acid.errors.ConstraintError, e:
        return bottle.redirect('.?error=' + urllib.quote(str(e)))
    return bottle.redirect('.')


if 'debug' in sys.argv:
    bottle.run(host='0.0.0.0', port=8000, debug=True)
else:
    import bjoern
    bjoern.run(bottle.default_app(), '0.0.0.0', 8000)
