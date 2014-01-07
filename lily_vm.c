#include <string.h>
#include <inttypes.h>

#include "lily_impl.h"
#include "lily_symtab.h"
#include "lily_opcode.h"
#include "lily_vm.h"
#include "lily_debug.h"

#define INTEGER_OP(OP) \
lhs = (lily_sym *)code[i+2]; \
rhs = (lily_sym *)code[i+3]; \
if (lhs->flags & S_IS_NIL) \
    novalue_error(vm, i, lhs); \
else if (rhs->flags & S_IS_NIL) \
    novalue_error(vm, i, rhs); \
((lily_sym *)code[i+4])->value.integer = \
lhs->value.integer OP rhs->value.integer; \
((lily_sym *)code[i+4])->flags &= ~S_IS_NIL; \
i += 5; \

#define INTNUM_OP(OP) \
lhs = (lily_sym *)code[i+2]; \
rhs = (lily_sym *)code[i+3]; \
if (lhs->flags & S_IS_NIL) \
    novalue_error(vm, i, lhs); \
else if (rhs->flags & S_IS_NIL) \
    novalue_error(vm, i, rhs); \
if (lhs->sig->cls->id == SYM_CLASS_NUMBER) { \
    if (rhs->sig->cls->id == SYM_CLASS_NUMBER) \
        ((lily_sym *)code[i+4])->value.number = \
        lhs->value.number OP rhs->value.number; \
    else \
        ((lily_sym *)code[i+4])->value.number = \
        lhs->value.number OP rhs->value.integer; \
} \
else \
    ((lily_sym *)code[i+4])->value.number = \
    lhs->value.integer OP ((lily_sym *)code[i+3])->value.number; \
((lily_sym *)code[i+4])->flags &= ~S_IS_NIL; \
i += 5;

#define COMPARE_OP(OP, STROP) \
lhs = (lily_sym *)code[i+2]; \
rhs = (lily_sym *)code[i+3]; \
if (lhs->flags & S_IS_NIL) \
    novalue_error(vm, i, lhs); \
else if (rhs->flags & S_IS_NIL) \
    novalue_error(vm, i, rhs); \
if (lhs->sig->cls->id == SYM_CLASS_NUMBER) { \
    if (rhs->sig->cls->id == SYM_CLASS_NUMBER) \
        ((lily_sym *)code[i+4])->value.integer = \
        lhs->value.number OP rhs->value.number; \
    else \
        ((lily_sym *)code[i+4])->value.integer = \
        lhs->value.number OP rhs->value.integer; \
} \
else if (lhs->sig->cls->id == SYM_CLASS_INTEGER) { \
    if (rhs->sig->cls->id == SYM_CLASS_INTEGER) \
        ((lily_sym *)code[i+4])->value.integer =  \
        (lhs->value.integer OP rhs->value.integer); \
    else \
        ((lily_sym *)code[i+4])->value.integer = \
        lhs->value.integer OP rhs->value.number; \
} \
else if (lhs->sig->cls->id == SYM_CLASS_STR) { \
    ((lily_sym *)code[i+4])->value.integer = \
    strcmp(lhs->value.str->str, \
           rhs->value.str->str) STROP; \
} \
((lily_sym *)code[i+4])->flags &= ~S_IS_NIL; \
i += 5;

/* This intentionally takes the input sym as 'to' and the flags for 'from'. It
   does that so accidentally reversing the arguments will trigger a compile
   error instead of working. This also helps to make what's being set a little
   more obvious, since there is only one sym given. */
#define COPY_NIL_FLAG(to, from) \
to->flags = (to->flags & ~S_IS_NIL) ^ (from & S_IS_NIL);

/** vm init and deletion **/
lily_vm_state *lily_new_vm_state(lily_raiser *raiser)
{
    lily_vm_state *vm = lily_malloc(sizeof(lily_vm_state));
    if (vm == NULL)
        return NULL;

    vm->saved_values = lily_malloc(sizeof(lily_saved_val) * 8);
    vm->method_stack = lily_malloc(sizeof(lily_vm_stack_entry *) * 4);
    vm->err_function = NULL;
    vm->in_function = 0;
    vm->method_stack_pos = 0;
    vm->raiser = raiser;
    vm->val_pos = 0;
    vm->val_size = 8;

    if (vm->method_stack) {
        int i;
        for (i = 0;i < 4;i++) {
            vm->method_stack[i] = lily_malloc(sizeof(lily_vm_stack_entry));
            if (vm->method_stack[i] == NULL)
                break;
        }
        vm->method_stack_size = i;
    }
    else
        vm->method_stack_size = 0;

    if (vm->saved_values == NULL || vm->method_stack == NULL ||
        vm->method_stack_size != 4) {

        lily_free_vm_state(vm);
        return NULL;
    }

    return vm;
}

void lily_free_vm_state(lily_vm_state *vm)
{
    int i;
    for (i = 0;i < vm->method_stack_size;i++)
        lily_free(vm->method_stack[i]);

    lily_free(vm->method_stack);
    lily_free(vm->saved_values);
    lily_free(vm);
}

/** VM helpers **/

/* circle_buster
   This function takes the current object (lhs_obj), and determines if assigning
   rhs_value to it would cause a circular reference. This function recurses into
   rhs_value, and fixes any circular references. This should be done before
   putting rhs_value into lhs_obj.
   Notes/Rules:
   * This will only tag objects that refer to lists as circular, but there are
     times when lists should be tagged as circular. Two lists containing each
     other is one such example.
     Right now, subscript assign checks for circular lists with
     nested_list_check.
   * This returns the number of references that have been fixed. Since the
     symtab will not call these derefs, the caller must take 'num_circles' refs
     away from lhs_obj.
   * Don't touch things that are set S_IS_NIL, because they may be invalid.
     Test that first, then for being visited already.
   * Set ->visited to 1 before entering and 0 after leaving. This prevents this
     function from infinitely recursing. */
