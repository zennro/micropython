#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "nlr.h"
#include "misc.h"
#include "mpconfig.h"
#include "qstr.h"
#include "obj.h"
#include "runtime.h"
#include "bc0.h"
#include "bc.h"

// (value) stack grows down (to be compatible with native code when passing pointers to the stack), top element is pointed to
// exception stack grows up, top element is pointed to

#define DECODE_UINT do { unum = *ip++; if (unum > 127) { unum = ((unum & 0x3f) << 8) | (*ip++); } } while (0)
#define DECODE_ULABEL do { unum = (ip[0] | (ip[1] << 8)); ip += 2; } while (0)
#define DECODE_SLABEL do { unum = (ip[0] | (ip[1] << 8)) - 0x8000; ip += 2; } while (0)
#define DECODE_QSTR do { qst = *ip++; if (qst > 127) { qst = ((qst & 0x3f) << 8) | (*ip++); } } while (0)
#define PUSH(val) *++sp = (val)
#define POP() (*sp--)
#define TOP() (*sp)
#define SET_TOP(val) *sp = (val)

mp_obj_t mp_execute_byte_code(const byte *code, const mp_obj_t *args, uint n_args, uint n_state) {
    n_state += 1; // XXX there is a bug somewhere which doesn't count enough state... (conwaylife and mandel have the bug)

    // allocate state for locals and stack
    mp_obj_t temp_state[10];
    mp_obj_t *state = &temp_state[0];
    if (n_state > 10) {
        state = m_new(mp_obj_t, n_state);
    }
    mp_obj_t *sp = &state[0] - 1;

    // init args
    for (int i = 0; i < n_args; i++) {
        assert(i < 8);
        state[n_state - 1 - i] = args[i];
    }

    const byte *ip = code;

    // get code info size
    machine_uint_t code_info_size = ip[0] | (ip[1] << 8) | (ip[2] << 16) | (ip[3] << 24);
    ip += code_info_size;

    // execute prelude to make any cells (closed over variables)
    {
        for (uint n_local = *ip++; n_local > 0; n_local--) {
            uint local_num = *ip++;
            if (local_num < n_args) {
                state[n_state - 1 - local_num] = mp_obj_new_cell(state[n_state - 1 - local_num]);
            } else {
                state[n_state - 1 - local_num] = mp_obj_new_cell(MP_OBJ_NULL);
            }
        }
    }

    // execute the byte code
    if (mp_execute_byte_code_2(code, &ip, &state[n_state - 1], &sp)) {
        // it shouldn't yield
        assert(0);
    }

    // TODO check fails if, eg, return from within for loop
    //assert(sp == &state[17]);
    return *sp;
}

