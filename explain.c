/*
  +----------------------------------------------------------------------+
  | PHP Version 7                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2015 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author:                                                              |
  +----------------------------------------------------------------------+
*/

/* $Id$ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "php_main.h"
#include "ext/standard/info.h"
#include "php_explain.h"

typedef struct _explain_opcode_t {
    const char *name;
    size_t  name_len;
    zend_uchar opcode;
} explain_opcode_t;

#define EXPLAIN_FILE   0x00000001
#define EXPLAIN_STRING 0x00000010
#define EXPLAIN_OPLINE 0x00000011

#define EXPLAIN_OPCODE_NAME(c) \
	{#c, sizeof(#c)-1, c}

#include "explain_opcodes.h"

#if ZEND_USE_ABS_JMP_ADDR
# define JMP_LINE(node, base_address)  (int32_t)(((long)((node).jmp_addr) - (long)(base_address)) / sizeof(zend_op))
#else
# define JMP_LINE(node, opline)  (int32_t)(((int32_t)((node).jmp_offset) / sizeof(zend_op)) + (opline))
#endif

ZEND_DECLARE_MODULE_GLOBALS(explain);

static inline void explain_opcode(long opcode, zval *return_value) { /* {{{ */

    if (opcode >= sizeof(opcodes)) {
        ZVAL_STRINGL(return_value, "unknown", strlen("unknown"));
    }

    explain_opcode_t decode = opcodes[opcode];

    if (decode.opcode == opcode) {
        ZVAL_STRINGL(return_value, decode.name, decode.name_len);
    } else {
        ZVAL_STRINGL(return_value, "unknown", strlen("unknown"));
    }
} /* }}} */

/* True global resources - no need for thread safety here */
static int le_explain;

/* {{{ PHP_INI
 */
/* Remove comments and fill if you need to have entries in php.ini
PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("explain.global_value",      "42", PHP_INI_ALL, OnUpdateLong, global_value, zend_explain_globals, explain_globals)
    STD_PHP_INI_ENTRY("explain.global_string", "foobar", PHP_INI_ALL, OnUpdateString, global_string, zend_explain_globals, explain_globals)
PHP_INI_END()
*/
/* }}} */

static int explain_variable(zend_ulong var, zend_llist *vars) { /* {{{ */
    zend_llist_position position;
    zend_ulong *cache = zend_llist_get_first_ex(vars, &position);
    zend_ulong id = 0;

    while (cache) {
        if (*cache == var) {
            return id;
        }
        cache = zend_llist_get_next_ex(vars, &position);
        id++;
    }

    zend_llist_add_element(vars, &var);

    return id;
} /* }}} */

static inline void explain_zend_op(zend_op_array *ops, znode_op *op, zend_ulong type, const char *name, size_t name_len, zend_llist *vars, zval *return_value_ptr) { /* {{{ */
    if (!op || type == IS_UNUSED)
        return;

    switch (type) {
        case IS_CV : {
            add_assoc_str_ex(return_value_ptr, name, name_len, ops->vars[(op->var - sizeof(zend_execute_data)) / sizeof(zval)]);
            break;
        }

        case IS_VAR:
        case IS_TMP_VAR: {
            /* convert this to a human friendly number */
            add_assoc_long_ex(return_value_ptr, name, name_len, explain_variable((zend_ulong) ops->vars - op->var, vars));
            break;
        }

        case IS_CONST : {
            zval pzval = *RT_CONSTANT_EX(ops->literals, *op);
            add_assoc_zval_ex(return_value_ptr, name, name_len, &pzval);
            break;
        }

    }
} /* }}} */

