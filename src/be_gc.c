#include "be_gc.h"
#include "be_vm.h"
#include "be_mem.h"
#include "be_var.h"
#include "be_vector.h"
#include "be_string.h"
#include "be_class.h"
#include "be_list.h"
#include "be_map.h"
#include "be_debug.h"

struct bgc {
    bgcobject *list;
    bgcobject *gray;
    size_t mcount;
    bbyte steprate;
    bbyte pause;
};

void auto_collect(bvm *vm);

void be_gc_init(bvm *vm)
{
    bgc *gc = be_malloc(sizeof(bgc));
    gc->list = NULL;
    gc->gray = NULL;
    gc->mcount = be_mcount();
    gc->steprate = 200;
    gc->pause = 0;
    vm->gc = gc;
}

void be_gc_setsteprate(bvm *vm, int rate)
{
    vm->gc->steprate = (bbyte)rate;
}

void be_gc_setpause(bvm *vm, int pause)
{
    vm->gc->pause = (bbyte)pause;
}

bgcobject* be_newgcobj(bvm *vm, int type, int size)
{
    bgc *gc = vm->gc;
    bgcobject *obj = be_malloc(size);

    set_type(obj, type);
    gc_setdark(obj);
    be_gc_auto(vm);
    obj->next = gc->list; /* insert to head */
    gc->list = obj;
    return obj;
}

void be_gc_addgray(bvm *vm, bgcobject *obj)
{
    bgc *gc = vm->gc;
    if (gc->list == obj) { /* first node */
        gc->list = obj->next;
    } else {
        bgcobject *prev = gc->list;
        /* find node */
        while (prev && prev->next != obj) {
            prev = prev->next;
        }
        if (!prev) {
            return;
        }
        prev->next = obj->next;
    }
    obj->next = gc->gray;
    gc->gray = obj;
    gc_setgray(obj);
}

void be_gc_removegray(bvm *vm, bgcobject *obj)
{
    bgc *gc = vm->gc;
    if (gc->gray == obj) {
        gc->gray = obj->next;
    } else {
        bgcobject *prev = gc->gray;
        /* find node */
        while (prev && prev->next != obj) {
            prev = prev->next;
        }
        if (!prev) {
            return;
        }
        prev->next = obj->next;
    }
    obj->next = gc->list;
    gc->list = obj;
    gc_setwhite(obj);
}

static void mark_object(bvm *vm, bgcobject *obj, int type);

static void mark_instance(bvm *vm, bgcobject *obj)
{
    bobject *o = cast_object(obj);
    while (o) {
        bvalue *var = be_object_members(o);
        int nvar = be_object_member_count(o);
        while (nvar--) {
            mark_object(vm, var->v.p, type(var));
            var++;
        }
        gc_setdark(o);
        o = be_object_super(o);
    }
}

static void mark_map(bvm *vm, bgcobject *obj)
{
    bmap *map = cast_map(obj);
    if (map) {
        bmapentry *slot = be_map_slots(map);
        int nslots = be_map_size(map);
        for (; nslots--; slot++) {
            bvalue *key = &slot->key;
            if (!value_isnil(key)) {
                bvalue *val = &slot->value;
                mark_object(vm, key->v.p, type(key));
                mark_object(vm, val->v.p, type(val));
            }
        }
        gc_setdark(obj);
    }
}

static void mark_list(bvm *vm, bgcobject *obj)
{
    blist *list = cast_list(obj);
    if (list) {
        bvalue *val = be_list_data(list);
        int count = be_list_count(list);
        for (; count--; val++) {
            mark_object(vm, val->v.p, type(val));
        }
        gc_setdark(obj);
    }
}

static void mark_closure(bvm *vm, bgcobject *obj)
{
    bclosure *cl = cast_closure(obj);
    if (cl) {
        int count = cl->nupvals;
        bupval **uv = cl->upvals;
        for (; count--; ++uv) {
            if ((*uv)->refcnt) {
                bvalue *v = (*uv)->value;
                mark_object(vm, v->v.p, type(v));
            }
        }
        gc_setdark(obj);
    }
}

static void mark_proto(bvm *vm, bgcobject *obj)
{
    bproto *p = cast_proto(obj);
    if (p) {
        int count = p->nconst;
        bvalue *k = p->ktab;
        mark_object(vm, gc_object(p->name), type(p->name));
        for (; count--; ++k) {
            mark_object(vm, k->v.p, type(k));
        }
    }
    gc_setdark(obj);
}