static int circle_buster(lily_object_val *lhs_obj, lily_sig *rhs_sig,
        lily_value rhs_value)
{
    int i, inner_count, num_circles;

    num_circles = 0;

    if (rhs_sig->cls->id == SYM_CLASS_LIST) {
        lily_list_val *value_list = rhs_value.list;
        int inner_cls_id = rhs_sig->node.value_sig->cls->id;

        if (inner_cls_id == SYM_CLASS_OBJECT) {
            lily_object_val *inner_obj;
            for (i = 0;i < value_list->num_values;i++) {
                if (value_list->flags[i] & S_IS_NIL)
                    continue;

                inner_obj = value_list->values[i].object;

                if (inner_obj == lhs_obj) {
                    /* Mark this as circular if it isn't already marked as such.
                       num_circles is always updated, which is necessary because
                       there may be a list of circular refs
                       (ex: a[0] = [a, a, a]) */
                    if ((value_list->flags[i] & S_IS_CIRCULAR) == 0)
                        value_list->flags[i] |= S_IS_CIRCULAR;

                    num_circles++;
                }
                else if (inner_obj->sig->cls->id == SYM_CLASS_LIST &&
                         inner_obj->value.list->visited == 0) {
                    /* If the object contains a list, then dive into that list
                       to check for circular references. */
                    if (value_list->flags[i] & S_IS_CIRCULAR)
                        continue;

                    inner_obj->value.list->visited = 1;
                    inner_count = circle_buster(lhs_obj, inner_obj->sig,
                            inner_obj->value);
                    inner_obj->value.list->visited = 0;
                    num_circles += inner_count;
                }
            }
        }
        else if (inner_cls_id == SYM_CLASS_LIST) {
            lily_value inner_list_val;
            for (i = 0;i < value_list->num_values;i++) {
                if (value_list->flags[i] & S_IS_NIL)
                    continue;

                inner_list_val = value_list->values[i];
                if (inner_list_val.list->visited)
                    continue;

                inner_list_val.list->visited = 1;
                inner_count = circle_buster(lhs_obj, rhs_sig->node.value_sig,
                        inner_list_val);
                inner_list_val.list->visited = 0;
                num_circles += inner_count;
            }
        }
    }

    return num_circles;
}

/* Returns 1 if the first list given contains the second list. This is used by
   op_sub_assign to help prevent a circular reference issue.
   The caller is expected to verify that both lists contain objects at some
   depth, since lists cannot be circular otherwise. */
static int is_list_in_list(lily_sig *lhs_sig, lily_list_val *lhs_list,
                           lily_list_val *rhs_list)
{
    int i;
    int found = 0;
    int num_values = lhs_list->num_values;
    int *flags = lhs_list->flags;
    lily_sig *elem_sig = lhs_sig->node.value_sig;

    /* Each element has the same signature, so hoist the loops to keep from
       checking it repeatedly. This might be more verbose, but will definitely
       be faster for large lists (which is more important). */
    if (elem_sig->cls->id == SYM_CLASS_LIST) {
        for (i = 0;i < num_values;i++) {
            if ((flags[i] & (S_IS_NIL | S_IS_CIRCULAR)) == 0) {
                lily_list_val *lv = lhs_list->values[i].list;
                if (lv == rhs_list ||
                    is_list_in_list(elem_sig, lv, rhs_list)) {
                    found = 1;
                    break;
                }
            }
        }
    }
    else if (elem_sig->cls->id == SYM_CLASS_OBJECT) {
        for (i = 0;i < num_values;i++) {
            /* Don't check for S_IS_NIL, because objects should never be nil. */
            if ((flags[i] & S_IS_CIRCULAR) == 0) {
                lily_object_val *ov = lhs_list->values[i].object;
                /* The object's sig is NULL if it does not contain anything. */
                if (ov->sig != NULL && ov->sig->cls->id == SYM_CLASS_LIST) {
                    if (ov->value.list == rhs_list) {
                        found = 1;
                        break;
                    }
                }
            }
        }
    }
    /* else it's a basic type, and cannot contain the list. */

    return found;
}

/* nested_list_check
   Both symbols contain a list, and right's list is to be put inside of left's
   list. Before checking that the lists could be nested, find out if they both
   contain objects at some point. If they do not, then they cannot be nested
   in each other. */
int nested_list_check(lily_sym *left, lily_sym *right)
{
    int is_nested = 0;
    lily_sig *node_sig;

    node_sig = left->sig->node.value_sig;
    while (node_sig->cls->id == SYM_CLASS_LIST)
        node_sig = node_sig->node.value_sig;

    if (node_sig->cls->id == SYM_CLASS_OBJECT) {
        node_sig = right->sig->node.value_sig;
        while (node_sig->cls->id == SYM_CLASS_LIST)
            node_sig = node_sig->node.value_sig;

            if (is_list_in_list(right->sig, right->value.list,
                                left->value.list))
                is_nested = 1;
    }

    return is_nested;
}

/* grow_method_stack
   This function grows the vm's method stack so it can take more method info.
   Calls lily_raise_nomem if unable to create method info. */
static void grow_method_stack(lily_vm_state *vm)
{
    int i;
    lily_vm_stack_entry **new_stack;

    /* Methods are free'd from 0 to stack_size, so don't increase stack_size
       just yet. */
    new_stack = lily_realloc(vm->method_stack,
            sizeof(lily_vm_stack_entry *) * 2 * vm->method_stack_size);

    if (new_stack == NULL)
        lily_raise_nomem(vm->raiser);

    vm->method_stack = new_stack;
    vm->method_stack_size *= 2;

    for (i = vm->method_stack_pos+1;i < vm->method_stack_size;i++) {
        vm->method_stack[i] = lily_malloc(sizeof(lily_vm_stack_entry));
        if (vm->method_stack[i] == NULL) {
            vm->method_stack_size = i;
            lily_raise_nomem(vm->raiser);
        }
    }
}

/* grow_method_stack
   This function grows the vm's saved values so it can save more storages. Calls
   lily_raise_nomem if unable to create method info. */
static void grow_saved_vals(lily_vm_state *vm, int upto)
{
    do {
        vm->val_size *= 2;
    } while ((vm->val_pos + upto) > vm->val_size);

    lily_saved_val *new_values = lily_realloc(vm->saved_values,
            sizeof(lily_saved_val) * vm->val_size);

    if (new_values == NULL)
        lily_raise_nomem(vm->raiser);

    vm->saved_values = new_values;
}

