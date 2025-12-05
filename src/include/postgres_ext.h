/*-------------------------------------------------------------------------
 *
 * postgres_ext.h
 *
 *   该文件包含在 PostgreSQL 中处处可见且对前端接口库客户端可见的声明。
 *   例如，Oid 类型是 libpq 等库的 API 的一部分。
 *
 *   针对特定接口的声明应放在该接口的头文件中（例如 libpq-fe.h）。
 *   本文件仅用于基本的 Postgres 声明。
 *
 *   用户编写的 C 函数不属于“外部于 Postgres”的范畴。
 *   这些函数相当于对后端的本地修改，并使用通常为 Postgres 内部使用的头文件
 *   与后端进行交互。
 *
 * src/include/postgres_ext.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef POSTGRES_EXT_H
#define POSTGRES_EXT_H

#include "pg_config_ext.h"

 /*
  * 对象 ID (Object ID) 是 Postgres 中的基本类型。
  */
typedef unsigned int Oid;

#ifdef __cplusplus
#define InvalidOid		(Oid(0))
#else
#define InvalidOid		((Oid) 0)
#endif

#define OID_MAX  UINT_MAX
/* 要使用上面的 #define，需要包含 <limits.h> */

#define atooid(x) ((Oid) strtoul((x), NULL, 10))
/* 要使用上述的 atooid 宏，需要包含 <stdlib.h> */


/* 定义用于客户端 API 声明的带符号 64 位整数类型。 */
typedef PG_INT64_TYPE pg_int64;


/*
 * 错误信息字段的标识符。放在此处以在前端和后端之间保持一致，
 * 并将它们导出给 libpq 应用程序。
 */
#define PG_DIAG_SEVERITY		'S'
#define PG_DIAG_SEVERITY_NONLOCALIZED 'V'
#define PG_DIAG_SQLSTATE		'C'
#define PG_DIAG_MESSAGE_PRIMARY 'M'
#define PG_DIAG_MESSAGE_DETAIL	'D'
#define PG_DIAG_MESSAGE_HINT	'H'
#define PG_DIAG_STATEMENT_POSITION 'P'
#define PG_DIAG_INTERNAL_POSITION 'p'
#define PG_DIAG_INTERNAL_QUERY	'q'
#define PG_DIAG_CONTEXT			'W'
#define PG_DIAG_SCHEMA_NAME		's'
#define PG_DIAG_TABLE_NAME		't'
#define PG_DIAG_COLUMN_NAME		'c'
#define PG_DIAG_DATATYPE_NAME	'd'
#define PG_DIAG_CONSTRAINT_NAME 'n'
#define PG_DIAG_SOURCE_FILE		'F'
#define PG_DIAG_SOURCE_LINE		'L'
#define PG_DIAG_SOURCE_FUNCTION 'R'

#endif							/* POSTGRES_EXT_H */