static inline void explain_optype(uint32_t type, zval *return_value_ptr) { /* {{{ */
    switch (type) {
        case IS_CV:
            ZVAL_STRINGL(return_value_ptr, "IS_CV", strlen("IS_CV"));
            break;
        case IS_TMP_VAR:
            ZVAL_STRINGL(return_value_ptr, "IS_TMP_VAR", strlen("IS_TMP_VAR"));
            break;
        case IS_VAR:
            ZVAL_STRINGL(return_value_ptr, "IS_VAR", strlen("IS_VAR"));
            break;
        case IS_CONST:
            ZVAL_STRINGL(return_value_ptr, "IS_CONST", strlen("IS_CONST"));
            break;
        case IS_UNUSED:
            ZVAL_STRINGL(return_value_ptr, "IS_UNUSED", strlen("IS_UNUSED"));
            break;
            /* special case for jmp's */
        case EXPLAIN_OPLINE:
            ZVAL_STRINGL(return_value_ptr, "IS_OPLINE", strlen("IS_OPLINE"));
            break;

        default: if (type & EXT_TYPE_UNUSED) {
                switch (type &~ EXT_TYPE_UNUSED) {
                    case IS_CV:
                        ZVAL_STRINGL(return_value_ptr, "IS_CV|EXT_TYPE_UNUSED", strlen("IS_CV|EXT_TYPE_UNUSED"));
                        break;
                    case IS_TMP_VAR:
                        ZVAL_STRINGL(return_value_ptr, "IS_TMP_VAR|EXT_TYPE_UNUSED", strlen("IS_TMP_VAR|EXT_TYPE_UNUSED"));
                        break;
                    case IS_VAR:
                        ZVAL_STRINGL(return_value_ptr, "IS_VAR|EXT_TYPE_UNUSED", strlen("IS_VAR|EXT_TYPE_UNUSED"));
                        break;
                    case IS_CONST:
                        ZVAL_STRINGL(return_value_ptr, "IS_CONST|EXT_TYPE_UNUSED", strlen("IS_CONST|EXT_TYPE_UNUSED"));
                        break;
                    case IS_UNUSED:
                        ZVAL_STRINGL(return_value_ptr, "IS_UNUSED|EXT_TYPE_UNUSED", strlen("IS_UNUSED|EXT_TYPE_UNUSED"));
                        break;
                }
            } else {
                ZVAL_STRINGL(return_value_ptr, "unknown", strlen("unknown"));
            }
    }
} /* }}} */