/* maybe_crossover_assign
   This handles assignment between two symbols which don't have the exact same
   type. This assumes the caller has verified that rhs is not nil.
   Returns 1 if the assignment happened, 0 otherwise. */
int maybe_crossover_assign(lily_sym *lhs, lily_sym *rhs)
{
    int ret = 1;

    if (rhs->sig->cls->id != SYM_CLASS_OBJECT) {
        if (lhs->sig->cls->id == SYM_CLASS_INTEGER &&
            rhs->sig->cls->id == SYM_CLASS_NUMBER)
            lhs->value.integer = (int64_t)(rhs->value.number);
        else if (lhs->sig->cls->id == SYM_CLASS_NUMBER &&
                 rhs->sig->cls->id == SYM_CLASS_INTEGER)
            lhs->value.number = (double)(rhs->value.integer);
        else
            ret = 0;
    }
    else {
        lily_value obj_val = rhs->value.object->value;
        int obj_class_id = rhs->value.object->sig->cls->id;

        if (lhs->sig->cls->id == SYM_CLASS_INTEGER &&
            obj_class_id == SYM_CLASS_NUMBER)
            lhs->value.integer = (int64_t)(obj_val.number);
        else if (lhs->sig->cls->id == SYM_CLASS_NUMBER &&
                 obj_class_id == SYM_CLASS_INTEGER)
            lhs->value.number = (double)(obj_val.integer);
        else {
            ret = 0;
        }
    }

    if (ret)
        lhs->flags &= ~S_IS_NIL;

    return ret;
}

/* novalue_error
   This is a helper routine that raises ErrNoValue because the given sym is
   nil but should not be. code_pos is the current code position, because the
   current method's info is not saved in the stack (because it would almost
   always be stale). */
static void novalue_error(lily_vm_state *vm, int code_pos, lily_sym *sym)
{
    /* ...So fill in the current method's info before dying. */
    lily_vm_stack_entry *top = vm->method_stack[vm->method_stack_pos-1];
    /* Methods do not have a linetable that maps opcodes to line numbers.
       Instead, the emitter writes the line number right after the opcode for
       any opcode that might call novalue_error. */ 
    top->line_num = top->code[code_pos+1];

    /* Show a var name, if possible... */
    if (sym->flags & VAR_SYM)
        lily_raise(vm->raiser, lily_ErrNoValue, "%s is nil.\n",
                ((lily_var *)sym)->name);
    else
        lily_raise(vm->raiser, lily_ErrNoValue, "Attempt to use nil value.\n");
}

/* divide_by_zero_error
   This is copied from novalue_error, except it raises ErrDivisionByZero and
   reports an attempt to divide by zero. */
void divide_by_zero_error(lily_vm_state *vm, int code_pos, lily_sym *sym)
{
    lily_vm_stack_entry *top = vm->method_stack[vm->method_stack_pos-1];
    top->line_num = top->code[code_pos+1];

    lily_raise(vm->raiser, lily_ErrDivideByZero,
            "Attempt to divide by zero.\n");
}

/* boundary_error
   Another copy of novalue_error, this one raising ErrOutOfRange. */
void boundary_error(lily_vm_state *vm, int code_pos, int pos)
{
    lily_vm_stack_entry *top = vm->method_stack[vm->method_stack_pos-1];
    top->line_num = top->code[code_pos+1];

    lily_raise(vm->raiser, lily_ErrOutOfRange,
            "Subscript index %d is out of range.\n", pos);
}

/* very_bad_subs_assign_error
   This happens when something is to be assigned to a list of object, but there
   isn't an object to assign to. This is an error, because objects should never
   be nil (parser always allocs a starting value for objects). */
static void very_bad_subs_assign_error(lily_vm_state *vm, int code_pos)
{
    lily_vm_stack_entry *top = vm->method_stack[vm->method_stack_pos-1];
    top->line_num = top->code[code_pos+1];

    lily_raise(vm->raiser, lily_ErrNoValue,
            "Subscript assign has no object to assign to!\n");
}

/** Built-in functions. These are referenced by lily_seed_symtab.h **/

/* lily_builtin_print
   This is called by the vm to implement the print function. [0] is the return
   (which isn't used), so args begin at [1]. */
void lily_builtin_print(lily_vm_state *vm, int num_args, lily_sym **args)
{
    lily_impl_send_html(args[0]->value.str->str);
}

/* lily_builtin_printfmt
   This is called by the vm to implement the printfmt function. [0] is the
   return, which is ignored in this case. [1] is the format string, and [2]+
   are the arguments. */
void lily_builtin_printfmt(lily_vm_state *vm, int num_args, lily_sym **args)
{
    char fmtbuf[64];
    char save_ch;
    char *fmt, *str_start;
    int cls_id, is_nil;
    int arg_pos = 0, i = 0;
    lily_sym *arg;
    lily_value val;

    fmt = args[0]->value.str->str;
    str_start = fmt;
    while (1) {
        if (fmt[i] == '\0')
            break;
        else if (fmt[i] == '%') {
            if (arg_pos == num_args)
                return;

            save_ch = fmt[i];
            fmt[i] = '\0';
            lily_impl_send_html(str_start);
            fmt[i] = save_ch;
            i++;

            arg = args[arg_pos + 1];
            cls_id = arg->value.object->sig->cls->id;
            val = arg->value.object->value;
            is_nil = 0;

            if (fmt[i] == 'i') {
                if (cls_id != SYM_CLASS_INTEGER)
                    return;
                if (is_nil)
                    lily_impl_send_html("(nil)");
                else {
                    snprintf(fmtbuf, 63, "%" PRId64, val.integer);
                    lily_impl_send_html(fmtbuf);
                }
            }
            else if (fmt[i] == 's') {
                if (cls_id != SYM_CLASS_STR)
                    return;
                if (is_nil)
                    lily_impl_send_html("(nil)");
                else
                    lily_impl_send_html(val.str->str);
            }
            else if (fmt[i] == 'n') {
                if (cls_id != SYM_CLASS_NUMBER)
                    return;

                if (is_nil)
                    lily_impl_send_html("(nil)");
                else {
                    snprintf(fmtbuf, 63, "%f", val.number);
                    lily_impl_send_html(fmtbuf);
                }
            }

            str_start = fmt + i + 1;
            arg_pos++;
        }
        i++;
    }

    lily_impl_send_html(str_start);
}

