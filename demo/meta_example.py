#!/usr/bin/env python

#
# NOTE: acid.meta is incomplete. This exmple does not function.
#

import acid
import acid.meta


class MyModel(acid.meta.Model):
    pass


class Item(MyModel):
    email = acid.meta.String()
    password = acid.meta.String()
    first_name = acid.meta.String()
    last_name = acid.meta.String()
    age = acid.meta.Integer()
    id = acid.meta.Integer()
    parent_id = acid.meta.Integer()

    @acid.meta.constraint
    def check_id(self):
        return self.id is not None

    @acid.meta.key
    def key(self):
        key = [self.id]
        parent_id = self.parent_id
        while parent_id:
            key.append(parent_id)
            parent = self.get(id=parent_id)
            assert parent
            parent_id = parent.id
        key.reverse()
        return tuple(key)

    @acid.meta.index
    def first_last(self):
        return self.first_name, self.last_name


store = acid.open('ListEngine')
MyModel.bind_store(store)

i = Item(id=123)
print i
i.save()
print i