static inline void explain_op_array(zend_op_array *ops, zval *result) {
    if (ops) {
        zend_ulong  next = 0;
        zend_llist vars;
        zend_llist_init(&vars, sizeof(size_t), NULL, 0);

        array_init(result);

        do {
            zval zopline;
            array_init(&zopline);

            zend_op *opline = &ops->opcodes[next];

            add_assoc_long_ex(&zopline, "opline", sizeof("opline") - 1, next);
            add_assoc_long_ex(&zopline, "opcode", sizeof("opcode") - 1, opline->opcode);

            switch (opline->opcode) {
                case ZEND_JMP:
#ifdef ZEND_GOTO
                case ZEND_GOTO:
#endif
#ifdef ZEND_FAST_CALL
                case ZEND_FAST_CALL:
#endif
                add_assoc_long_ex(&zopline, "op1_type", sizeof("op1_type") - 1, EXPLAIN_OPLINE);

#if ZEND_USE_ABS_JMP_ADDR
                zend_op *base_address = &(opa->opcodes[0]);
                add_assoc_long_ex(&zopline, "op1", sizeof("op1") - 1, JMP_LINE(opline->op1, base_address));
#else
                add_assoc_long_ex(&zopline, "op1", sizeof("op1") - 1, JMP_LINE(opline->op1, next));
#endif

                break;

                case ZEND_JMPZNZ:
                    add_assoc_long_ex(&zopline, "op1_type", sizeof("op1_type") - 1, opline->op1_type);
                    explain_zend_op(ops, &opline->op1, opline->op1_type, "op1", sizeof("op1") - 1, &vars, &zopline);

                    /* TODO(krakjoe) needs opline->extended_value on true, opline_num on false */
                    add_assoc_long_ex(&zopline, "op2_type", sizeof("op2_type") - 1, EXPLAIN_OPLINE);
                    add_assoc_long_ex(&zopline, "op2", sizeof("op2") - 1, opline->op2.opline_num);

                    add_assoc_long_ex(&zopline, "result_type", sizeof("result_type") - 1, opline->result_type);
                    explain_zend_op(ops, &opline->result, opline->result_type, "result", sizeof("result") - 1, &vars, &zopline);
                    break;

                case ZEND_JMPZ:
                case ZEND_JMPNZ:
                case ZEND_JMPZ_EX:
                case ZEND_JMPNZ_EX:

#ifdef ZEND_JMP_SET
                case ZEND_JMP_SET:
#endif
#ifdef ZEND_JMP_SET_VAR
                case ZEND_JMP_SET_VAR:
#endif
                    add_assoc_long_ex(&zopline, "op1_type", sizeof("op1_type") - 1, opline->op1_type);
                    explain_zend_op(ops, &opline->op1, opline->op1_type, "op1", sizeof("op1") - 1, &vars, &zopline);

                    add_assoc_long_ex(&zopline, "op2_type", sizeof("op2_type") - 1, EXPLAIN_OPLINE);
#if ZEND_USE_ABS_JMP_ADDR
                    zend_op *base_address = &(opa->opcodes[0]);
                    add_assoc_long_ex(&zopline, "op2", sizeof("op2") - 1, JMP_LINE(opline->op2, base_address));
#else
                    add_assoc_long_ex(&zopline, "op2", sizeof("op2") - 1, JMP_LINE(opline->op2, next));
#endif
                    add_assoc_long_ex(&zopline, "result_type", sizeof("result_type") - 1, opline->result_type);

                    explain_zend_op(ops, &opline->result, opline->result_type, "result", sizeof("result") - 1, &vars, &zopline);
                    break;

                case ZEND_RECV_INIT:
                    add_assoc_long_ex(&zopline, "result_type", sizeof("result_type") - 1, opline->result_type);
                    explain_zend_op(ops, &opline->result, opline->result_type, "result", sizeof("result") - 1, &vars, &zopline);
                    break;

                default: {
                    add_assoc_long_ex(&zopline, "op1_type", sizeof("op1_type") - 1, opline->op1_type);
                    explain_zend_op(ops, &opline->op1, opline->op1_type, "op1", sizeof("op1") - 1, &vars, &zopline);

                    add_assoc_long_ex(&zopline, "op2_type", sizeof("op2_type") - 1, opline->op2_type);
                    explain_zend_op(ops, &opline->op2, opline->op2_type, "op2", sizeof("op2") - 1, &vars, &zopline);

                    add_assoc_long_ex(&zopline, "result_type", sizeof("result_type") - 1, opline->result_type);
                    explain_zend_op(ops, &opline->result, opline->result_type, "result", sizeof("result") - 1, &vars, &zopline);
                }
            }

            if (opline->extended_value) {
                add_assoc_long_ex(&zopline, "extended_value", sizeof("extended_value") - 1, opline->extended_value);
            }

            add_assoc_long_ex(&zopline, "lineno", sizeof("lineno") - 1, opline->lineno);

            add_next_index_zval(result, &zopline);
        } while (++next < ops->last);

        zend_llist_destroy(&vars);
    } else {
        ZVAL_NULL(result);
    }
}

static inline void explain_create_caches(HashTable *classes, HashTable *functions) { /* {{{ */
    zend_function tf;
    zend_class_entry *te;

    zend_hash_init(classes, zend_hash_num_elements(classes), NULL, NULL, 0);
    zend_hash_copy(classes, CG(class_table), NULL);

    zend_hash_init(functions, zend_hash_num_elements(functions), NULL, NULL, 0);
    zend_hash_copy(functions, CG(function_table), NULL);
} /* }}} */

static inline void explain_destroy_caches(HashTable *classes, HashTable *functions) { /* {{{ */
    zend_hash_destroy(classes);
    zend_hash_destroy(functions);
} /* }}} */

/* Every user-visible function in PHP should document itself in the source */
/* {{{ proto string confirm_explain_compiled(string arg)
   Return a string to confirm that the module is compiled in */
