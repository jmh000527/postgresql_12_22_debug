/*-------------------------------------------------------------------------
 *
 * fe_memutils.c
 *	  memory management support for frontend code
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/common/fe_memutils.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#error "This file is not expected to be compiled for backend code"
#endif

#include "postgres_fe.h"

/*
 * 内部内存分配函数，支持额外标志。
 * size: 分配的字节数
 * flags: 分配标志（如是否初始化为零、是否允许 OOM）
 */
static inline void *
pg_malloc_internal(size_t size, int flags)
{
	void	   *tmp;

	/* 避免 malloc(0) 的不可移植行为 */
	if (size == 0)
		size = 1;
	tmp = malloc(size);
	if (tmp == NULL)
	{
		/* 如果未设置 MCXT_ALLOC_NO_OOM，则内存不足时退出 */
		if ((flags & MCXT_ALLOC_NO_OOM) == 0)
		{
			fprintf(stderr, _("out of memory\n"));
			exit(EXIT_FAILURE);
		}
		return NULL;
	}

	/* 如果设置了 MCXT_ALLOC_ZERO，则将内存初始化为零 */
	if ((flags & MCXT_ALLOC_ZERO) != 0)
		MemSet(tmp, 0, size);
	return tmp;
}

/*
 * 分配指定大小的内存
 */
void *
pg_malloc(size_t size)
{
	return pg_malloc_internal(size, 0);
}

/*
 * 分配并初始化为零的内存
 */
void *
pg_malloc0(size_t size)
{
	return pg_malloc_internal(size, MCXT_ALLOC_ZERO);
}

/*
 * 支持额外标志的内存分配
 */
void *
pg_malloc_extended(size_t size, int flags)
{
	return pg_malloc_internal(size, flags);
}

/*
 * 重新分配内存
 * ptr: 原始指针
 * size: 新的大小
 */
void *
pg_realloc(void *ptr, size_t size)
{
	void	   *tmp;

	/* 避免 realloc(NULL, 0) 的不可移植行为 */
	if (ptr == NULL && size == 0)
		size = 1;
	tmp = realloc(ptr, size);
	if (!tmp)
	{
		fprintf(stderr, _("out of memory\n"));
		exit(EXIT_FAILURE);
	}
	return tmp;
}

/*
 * "Safe" wrapper around strdup().
 */
char *
pg_strdup(const char *in)
{
	char	   *tmp;

	if (!in)
	{
		fprintf(stderr,
				_("cannot duplicate null pointer (internal error)\n"));
		exit(EXIT_FAILURE);
	}
	tmp = strdup(in);
	if (!tmp)
	{
		fprintf(stderr, _("out of memory\n"));
		exit(EXIT_FAILURE);
	}
	return tmp;
}

void
pg_free(void *ptr)
{
	if (ptr != NULL)
		free(ptr);
}

/*
 * 后端内存管理函数的前端模拟。用于编译后端文件的程序。
 */
void *
palloc(Size size)
{
	// 分配指定大小的内存，类似后端的 palloc
	return pg_malloc_internal(size, 0);
}

void *
palloc0(Size size)
{
	// 分配并初始化为零的内存，类似后端的 palloc0
	return pg_malloc_internal(size, MCXT_ALLOC_ZERO);
}

void *
palloc_extended(Size size, int flags)
{
	// 分配内存并支持额外标志，类似后端的 palloc_extended
	return pg_malloc_internal(size, flags);
}

void
pfree(void *pointer)
{
	// 释放内存，类似后端的 pfree
	pg_free(pointer);
}

char *
pstrdup(const char *in)
{
	// 复制字符串，类似后端的 pstrdup
	return pg_strdup(in);
}

void *
repalloc(void *pointer, Size size)
{
	// 重新分配内存，类似后端的 repalloc
	return pg_realloc(pointer, size);
}