static void mark_object(bvm *vm, bgcobject *obj, int type)
{
    switch (type) {
    case VT_STRING: case VT_CLASS: case VT_PROTO:
    case VT_INSTANCE: case VT_MAP: case VT_LIST:
    case VT_CLOSURE:
        if (gc_isdark(obj)) {
            return;
        }
        break;
    default: return;
    }
    switch (type) {
    case VT_STRING: gc_setdark(obj); break;
    case VT_CLASS: gc_setdark(obj); break;
    case VT_PROTO: mark_proto(vm, obj); break;
    case VT_INSTANCE: mark_instance(vm, obj); break;
    case VT_MAP: mark_map(vm, obj); break;
    case VT_LIST: mark_list(vm, obj); break;
    case VT_CLOSURE: mark_closure(vm, obj); break;
    default: break;
    }
}

void free_map(bgcobject *obj)
{
    bmap *map = cast_map(obj);
    if (map) {
        be_free(be_map_slots(map));
    }
    be_free(obj);
}

void free_list(bgcobject *obj)
{
    blist *list = cast_list(obj);
    if (list) {
        be_free(be_list_data(list));
    }
    be_free(obj);
}

void free_closure(bgcobject *obj)
{
    bclosure *cl = cast_closure(obj);
    if (cl) {
        int i, count = cl->nupvals;
        for (i = 0; i < count; ++i) {
            bupval *uv = cl->upvals[i];
            if (uv->refcnt) {
                --uv->refcnt;
            }
            /* delete non-referenced closed upvalue */
            if (uv->value == &uv->u.value && !uv->refcnt) {
                be_free(uv);
            }
        }
    }
    be_free(obj);
}

static void free_object(bvm *vm, bgcobject *obj)
{
    switch (obj->type) {
    case VT_STRING: be_deletestrgc(vm, cast_str(obj)); break;
    case VT_INSTANCE: be_free(obj); break;
    case VT_MAP: free_map(obj); break;
    case VT_LIST: free_list(obj); break;
    case VT_CLOSURE: free_closure(obj); break;
    default:
        break;
    }
}

static void set_gray(bvalue *begin, int count)
{
    bvalue *end;
    for (end = begin + count; begin < end; ++begin) {
        switch (type(begin)) {
        case VT_CLOSURE: case VT_PROTO:
        case VT_CLASS: case VT_INSTANCE:
        case VT_STRING: case VT_LIST: case VT_MAP:
            gc_setgray(gc_object(begin->v.p));
        default: break;
        }
    }
}

static void mark_unscanned(bvm *vm)
{
    bgc *gc = vm->gc;
    bgcobject *node;
    for (node = gc->list; node; node = node->next) {
        if (gc_isgray(node)) {
            mark_object(vm, node, type(node));
        }
    }
}

static void mark_gray(bvm *vm)
{
    bgc *gc = vm->gc;
    bgcobject *node;
    for (node = gc->gray; node; node = node->next) {
        if (gc_isgray(node)) {
            mark_object(vm, node, type(node));
        }
    }
}

static void delete_white(bvm *vm)
{
    bgc *gc = vm->gc;
    bgcobject *node, *prev, *next;
    for (node = gc->list, prev = node; node; node = next) {
        next = node->next;
        if (gc_iswhite(node)) {
            if (node == gc->list) { /* first node */
                gc->list = node->next;
                prev = node->next;
            } else { /* not first node */
                prev->next = next;
            }
            free_object(vm, node);
        } else {
            gc_setwhite(node);
            prev = node;
        }
    }
}

static void clear_graylist(bvm *vm)
{
    bgc *gc = vm->gc;
    bgcobject *node;
    for (node = gc->gray; node; node = node->next) {
        if (gc_isdark(node)) {
            gc_setgray(node);
        }
    }
}

static int nstack(bvm *vm)
{
    bcallframe *cf = vm->cf;
    if (be_stack_isempty(vm->callstack)) {
        return 0;
    }
    if (cf->status & PRIM_FUNC) {
        return cf->u.top - vm->stack;
    }
    return cf->reg - vm->stack + cf->u.s.closure->proto->nlocal;
}

void be_gc_auto(bvm *vm)
{
    bgc *gc = vm->gc;
    size_t mcount = be_mcount();
    if (gc->pause && mcount > gc->mcount * gc->steprate / 100) {
        be_gc_collect(vm);
    }
}

void be_gc_collect(bvm *vm)
{
    /* step 1: set root-set reference object to unscanned */
    set_gray(vm->global, vm->gbldesc.nglobal); /* global objects */
    set_gray(vm->stack, nstack(vm)); /* stack objects */
    /* step 2: set unscanned object to black */
    mark_gray(vm);
    mark_unscanned(vm);
    /* step 3: delete unreachable object */
    delete_white(vm);
    clear_graylist(vm);
}