PHP_FUNCTION(explain)
{
    zval *code, *classes, *functions, res;
    zend_ulong options = EXPLAIN_FILE;
    HashTable caches[2] = {0, 0};

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "z|lzz", &code, &options, &classes, &functions) == FAILURE) {
        return;
    }

    zend_file_handle fh;
    zend_op_array *ops = NULL;

    if (options & EXPLAIN_FILE) {
        if (php_stream_open_for_zend_ex(Z_STRVAL_P(code), &fh, USE_PATH|STREAM_OPEN_FOR_INCLUDE) == SUCCESS) {
            explain_create_caches(&caches[0], &caches[1]);
            ops = zend_compile_file(&fh, ZEND_INCLUDE);
            zend_destroy_file_handle(&fh);
        } else {
            zend_error(E_WARNING, "file %s couldn't be opened", Z_STRVAL_P(code));
            RETURN_FALSE;
        }
    } else if (options & EXPLAIN_STRING) {
        explain_create_caches(&caches[0], &caches[1]);
        ops = zend_compile_string(code, "explained");
        if (!ops) {
            explain_destroy_caches(&caches[0], &caches[1]);
        }
    } else {
        zend_error(E_WARNING, "invalid options passed to explain (%lu), please see documentation", options);
        RETURN_FALSE;
    }

    if (!ops) {
        explain_destroy_caches(&caches[0], &caches[1]);
        zend_error(E_WARNING, "explain was unable to compile code");
        RETURN_FALSE;
    }

    explain_op_array(ops, &res);

    if (ZVAL_IS_NULL(&res)) {
        explain_destroy_caches(&caches[0], &caches[1]);
        zend_error(E_WARNING, "explain was unable to compile code");
        RETURN_FALSE;
    }

    if (classes) {
        zend_ulong pce_num_key;
        zend_string *pce_name;
        zend_class_entry *pce;

        if (Z_TYPE_P(classes) != IS_ARRAY) {
            array_init(classes);
        }

        ZEND_HASH_FOREACH_KEY_VAL(CG(class_table), pce_num_key, pce_name, pce)
        {
            if (pce->type == ZEND_USER_CLASS && !zend_hash_exists(&caches[0], pce_name)) {
                zval zce;
                zend_function *pfe;

                array_init(&zce);

                ZEND_HASH_FOREACH_PTR(&pce->function_table, pfe) {
                    if (pfe->common.type == ZEND_USER_FUNCTION) {
                        zval zfe;

                        array_init(&zfe);
                        explain_op_array(&pfe->op_array, &zfe);

                        add_assoc_zval_ex(&zce, pfe->common.function_name, strlen(pfe->common.function_name), &zfe);
                    }
                } ZEND_HASH_FOREACH_END();

                add_assoc_zval_ex(classes, pce->name, strlen(pce->name), &zce);
            }

        }ZEND_HASH_FOREACH_END();
    }

    if (functions) {
        zend_ulong pfe_num_key;
        zend_function *pfe;
        zend_string *fe_name;

        if (Z_TYPE_P(functions) != IS_ARRAY) {
            array_init(functions);
        }

        ZEND_HASH_FOREACH_KEY_VAL(CG(function_table), pfe_num_key, fe_name, pfe) {
            if (pfe->common.type == ZEND_USER_FUNCTION && !zend_hash_exists(&caches[1], fe_name)) {
                zval zfe;

                array_init(&zfe);
                explain_op_array(&pfe->op_array, &zfe);

                add_assoc_zval_ex(functions, fe_name, strlen(fe_name), &zfe);
            }
        } ZEND_HASH_FOREACH_END();
    }

    explain_destroy_caches(&caches[0], &caches[1]);

    RETURN_ZVAL(&res, 0, 0);

}
/* }}} */

/* {{{ proto string explain_opcode(integer opcode)
    get the friendly name for an opcode */
PHP_FUNCTION(explain_opcode) {
    long opcode;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l", &opcode) == FAILURE) {
        return;
    }

    explain_opcode(opcode, return_value);
} /* }}} */

/* {{{ proto string explain_optype(integer optype)
    get the friendly name for an optype */
PHP_FUNCTION(explain_optype) {
    long optype;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l", &optype) == FAILURE) {
        return;
    }

    explain_optype((zend_uchar)optype, return_value);
} /* }}} */

/* {{{ */
static inline void php_explain_globals_ctor(zend_explain_globals *eg) {} /* }}} */