/** VM opcode helpers **/

/* op_ref_assign
   VM helper called for handling complex assigns. [1] is lhs, [2] is rhs. This
   does an assign along with the appropriate ref/deref stuff. This is suitable
   for anything that needs that ref/deref stuff except for object. */
void op_ref_assign(lily_sym **syms)
{
    lily_sym *lhs, *rhs;
    lily_value value;

    lhs = ((lily_sym *)syms[2]);
    rhs = ((lily_sym *)syms[1]);
    value = lhs->value;

    if (!(lhs->flags & S_IS_NIL))
        lily_deref_unknown_val(lhs->sig, value);
    if (!(rhs->flags & S_IS_NIL)) {
        rhs->value.generic->refcount++;
        lhs->flags &= ~S_IS_NIL;
    }
    else
        lhs->flags |= S_IS_NIL;
    lhs->value = rhs->value;
}

/* op_sub_assign
   This handles the o_sub_assign opcode for the vm. This first unpacks the 2, 3,
   and 4 as the lhs, the index, and the rhs. Once it checks to make sure the
   index is valid, then 'lhs[index] = rhs' is performed. This is a bit complex
   because it has to handle any kind of assign.
   Sometimes, value will be a storage from o_subscript. However, this code is
   required because it assigns to the value in the list, instead of a storage
   where that value is unloaded. */
void op_sub_assign(lily_vm_state *vm, uintptr_t *code, int pos)
{
    lily_sym *lhs, *rhs;
    lily_sym *index_sym;
    int index_int;
    lily_value *values;
    int flags;

    lhs = ((lily_sym *)code[pos+2]);
    if (lhs->flags & S_IS_NIL)
        novalue_error(vm, pos, lhs);
    values = lhs->value.list->values;

    index_sym = ((lily_sym *)code[pos+3]);
    if (index_sym->flags & S_IS_NIL)
        novalue_error(vm, pos, index_sym);
    index_int = index_sym->value.integer;

    rhs = ((lily_sym *)code[pos+4]);
    if (rhs->flags & S_IS_NIL)
        novalue_error(vm, pos, rhs);

    if (index_int >= lhs->value.list->num_values)
        boundary_error(vm, pos, index_int);

    /* todo: Wraparound would be nice. */
    if (index_int < 0)
        boundary_error(vm, pos, index_int);

    flags = lhs->value.list->flags[index_int];

    /* If this list does not contain objects, then standard
       assign or ref/deref assign is enough. */
    if (lhs->sig->node.value_sig->cls->id != SYM_CLASS_OBJECT) {
        if (rhs->sig->cls->is_refcounted) {
            /* Don't touch circular or nil values. */
            if (flags == 0)
                /* Deref whatever is inside of this. rhs->sig is used because
                   it's simpler than lhs->sig->node.value_sig. */
                lily_deref_unknown_val(rhs->sig, values[index_int]);

            /* If the right list contains the left (and the left will soon
               contain the right), do NOT ref the list. It's circular, so tag it
               as being circular instead.
               This only applies if both lists hold objects at some point.
               Otherwise, a circular ref is impossible and nested_list_check is
               false. */
            if (rhs->sig->cls->id == SYM_CLASS_LIST &&
                nested_list_check(lhs, rhs))
                flags |= S_IS_CIRCULAR;
            else {
                flags &= ~S_IS_CIRCULAR;
                rhs->value.generic->refcount++;
            }
        }
        /* else nothing to ref/deref, so do assign only. */

        values[index_int] = rhs->value;
    }
    else {
        /* Objects are supposed to never be nil, so this should be impossible.
           The name reflects this. */
        if (flags & S_IS_NIL)
            very_bad_subs_assign_error(vm, pos);

        /* Do an object assign to the value. */
        lily_object_val *ov = values[index_int].object;
        if (rhs->sig->cls->id != SYM_CLASS_OBJECT) {
            /* Only drop the ref if it's not circular. */
            if (ov->sig && ov->sig->cls->is_refcounted &&
                (flags & S_IS_CIRCULAR) == 0)
                lily_deref_unknown_val(ov->sig, ov->value);

            if (rhs->sig->cls->id == SYM_CLASS_LIST) {
                rhs->value.generic->refcount++;
                int circle_count;
                /* Does this list contain any circular references back to the
                   object that holds it? If so, adjust the refcount. */
                circle_count = circle_buster(ov, rhs->sig, rhs->value);

                if (circle_count) {
                    flags |= S_IS_CIRCULAR;
                    lily_deref_list_val_by(rhs->sig, rhs->value.list,
                            circle_count);
                }
                else
                    flags &= ~S_IS_CIRCULAR;
            }
            else {
                flags &= ~S_IS_CIRCULAR;
                if (rhs->sig->cls->is_refcounted)
                    rhs->value.generic->refcount++;

                circle_buster(ov, rhs->sig, rhs->value);
            }

            ov->sig = rhs->sig;
            ov->value = rhs->value;
        }
        else {
            lily_object_val *rhs_obj = rhs->value.object;
            /* Make sure it's refcounted AND it's not circular. */
            if (ov->sig->cls->is_refcounted && flags == 0)
                lily_deref_unknown_val(ov->sig, ov->value);

            if (rhs_obj->sig->cls->is_refcounted)
                rhs_obj->value.generic->refcount++;

            ov->sig = rhs_obj->sig;
            ov->value = rhs_obj->value;
            flags &= ~S_IS_CIRCULAR;
        }
    }

    /* rhs was verified to be non-nil, so make sure the flags reflect that. This
       is necessary because list creation allows for nil elements. */
    flags &= ~S_IS_NIL;

    lhs->value.list->flags[index_int] = flags;
}