// fastn has items in reverse order (fastn[0] is local[0], fastn[-1] is local[1], etc)
// sp points to bottom of stack which grows up
// returns true if bytecode yielded
bool mp_execute_byte_code_2(const byte *code_info, const byte **ip_in_out, mp_obj_t *fastn, mp_obj_t **sp_in_out) {
    // careful: be sure to declare volatile any variables read in the exception handler (written is ok, I think)

    const byte *ip = *ip_in_out;
    mp_obj_t *sp = *sp_in_out;
    machine_uint_t unum;
    qstr qst;
    mp_obj_t obj1, obj2;
    mp_obj_t fast0 = fastn[0], fast1 = fastn[-1], fast2 = fastn[-2];
    nlr_buf_t nlr;

    volatile machine_uint_t currently_in_except_block = 0; // 0 or 1, to detect nested exceptions
    machine_uint_t exc_stack[8]; // on the exception stack we store (ip, sp | X) for each block, X = previous value of currently_in_except_block
    machine_uint_t *volatile exc_sp = &exc_stack[0] - 1; // stack grows up, exc_sp points to top of stack
    const byte *volatile save_ip = ip; // this is so we can access ip in the exception handler without making ip volatile (which means the compiler can't keep it in a register in the main loop)

    // TODO if an exception occurs, do fast[0,1,2] become invalid??

    // outer exception handling loop
    for (;;) {
        if (nlr_push(&nlr) == 0) {
            // loop to execute byte code
            for (;;) {
                save_ip = ip;
                int op = *ip++;
                switch (op) {
                    case MP_BC_LOAD_CONST_FALSE:
                        PUSH(mp_const_false);
                        break;

                    case MP_BC_LOAD_CONST_NONE:
                        PUSH(mp_const_none);
                        break;

                    case MP_BC_LOAD_CONST_TRUE:
                        PUSH(mp_const_true);
                        break;

                    case MP_BC_LOAD_CONST_ELLIPSIS:
                        PUSH(mp_const_ellipsis);
                        break;

                    case MP_BC_LOAD_CONST_SMALL_INT:
                        unum = (ip[0] | (ip[1] << 8) | (ip[2] << 16)) - 0x800000;
                        ip += 3;
                        PUSH(MP_OBJ_NEW_SMALL_INT(unum));
                        break;

                    case MP_BC_LOAD_CONST_INT:
                        DECODE_QSTR;
                        PUSH(mp_obj_new_int_from_long_str(qstr_str(qst)));
                        break;

                    case MP_BC_LOAD_CONST_DEC:
                        DECODE_QSTR;
                        PUSH(rt_load_const_dec(qst));
                        break;

                    case MP_BC_LOAD_CONST_ID:
                        DECODE_QSTR;
                        PUSH(rt_load_const_str(qst)); // TODO
                        break;

                    case MP_BC_LOAD_CONST_BYTES:
                        DECODE_QSTR;
                        PUSH(rt_load_const_bytes(qst));
                        break;

                    case MP_BC_LOAD_CONST_STRING:
                        DECODE_QSTR;
                        PUSH(rt_load_const_str(qst));
                        break;

                    case MP_BC_LOAD_FAST_0:
                        PUSH(fast0);
                        break;

                    case MP_BC_LOAD_FAST_1:
                        PUSH(fast1);
                        break;

                    case MP_BC_LOAD_FAST_2:
                        PUSH(fast2);
                        break;

                    case MP_BC_LOAD_FAST_N:
                        DECODE_UINT;
                        PUSH(fastn[-unum]);
                        break;

                    case MP_BC_LOAD_DEREF:
                        DECODE_UINT;
                        if (unum == 0) {
                            obj1 = fast0;
                        } else if (unum == 1) {
                            obj1 = fast1;
                        } else if (unum == 2) {
                            obj1 = fast2;
                        } else {
                            obj1 = fastn[-unum];
                        }
                        PUSH(rt_get_cell(obj1));
                        break;

                    case MP_BC_LOAD_NAME:
                        DECODE_QSTR;
                        PUSH(rt_load_name(qst));
                        break;

                    case MP_BC_LOAD_GLOBAL:
                        DECODE_QSTR;
                        PUSH(rt_load_global(qst));
                        break;

                    case MP_BC_LOAD_ATTR:
                        DECODE_QSTR;
                        SET_TOP(rt_load_attr(TOP(), qst));
                        break;

                    case MP_BC_LOAD_METHOD:
                        DECODE_QSTR;
                        rt_load_method(*sp, qst, sp);
                        sp += 1;
                        break;

                    case MP_BC_LOAD_BUILD_CLASS:
                        PUSH(rt_load_build_class());
                        break;

                    case MP_BC_STORE_FAST_0:
                        fast0 = POP();
                        break;

                    case MP_BC_STORE_FAST_1:
                        fast1 = POP();
                        break;

                    case MP_BC_STORE_FAST_2:
                        fast2 = POP();
                        break;

                    case MP_BC_STORE_FAST_N:
                        DECODE_UINT;
                        fastn[-unum] = POP();
                        break;

                    case MP_BC_STORE_DEREF:
                        DECODE_UINT;
                        if (unum == 0) {
                            obj1 = fast0;
                        } else if (unum == 1) {
                            obj1 = fast1;
                        } else if (unum == 2) {
                            obj1 = fast2;
                        } else {
                            obj1 = fastn[-unum];
                        }
                        rt_set_cell(obj1, POP());
                        break;

                    case MP_BC_STORE_NAME:
                        DECODE_QSTR;
                        rt_store_name(qst, POP());
                        break;

                    case MP_BC_STORE_GLOBAL:
                        DECODE_QSTR;
                        rt_store_global(qst, POP());
                        break;

                    case MP_BC_STORE_ATTR:
                        DECODE_QSTR;
                        rt_store_attr(sp[0], qst, sp[-1]);
                        sp -= 2;
                        break;

                    case MP_BC_STORE_SUBSCR:
                        rt_store_subscr(sp[-1], sp[0], sp[-2]);
                        sp -= 3;
                        break;

                    case MP_BC_DUP_TOP:
                        obj1 = TOP();
                        PUSH(obj1);
                        break;

                    case MP_BC_DUP_TOP_TWO:
                        sp += 2;
                        sp[0] = sp[-2];
                        sp[-1] = sp[-3];
                        break;

                    case MP_BC_POP_TOP:
                        sp -= 1;
                        break;

                    case MP_BC_ROT_TWO:
                        obj1 = sp[0];
                        sp[0] = sp[-1];
                        sp[-1] = obj1;
                        break;

                    case MP_BC_ROT_THREE:
                        obj1 = sp[0];
                        sp[0] = sp[-1];
                        sp[-1] = sp[-2];
                        sp[-2] = obj1;
                        break;

                    case MP_BC_JUMP:
                        DECODE_SLABEL;
                        ip += unum;
                        break;

                    case MP_BC_POP_JUMP_IF_TRUE:
                        DECODE_SLABEL;
                        if (rt_is_true(POP())) {
                            ip += unum;
                        }
                        break;

                    case MP_BC_POP_JUMP_IF_FALSE:
                        DECODE_SLABEL;
                        if (!rt_is_true(POP())) {
                            ip += unum;
                        }
                        break;

                    case MP_BC_JUMP_IF_TRUE_OR_POP:
                        DECODE_SLABEL;
                        if (rt_is_true(TOP())) {
                            ip += unum;
                        } else {
                            sp--;
                        }
                        break;

                    case MP_BC_JUMP_IF_FALSE_OR_POP:
                        DECODE_SLABEL;
                        if (rt_is_true(TOP())) {
                            sp--;
                        } else {
                            ip += unum;
                        }
                        break;

                        /* we are trying to get away without using this opcode
                    case MP_BC_SETUP_LOOP:
                        DECODE_UINT;
                        // push_block(MP_BC_SETUP_LOOP, ip + unum, sp)
                        break;
                        */

                    // TODO this might need more sophisticated handling when breaking from within an except
                    case MP_BC_BREAK_LOOP:
                        DECODE_ULABEL;
                        ip += unum;
                        break;

                    // TODO this might need more sophisticated handling when breaking from within an except
                    case MP_BC_CONTINUE_LOOP:
                        DECODE_ULABEL;
                        ip += unum;
                        break;

                    // matched against: POP_BLOCK or POP_EXCEPT (anything else?)
                    case MP_BC_SETUP_EXCEPT:
                        DECODE_ULABEL; // except labels are always forward
                        *++exc_sp = (machine_uint_t)ip + unum;
                        *++exc_sp = (((machine_uint_t)sp) | currently_in_except_block);
                        currently_in_except_block = 0; // in a try block now
                        break;

                    case MP_BC_END_FINALLY:
                        // not implemented
                        // if TOS is an exception, reraises the exception (3 values on TOS)
                        // if TOS is an integer, does something else
                        // if TOS is None, just pops it and continues
                        // else error
                        assert(0);
                        break;

                    case MP_BC_GET_ITER:
                        SET_TOP(rt_getiter(TOP()));
                        break;

                    case MP_BC_FOR_ITER:
                        DECODE_ULABEL; // the jump offset if iteration finishes; for labels are always forward
                        obj1 = rt_iternext(TOP());
                        if (obj1 == mp_const_stop_iteration) {
                            --sp; // pop the exhausted iterator
                            ip += unum; // jump to after for-block
                        } else {
                            PUSH(obj1); // push the next iteration value
                        }
                        break;

                    // matched against: SETUP_EXCEPT, SETUP_FINALLY, SETUP_WITH
                    case MP_BC_POP_BLOCK:
                        // we are exiting an exception handler, so pop the last one of the exception-stack
                        assert(exc_sp >= &exc_stack[0]);
                        currently_in_except_block = (exc_sp[0] & 1); // restore previous state
                        exc_sp -= 2; // pop back to previous exception handler
                        break;

                    // matched against: SETUP_EXCEPT
                    case MP_BC_POP_EXCEPT:
                        // TODO need to work out how blocks work etc
                        // pops block, checks it's an exception block, and restores the stack, saving the 3 exception values to local threadstate
                        assert(exc_sp >= &exc_stack[0]);
                        //sp = (mp_obj_t*)(*exc_sp--);
                        //exc_sp--; // discard ip
                        currently_in_except_block = (exc_sp[0] & 1); // restore previous state
                        exc_sp -= 2; // pop back to previous exception handler
                        //sp -= 3; // pop 3 exception values
                        break;

                    case MP_BC_UNARY_OP:
                        unum = *ip++;
                        SET_TOP(rt_unary_op(unum, TOP()));
                        break;

                    case MP_BC_BINARY_OP:
                        unum = *ip++;
                        obj2 = POP();
                        obj1 = TOP();
                        SET_TOP(rt_binary_op(unum, obj1, obj2));
                        break;

                    case MP_BC_BUILD_TUPLE:
                        DECODE_UINT;
                        sp -= unum - 1;
                        SET_TOP(rt_build_tuple(unum, sp));
                        break;

                    case MP_BC_BUILD_LIST:
                        DECODE_UINT;
                        sp -= unum - 1;
                        SET_TOP(rt_build_list(unum, sp));
                        break;

                    case MP_BC_LIST_APPEND:
                        DECODE_UINT;
                        // I think it's guaranteed by the compiler that sp[unum] is a list
                        rt_list_append(sp[-unum], sp[0]);
                        sp--;
                        break;

                    case MP_BC_BUILD_MAP:
                        DECODE_UINT;
                        PUSH(rt_build_map(unum));
                        break;

                    case MP_BC_STORE_MAP:
                        sp -= 2;
                        rt_store_map(sp[0], sp[2], sp[1]);
                        break;

                    case MP_BC_MAP_ADD:
                        DECODE_UINT;
                        // I think it's guaranteed by the compiler that sp[-unum - 1] is a map
                        rt_store_map(sp[-unum - 1], sp[0], sp[-1]);
                        sp -= 2;
                        break;

                    case MP_BC_BUILD_SET:
                        DECODE_UINT;
                        sp -= unum - 1;
                        SET_TOP(rt_build_set(unum, sp));
                        break;

                    case MP_BC_SET_ADD:
                        DECODE_UINT;
                        // I think it's guaranteed by the compiler that sp[-unum] is a set
                        rt_store_set(sp[-unum], sp[0]);
                        sp--;
                        break;

#if MICROPY_ENABLE_SLICE
                    case MP_BC_BUILD_SLICE:
                        DECODE_UINT;
                        if (unum == 2) {
                            obj2 = POP();
                            obj1 = TOP();
                            SET_TOP(mp_obj_new_slice(obj1, obj2, NULL));
                        } else {
                            printf("3-argument slice is not supported\n");
                            assert(0);
                        }
                        break;
#endif

                    case MP_BC_UNPACK_SEQUENCE:
                        DECODE_UINT;
                        rt_unpack_sequence(sp[0], unum, sp);
                        sp += unum - 1;
                        break;

                    case MP_BC_MAKE_FUNCTION:
                        DECODE_UINT;
                        PUSH(rt_make_function_from_id(unum));
                        break;

                    case MP_BC_MAKE_CLOSURE:
                        DECODE_UINT;
                        obj1 = POP();
                        PUSH(rt_make_closure_from_id(unum, obj1));
                        break;

                    case MP_BC_CALL_FUNCTION:
                        DECODE_UINT;
                        // unum & 0xff == n_positional
                        // (unum >> 8) & 0xff == n_keyword
                        sp -= (unum & 0xff) + ((unum >> 7) & 0x1fe);
                        SET_TOP(rt_call_function_n_kw(*sp, unum & 0xff, (unum >> 8) & 0xff, sp + 1));
                        break;

                    case MP_BC_CALL_METHOD:
                        DECODE_UINT;
                        // unum & 0xff == n_positional
                        // (unum >> 8) & 0xff == n_keyword
                        sp -= (unum & 0xff) + ((unum >> 7) & 0x1fe) + 1;
                        SET_TOP(rt_call_method_n_kw(unum & 0xff, (unum >> 8) & 0xff, sp));
                        break;

                    case MP_BC_RETURN_VALUE:
                        nlr_pop();
                        *sp_in_out = sp;
                        assert(exc_sp == &exc_stack[0] - 1);
                        return false;

                    case MP_BC_RAISE_VARARGS:
                        unum = *ip++;
                        assert(unum == 1);
                        obj1 = POP();
                        nlr_jump(obj1);

                    case MP_BC_YIELD_VALUE:
                        nlr_pop();
                        *ip_in_out = ip;
                        fastn[0] = fast0;
                        fastn[-1] = fast1;
                        fastn[-2] = fast2;
                        *sp_in_out = sp;
                        return true;

                    case MP_BC_IMPORT_NAME:
                        DECODE_QSTR;
                        obj1 = POP();
                        SET_TOP(rt_import_name(qst, obj1, TOP()));
                        break;

                    case MP_BC_IMPORT_FROM:
                        DECODE_QSTR;
                        obj1 = rt_import_from(TOP(), qst);
                        PUSH(obj1);
                        break;

                    default:
                        printf("code %p, byte code 0x%02x not implemented\n", ip, op);
                        assert(0);
                        nlr_pop();
                        return false;
                }
            }

        } else {
            // exception occurred

            // set file and line number that the exception occurred at
            if (MP_OBJ_IS_TYPE(nlr.ret_val, &exception_type)) {
                machine_uint_t code_info_size = code_info[0] | (code_info[1] << 8) | (code_info[2] << 16) | (code_info[3] << 24);
                qstr source_file = code_info[4] | (code_info[5] << 8) | (code_info[6] << 16) | (code_info[7] << 24);
                qstr block_name = code_info[8] | (code_info[9] << 8) | (code_info[10] << 16) | (code_info[11] << 24);
                machine_uint_t source_line = 1;
                machine_uint_t bc = save_ip - code_info - code_info_size;
                //printf("find %lu %d %d\n", bc, code_info[12], code_info[13]);
                for (const byte* ci = code_info + 12; *ci && bc >= ((*ci) & 31); ci++) {
                    bc -= *ci & 31;
                    source_line += *ci >> 5;
                }
                mp_obj_exception_add_traceback(nlr.ret_val, source_file, source_line, block_name);
            }

            while (currently_in_except_block) {
                // nested exception

                assert(exc_sp >= &exc_stack[0]);

                // TODO make a proper message for nested exception
                // at the moment we are just raising the very last exception (the one that caused the nested exception)

                // move up to previous exception handler
                currently_in_except_block = (exc_sp[0] & 1); // restore previous state
                exc_sp -= 2; // pop back to previous exception handler
            }

            if (exc_sp >= &exc_stack[0]) {
                // set flag to indicate that we are now handling an exception
                currently_in_except_block = 1;

                // catch exception and pass to byte code
                sp = (mp_obj_t*)(exc_sp[0] & (~((machine_uint_t)1)));
                ip = (const byte*)(exc_sp[-1]);
                // push(traceback, exc-val, exc-type)
                PUSH(mp_const_none);
                PUSH(nlr.ret_val);
                PUSH(nlr.ret_val); // TODO should be type(nlr.ret_val), I think...

            } else {
                // re-raise exception to higher level
                // TODO what to do if this is a generator??
                nlr_jump(nlr.ret_val);
            }
        }
    }
}
