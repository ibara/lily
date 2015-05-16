#include <string.h>

#include "lily_alloc.h"
#include "lily_core_types.h"
#include "lily_lexer.h"
#include "lily_vm.h"
#include "lily_seed.h"

int lily_bytestring_eq(lily_vm_state *vm, int *depth, lily_value *left,
        lily_value *right)
{
    int ret;

    if (left->value.string->size == right->value.string->size &&
        (left->value.string == right->value.string ||
         memcmp(left->value.string->string, right->value.string->string,
                left->value.string->size) == 0))
        ret = 1;
    else
        ret = 0;

    return ret;
}

void lily_bytestring_encode(lily_vm_state *vm, lily_function_val *self,
        uint16_t *code)
{
    lily_value **vm_regs = vm->vm_regs;
    lily_string_val *input_bytestring = vm_regs[code[0]]->value.string;
    char *encode_method = vm_regs[code[1]]->value.string->string;
    lily_value *result = vm_regs[code[2]];

    if (strcmp(encode_method, "error") != 0) {
        lily_raise(vm->raiser, lily_ValueError,
                "Encode option should be either 'ignore' or 'error'.\n");
    }

    char *byte_buffer = input_bytestring->string;
    int byte_buffer_size = input_bytestring->size;

    if (lily_is_valid_utf8(byte_buffer, byte_buffer_size) == 0) {
        lily_raise(vm->raiser, lily_ValueError,
                "Invalid utf-8 sequence found in buffer.\n");
    }

    lily_string_val *new_sv = lily_malloc(sizeof(lily_string_val));
    char *sv_buffer = lily_malloc(byte_buffer_size + 1);

    /* The utf-8 validator function also ensures that there are no embedded
       \0's, so it's safe to use strcpy. */
    strcpy(sv_buffer, byte_buffer);

    new_sv->refcount = 1;
    new_sv->string = sv_buffer;
    new_sv->size = byte_buffer_size;

    lily_raw_value v = {.string = new_sv};

    lily_move_raw_value(vm, result, 0, v);
}

static const lily_func_seed encode =
    {NULL, "encode", dyna_function, "function encode(bytestring, string => string)", lily_bytestring_encode};

int lily_bytestring_setup(lily_symtab *symtab, lily_class *cls)
{
    cls->dynaload_table = &encode;
    return 1;
}