/* op_build_list
   VM helper called for handling o_build_list. This is a bit tricky, becaus the
   storage may have already had a previous list assigned to it. Additionally,
   the new list info may fail to allocate.
   But most importantly, list handling is a bit broken right now. Lists keep a
   copy of their element sig in the value and in the value part of their own
   sig. This...is a problem that will eventually get fixed. */
void op_build_list(lily_vm_state *vm, lily_sym **syms, int i)
{
    int num_elems = (intptr_t)(syms[2]);
    int j;
    lily_storage *storage = (lily_storage *)syms[3+num_elems];
    lily_sig *elem_sig = storage->sig->node.value_sig;

    lily_list_val *lv = lily_malloc(sizeof(lily_list_val));
    if (lv == NULL)
        lily_raise_nomem(vm->raiser);

    lv->visited = 0;
    lv->values = lily_malloc(num_elems * sizeof(void *));
    if (lv->values == NULL) {
        lily_free(lv);
        lily_raise_nomem(vm->raiser);
    }

    lv->flags = lily_malloc(num_elems * sizeof(int));
    if (lv->flags == NULL) {
        lily_free(lv->values);
        lily_free(lv);
        lily_raise_nomem(vm->raiser);
    }

    /* It's possible that the storage this will assign to was used to
       assign a different list. Deref the old value, or it won't be
       collected. This must be done after allocating the new value,
       because symtab will want to deref the storage during cleanup
       (resulting in an extra deref). */

    if (!(storage->flags & S_IS_NIL))
        lily_deref_list_val(storage->sig, storage->value.list);

    /* This could be condensed down, but doing it this way improves speed since
       the elem_sig won't change over the loop. */
    if (elem_sig->cls->id != SYM_CLASS_OBJECT) {
        if (elem_sig->cls->is_refcounted) {
            for (j = 0;j < num_elems;j++) {
                if (!(syms[3+j]->flags & S_IS_NIL)) {
                    lv->values[j] = syms[3+j]->value;
                    lv->values[j].generic->refcount++;
                    lv->flags[j] = 0;
                }
                else
                    lv->flags[j] = S_IS_NIL;
            }
        }
        else {
            for (j = 0;j < num_elems;j++) {
                if (!(syms[3+j]->flags & S_IS_NIL)) {
                    lv->values[j] = syms[3+j]->value;
                    lv->flags[j] = 0;
                }
                else
                    lv->flags[j] = S_IS_NIL;
            }
        }
    }
    else {
        for (j = 0;j < num_elems;j++) {
            if (!(syms[3+j]->flags & S_IS_NIL)) {
                /* Without copying to a separate object:
                   object o = 10    o = [o]
                   (o is now a useless circular reference. What?)

                   With copying to a separate object:
                   object o = 10    o = [o]
                   (o is now a list of object, with [0] being 10. */
                lily_object_val *oval = lily_try_new_object_val();
                if (oval == NULL) {
                    /* The inner values must be destroyed here, because the
                       symtab has no linkage for them. */
                    int k;
                    for (k = 0;k < j;k++) {
                        if (lv->flags[k] == 0)
                            lily_deref_object_val(lv->values[k].object);
                    }
                    lily_free(lv->flags);
                    lily_free(lv->values);
                    lily_free(lv);
                    lily_raise_nomem(vm->raiser);
                }
                memcpy(oval, syms[3+j]->value.object, sizeof(lily_object_val));
                oval->refcount = 1;

                if (oval->sig && oval->sig->cls->is_refcounted)
                    oval->value.generic->refcount++;

                lv->values[j].object = oval;
                lv->flags[j] = 0;
            }
            else
                lv->flags[j] = S_IS_NIL;
        }
    }

    lv->num_values = num_elems;
    lv->refcount = 1;
    storage->value.list = lv;
    storage->flags &= ~S_IS_NIL;
}

static void do_keyword_show(lily_vm_state *vm, lily_sym *sym)
{
    lily_show_sym(sym, vm->raiser->msgbuf);
}

/* do_ref_deref
   Do an assignment where lhs loses a ref and rhs gains one. */
static void do_ref_deref(lily_class *cls, lily_sym *down_sym, lily_sym *up_sym)
{
    lily_generic_val *up_gv = up_sym->value.generic;

    if ((up_sym->flags & S_IS_NIL) == 0)
        up_gv->refcount++;

    if ((down_sym->flags & S_IS_NIL) == 0) {
        if (cls->id == SYM_CLASS_STR)
            lily_deref_str_val(down_sym->value.str);
        else if (cls->id == SYM_CLASS_METHOD)
            lily_deref_method_val(down_sym->value.method);
        else if (cls->id == SYM_CLASS_LIST)
            lily_deref_list_val(down_sym->sig, down_sym->value.list);
        else if (cls->id == SYM_CLASS_OBJECT)
            lily_deref_object_val(down_sym->value.object);
    }
}

/** The mighty VM **/

/* lily_vm_execute
   This is the VM part of lily. It executes any code on @main, as well as
   anything called by @main. Finishes when it encounters the o_vm_return
   opcode.
   This function occasionally farms work out to other routines to keep the size
   from being too big. It does not recurse, instead saving everything necessary
   to the vm state for each call. */