static inline void php_explain_destroy_ops(zend_op_array **ops) { /* {{{ */
    if ((*ops)) {
        destroy_op_array(*ops);
        efree(*ops);
    }
} /* }}} */

/* {{{ PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(explain)
{
    ZEND_INIT_MODULE_GLOBALS(explain, php_explain_globals_ctor, NULL);

    REGISTER_LONG_CONSTANT("EXPLAIN_STRING",          EXPLAIN_STRING,      CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("EXPLAIN_FILE",            EXPLAIN_FILE,        CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("EXPLAIN_OPLINE",          EXPLAIN_OPLINE,      CONST_CS | CONST_PERSISTENT);

    REGISTER_LONG_CONSTANT("EXPLAIN_IS_UNUSED",       IS_UNUSED,           CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("EXPLAIN_IS_VAR",          IS_VAR,              CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("EXPLAIN_IS_TMP_VAR",      IS_TMP_VAR,          CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("EXPLAIN_IS_CV",           IS_CV,               CONST_CS | CONST_PERSISTENT);
#ifdef EXT_TYPE_UNUSED
    REGISTER_LONG_CONSTANT("EXPLAIN_EXT_TYPE_UNUSED", EXT_TYPE_UNUSED,     CONST_CS | CONST_PERSISTENT);
#endif

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(explain)
{
	/* uncomment this line if you have INI entries
	UNREGISTER_INI_ENTRIES();
	*/
	return SUCCESS;
}
/* }}} */

/* Remove if there's nothing to do at request start */
/* {{{ PHP_RINIT_FUNCTION
 */
PHP_RINIT_FUNCTION(explain)
{
#if defined(COMPILE_DL_EXPLAIN) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif

    zend_hash_init(&EX_G(explained), 8, NULL, (dtor_func_t) php_explain_destroy_ops, 0);
    zend_hash_init(&EX_G(zval_cache), 8, NULL, (dtor_func_t) ZVAL_PTR_DTOR, 0);

	return SUCCESS;
}
/* }}} */

/* Remove if there's nothing to do at request end */
/* {{{ PHP_RSHUTDOWN_FUNCTION
 */
PHP_RSHUTDOWN_FUNCTION(explain)
{
    zend_hash_destroy(&EX_G(explained));
    zend_hash_destroy(&EX_G(zval_cache));

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(explain)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "explain support", "enabled");
	php_info_print_table_end();

	/* Remove comments if you have entries in php.ini
	DISPLAY_INI_ENTRIES();
	*/
}
/* }}} */

ZEND_BEGIN_ARG_INFO_EX(arginfo_explain, 0, 0, 1)
                ZEND_ARG_INFO(0, code)
                ZEND_ARG_INFO(0, options)
                ZEND_ARG_INFO(1, classes)
                ZEND_ARG_INFO(1, functions)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_explain_opcode, 0, 0, 1)
                ZEND_ARG_INFO(0, opcode)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_explain_optype, 0, 0, 1)
                ZEND_ARG_INFO(0, optype)
ZEND_END_ARG_INFO()

/* {{{ explain_functions[]
 *
 * Every user visible function must have an entry in explain_functions[].
 */
const zend_function_entry explain_functions[] = {
	PHP_FE(explain,	NULL)
    PHP_FE(explain_opcode, arginfo_explain_opcode)
    PHP_FE(explain_optype, arginfo_explain_optype)
	PHP_FE_END	/* Must be the last line in explain_functions[] */
};
/* }}} */

/* {{{ explain_module_entry
 */
zend_module_entry explain_module_entry = {
	STANDARD_MODULE_HEADER,
	"explain",
	explain_functions,
	PHP_MINIT(explain),
	PHP_MSHUTDOWN(explain),
	PHP_RINIT(explain),		/* Replace with NULL if there's nothing to do at request start */
	PHP_RSHUTDOWN(explain),	/* Replace with NULL if there's nothing to do at request end */
	PHP_MINFO(explain),
	PHP_EXPLAIN_VERSION,
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_EXPLAIN
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE();
#endif
ZEND_GET_MODULE(explain)
#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