void lily_vm_execute(lily_vm_state *vm)
{
    uintptr_t *code;
    int i, j, k;
    lily_method_val *mval;
    lily_sig *cast_sig;
    lily_sym *lhs, *rhs, *loop_var, *step_var;
    lily_var *v;
    lily_method_val *m;
    lily_vm_stack_entry *stack_entry;
    int64_t for_temp;

    m = vm->main->value.method;
    code = m->code;
    i = 0;

    /* The stack always contains the information of the current method. The line
       number of the current entry will be filled in on the next call. When an
       error is thrown, it will need to write in the line number of the last
       call level. */
    stack_entry = vm->method_stack[0];
    stack_entry->method = ((lily_sym *)vm->main)->value.method;
    stack_entry->code = code;
    vm->method_stack_pos = 1;

    if (setjmp(vm->raiser->jumps[vm->raiser->jump_pos]) == 0)
        vm->raiser->jump_pos++;
    else {
        if (vm->in_function) {
            vm->err_function = ((lily_var *)code[i+2])->value.function;

            lily_vm_stack_entry *top = vm->method_stack[vm->method_stack_pos-1];
            top->line_num = top->code[i+1];
        }

        /* Don't yield to parser, because it will continue as if nothing
           happened. Instead, jump to where it would jump. */
        longjmp(vm->raiser->jumps[vm->raiser->jump_pos-2], 1);
    }

    while (1) {
        switch(code[i]) {
            case o_assign:
                rhs = ((lily_sym *)code[i+2]);
                lhs = ((lily_sym *)code[i+3]);

                COPY_NIL_FLAG(lhs, rhs->flags)
                lhs->value = rhs->value;
                i += 4;
                break;
            case o_integer_add:
                INTEGER_OP(+)
                break;
            case o_integer_minus:
                INTEGER_OP(-)
                break;
            case o_number_add:
                INTNUM_OP(+)
                break;
            case o_number_minus:
                INTNUM_OP(-)
                break;
            case o_less:
                COMPARE_OP(<, == -1)
                break;
            case o_less_eq:
                COMPARE_OP(<=, <= 0)
                break;
            case o_is_equal:
                COMPARE_OP(==, == 0)
                break;
            case o_greater:
                COMPARE_OP(>, == 1)
                break;
            case o_greater_eq:
                COMPARE_OP(>, >= 0)
                break;
            case o_not_eq:
                COMPARE_OP(!=, != 0)
                break;
            case o_jump:
                i = code[i+1];
                break;
            case o_modulo:
                INTEGER_OP(%)
                break;
            case o_integer_mul:
                INTEGER_OP(*)
                break;
            case o_number_mul:
                INTNUM_OP(*)
                break;
            case o_integer_div:
                /* Before doing INTEGER_OP, check for a division by zero. This
                   will involve some redundant checking of the rhs, but better
                   than dumping INTEGER_OP's contents here or rewriting
                   INTEGER_OP for the special case of division. */
                rhs = (lily_sym *)code[i+3];
                if (rhs->flags & S_IS_NIL)
                    novalue_error(vm, i, rhs);
                if (rhs->value.integer == 0)
                    divide_by_zero_error(vm, i, rhs);
                INTEGER_OP(/)
                break;
            case o_left_shift:
                INTEGER_OP(<<)
                break;
            case o_right_shift:
                INTEGER_OP(>>)
                break;
            case o_bitwise_and:
                INTEGER_OP(&)
                break;
            case o_bitwise_or:
                INTEGER_OP(|)
                break;
            case o_bitwise_xor:
                INTEGER_OP(^)
                break;
            case o_number_div:
                /* This is a little more tricky, because the rhs could be a
                   number or an integer... */
                rhs = (lily_sym *)code[i+3];
                if (rhs->flags & S_IS_NIL)
                    novalue_error(vm, i, rhs);
                if (rhs->sig->cls->id == SYM_CLASS_INTEGER &&
                    rhs->value.integer == 0)
                    divide_by_zero_error(vm, i, rhs);
                else if (rhs->sig->cls->id == SYM_CLASS_NUMBER &&
                         rhs->value.number == 0)
                    divide_by_zero_error(vm, i, rhs);

                INTNUM_OP(/)
                break;
            case o_jump_if:
                lhs = (lily_sym *)code[i+2];
                {
                    int cls_id, result;
                    cls_id = lhs->sig->cls->id;

                    if (cls_id == SYM_CLASS_INTEGER)
                        result = (lhs->value.integer == 0);
                    else if (cls_id == SYM_CLASS_NUMBER)
                        result = (lhs->value.number == 0);
                    else if (cls_id == SYM_CLASS_OBJECT) {
                        if (lhs->value.object->sig->cls->id == SYM_CLASS_INTEGER)
                            result = (lhs->value.object->value.integer == 0);
                        else if (lhs->value.object->sig->cls->id == SYM_CLASS_NUMBER)
                            result = (lhs->value.object->value.number == 0);
                        else
                            /* Objects will never have S_IS_NIL set because they
                               must always exist so they can always store a
                               value. So check for a sig, instead of a value. */
                            result = (lhs->value.object->sig == NULL);
                    }
                    else
                        result = (lhs->flags & S_IS_NIL);

                    if (result != code[i+1])
                        i = code[i+3];
                    else
                        i += 4;
                }
                break;
            case o_func_call:
            {
                /* var, func, #args, ret, args... */
                lily_function_val *fval;
                lily_func func;
                int j = code[i+3];

                /* The func HAS to be grabbed from the var to support passing
                   funcs as args. */
                fval = (lily_function_val *)((lily_var *)code[i+2])->value.function;
                func = fval->func;

                vm->in_function = 1;
                func(vm, j, (lily_sym **)code+i+4);
                vm->in_function = 0;
                i += 5 + j;
            }
                break;
            case o_save:
                j = code[i+1];

                if (vm->val_pos + j > vm->val_size)
                    grow_saved_vals(vm, vm->val_pos + j);

                i += 2;
                for (k = 0;k < j;k++, i++, vm->val_pos++) {
                    lhs = (lily_sym *)code[i];
                    vm->saved_values[vm->val_pos].sym = lhs;
                    vm->saved_values[vm->val_pos].flags = lhs->flags;
                    vm->saved_values[vm->val_pos].value = lhs->value;
                }
                break;
            case o_restore:
                /* o_save finishes with the position ahead, so fix that. */
                vm->val_pos--;
                for (j = code[i+1];j > 0;j--,vm->val_pos--) {
                    lhs = vm->saved_values[vm->val_pos].sym;
                    lhs->flags = vm->saved_values[vm->val_pos].flags;
                    lhs->value = vm->saved_values[vm->val_pos].value;
                }
                /* Make it point to the spot to be saved to again. */
                vm->val_pos++;
                i += 2;
                break;
            case o_method_call:
            {
                int j;
                if (vm->method_stack_pos+1 == vm->method_stack_size)
                    grow_method_stack(vm);

                /* This has to be grabbed each time, because of methods passing
                   as args. This can't be written in at emit time, because the
                   method arg would be empty then. */
                mval = ((lily_var *)code[i+2])->value.method;
                v = mval->first_arg;
                j = i + 4 + code[i+3];

                stack_entry = vm->method_stack[vm->method_stack_pos-1];
                stack_entry->line_num = code[i+1];
                /* j is where the return is, so one after that will be where the
                   next opcode starts. Don't use i because it doesn't count
                   args. */
                stack_entry->code_pos = j + 1;

                stack_entry->ret = (lily_sym *)code[j];
                stack_entry = vm->method_stack[vm->method_stack_pos];

                /* Add this entry to the call stack. */
                stack_entry->method = mval;

                i += 4;
                /* Map call values to method arguments. */
                for (v = mval->first_arg; i < j;v = v->next, i++) {
                    lily_sym *arg = (lily_sym *)code[i];

                    if (v->sig->cls->is_refcounted)
                        do_ref_deref(v->sig->cls, (lily_sym *)v, arg);

                    COPY_NIL_FLAG(v, arg->flags)
                    v->value = arg->value;
                }

                stack_entry->code = mval->code;
                vm->method_stack_pos++;

                /* Finally, load up the new code to run. */
                code = mval->code;
                i = 0;
            }
                break;
            case o_unary_not:
                lhs = (lily_sym *)code[i+2];
                if (lhs->flags & S_IS_NIL)
                    novalue_error(vm, i, lhs);
                rhs = (lily_sym *)code[i+3];
                rhs->flags &= ~S_IS_NIL;
                rhs->value.integer = !(lhs->value.integer);
                i += 4;
                break;
            case o_unary_minus:
                lhs = (lily_sym *)code[i+2];
                if (lhs->flags & S_IS_NIL)
                    novalue_error(vm, i, lhs);
                rhs = (lily_sym *)code[i+3];
                rhs->flags &= ~S_IS_NIL;
                rhs->value.integer = -lhs->value.integer;
                i += 4;
                break;
            case o_return_val:
                vm->method_stack_pos--;
                stack_entry = vm->method_stack[vm->method_stack_pos-1];
                lhs = stack_entry->ret;
                rhs = (lily_sym *)code[i+2];

                if (lhs->sig->cls->is_refcounted)
                    do_ref_deref(lhs->sig->cls, (lily_sym *)lhs,
                                 (lily_sym *)rhs);

                /* This may have been an accidental return of a nil value. */
                COPY_NIL_FLAG(lhs, rhs->flags)
                lhs->value = rhs->value;
                code = stack_entry->code;
                i = stack_entry->code_pos;
                break;
            case o_return_noval:
                vm->method_stack_pos--;
                stack_entry = vm->method_stack[vm->method_stack_pos-1];
                code = stack_entry->code;
                i = stack_entry->code_pos;
                break;
            case o_subscript:
                lhs = (lily_sym *)code[i+2];
                rhs = (lily_sym *)code[i+3];
                if (lhs->flags & S_IS_NIL)
                    novalue_error(vm, i, lhs);

                if (rhs->flags & S_IS_NIL)
                    novalue_error(vm, i, rhs);

                {
                    /* lhs is the var, rhs is the subscript. Emitter has
                       verified that rhs is an integer. */
                    int rhs_index = rhs->value.integer;
                    lily_sym *result = ((lily_sym *)code[i+4]);

                    /* Too big! */
                    if (rhs_index >= lhs->value.list->num_values)
                        boundary_error(vm, i, rhs_index);

                    /* todo: Wraparound would be nice. */
                    if (rhs < 0)
                        boundary_error(vm, i, rhs_index);

                    if ((result->flags & S_IS_NIL) == 0) {
                        /* Do not use && to combine these two if's, because
                           objects are marked as refcounted. This would be a
                           no-op now, but could be hazardous in the future. */
                        if (result->sig->cls->id == SYM_CLASS_OBJECT) {
                            if (result->value.object->sig != NULL) {
                                lily_deref_unknown_val(
                                        result->value.object->sig,
                                        result->value.object->value);
                            }
                        }
                        else if (result->sig->cls->is_refcounted)
                            lily_deref_unknown_val(result->sig, result->value);
                        /* no-op if not refcounted. */
                    }

                    if (lhs->value.list->flags[rhs_index] == 0) {
                        if (result->sig->cls->id != SYM_CLASS_OBJECT) {
                            if (result->sig->cls->is_refcounted) {
                                result->value =
                                        lhs->value.list->values[rhs_index];
                                result->value.generic->refcount++;
                            }
                            else
                                result->value =
                                        lhs->value.list->values[rhs_index];
                        }
                        else {
                            lily_object_val *oval;
                            oval = lhs->value.list->values[rhs_index].object;

                            if (oval->sig) {
                                if (oval->sig->cls->is_refcounted)
                                    oval->value.generic->refcount++;

                                result->value.object->value = oval->value;
                            }
                            result->value.object->sig = oval->sig;
                        }

                        result->flags &= ~S_IS_NIL;
                    }
                    else if (result->sig->cls->id == SYM_CLASS_OBJECT &&
                             lhs->value.list->flags[rhs_index] & S_IS_CIRCULAR) {
                        lily_object_val *oval;
                        oval = lhs->value.list->values[rhs_index].object;

                        /* Don't check if it's refcounted: If it were not, then
                           it wouldn't be tagged as circular. */
                        oval->value.generic->refcount++;
                        result->value.object->value = oval->value;
                        result->value.object->sig = oval->sig;

                        result->flags &= ~S_IS_NIL;
                    }
                    else
                        result->flags |= S_IS_NIL;
                }
                i += 5;
                break;
            case o_sub_assign:
                op_sub_assign(vm, code, i);
                i += 5;
                break;
            case o_build_list:
                op_build_list(vm, (lily_sym **)code+i, i);
                i += code[i+2] + 4;
                break;
            case o_ref_assign:
                op_ref_assign(((lily_sym **)code+i+1));
                i += 4;
                break;
            case o_show:
                rhs = (lily_sym *)code[i+2];
                do_keyword_show(vm, rhs);
                i += 3;
                break;
            case o_obj_typecast:
                rhs = ((lily_sym *)code[i+2]);
                lhs = ((lily_sym *)code[i+3]);
                cast_sig = lhs->sig;

                if (rhs->flags & S_IS_NIL || rhs->value.object->sig == NULL)
                    novalue_error(vm, i, rhs);

                if (lily_sigequal(cast_sig, rhs->value.object->sig)) {
                    if (lhs->sig->cls->is_refcounted) {
                        rhs->value.object->value.generic->refcount++;
                        if ((lhs->flags & S_IS_NIL) == 0)
                            lily_deref_unknown_val(lhs->sig, lhs->value);
                        else
                            lhs->flags &= ~S_IS_NIL;

                        lhs->value = rhs->value.object->value;
                    }
                    else {
                        lhs->value = rhs->value.object->value;
                        lhs->flags &= ~S_IS_NIL;
                    }
                }
                /* Since integer and number can be cast between each other,
                   allow that with object casts as well. */
                else if (maybe_crossover_assign(lhs, rhs) == 0) {
                    lily_vm_stack_entry *top;
                    top = vm->method_stack[vm->method_stack_pos-1];
                    top->line_num = top->code[i+1];

                    lily_raise(vm->raiser, lily_ErrBadCast,
                            "Cannot cast object containing type '%T' to type '%T'.\n",
                            rhs->value.object->sig, lhs->sig);
                }

                i += 4;
                break;
            case o_obj_assign:
                lhs = ((lily_sym *)code[i+3]);
                rhs = ((lily_sym *)code[i+2]);

                if (lhs->value.object->sig != NULL) {
                    if (lhs->value.object->sig->cls->is_refcounted)
                        lily_deref_unknown_val(lhs->value.object->sig,
                                lhs->value.object->value);
                }

                /* Objects are treated like containers, wherein the value and
                   the sig are ref/deref'd here, instead of the actual
                   object. */
                if (rhs->sig->cls->id == SYM_CLASS_OBJECT) {
                    if (rhs->value.object->sig == NULL) {
                        lhs->value.object->sig = NULL;
                    }
                    else {
                        /* object = object. Grab what's inside of the rhs
                           object. This has the nice side-effect of making sure
                           that objects aren't super-nested in each other. */
                        lily_object_val *rhs_obj = rhs->value.object;
                        lhs->value.object->sig = rhs_obj->sig;
                        if (rhs_obj->sig->cls->is_refcounted)
                            rhs_obj->value.generic->refcount++;
                        lhs->value.object->value = rhs_obj->value;
                    }
                }
                else {
                    /* object = !object
                       Just copy the sig and the value over, but make sure that
                       there isn't a circular ref. */
                    if (rhs->sig->cls->id == SYM_CLASS_LIST)
                        circle_buster(lhs->value.object, rhs->sig, rhs->value);

                    lhs->value.object->sig = rhs->sig;
                    if (rhs->sig->cls->is_refcounted)
                        rhs->value.generic->refcount++;

                    lhs->value.object->value = rhs->value;
                }
                i += 4;
                break;
            case o_intnum_typecast:
                rhs = (lily_sym *)code[i+2];
                lhs = (lily_sym *)code[i+3];
                if (rhs->flags & S_IS_NIL)
                    novalue_error(vm, i, rhs);

                /* Guaranteed to work, because rhs is non-nil and emitter has
                   already verified the types. This will also make sure that the
                   nil flag isn't set on lhs. */
                maybe_crossover_assign(lhs, rhs);
                i += 4;
                break;
            case o_integer_for:
                loop_var = (lily_sym *)code[i+2];
                /* lhs is the start, and also incremented. This is done so that
                   user assignments cannot cause the loop to leave early. This
                   may be changed in the future.
                   rhs is the stop value. */
                lhs      = (lily_sym *)code[i+3];
                rhs      = (lily_sym *)code[i+4];
                step_var = (lily_sym *)code[i+5];

                for_temp = lhs->value.integer + step_var->value.integer;
                /* Copied from Lua's for loop. */
                if ((step_var->value.integer > 0)
                        /* Positive bound check */
                        ? (for_temp <= rhs->value.integer)
                        /* Negative bound check */
                        : (for_temp >= rhs->value.integer)) {

                    /* Haven't reached the end yet, so bump the internal and
                       external values.*/
                    lhs->value.integer = for_temp;
                    loop_var->value.integer = for_temp;
                    /* The loop var may have been altered and set nil. Make sure
                       it is not nil. */
                    loop_var->flags &= ~S_IS_NIL;
                    i += 7;
                }
                else
                    i = code[i+6];

                break;
            case o_for_setup:
                loop_var = (lily_sym *)code[i+2];
                /* lhs is the start, rhs is the stop. */
                lhs      = (lily_sym *)code[i+3];
                rhs      = (lily_sym *)code[i+4];
                step_var = (lily_sym *)code[i+5];

                if (lhs->flags & S_IS_NIL)
                    novalue_error(vm, i, lhs);
                if (rhs->flags & S_IS_NIL)
                    novalue_error(vm, i, rhs);

                /* +6 is used to indicate if the step needs to be generated, or
                   if it's already calculated. */
                if (code[i+6] == 1) {
                    if (lhs->value.integer <= rhs->value.integer)
                        step_var->value.integer = +1;
                    else
                        step_var->value.integer = -1;
                    step_var->flags &= ~S_IS_NIL;
                }
                else {
                    if (step_var->flags & S_IS_NIL)
                        novalue_error(vm, i, step_var);

                    if (step_var->value.integer == 0)
                        lily_raise(vm->raiser, lily_ErrBadValue,
                                   "for loop step cannot be 0.\n");
                }

                loop_var->value.integer = lhs->value.integer;
                loop_var->flags &= ~S_IS_NIL;

                i += 7;
                break;
            case o_return_expected:
            {
                lily_vm_stack_entry *top;
                top = vm->method_stack[vm->method_stack_pos-1];
                top->line_num = top->code[i+1];
                lily_raise(vm->raiser, lily_ErrReturnExpected,
                        "Method %s completed without returning a value.\n",
                        top->method->trace_name);
            }
                break;
            case o_return_from_vm:
                /* Remember to remove the jump that the vm installed, since it's
                   no longer valid. */
                vm->raiser->jump_pos--;
                return;
        }
    }
}
